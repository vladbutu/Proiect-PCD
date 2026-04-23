/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Server TCP cu poll() pentru protocolul video
 * Ce face programul:
 * - deschide un socket TCP si face listen pe portul din configurare
 * - foloseste poll() pt I/O multiplexing (cerinta Nivel A; evit limita FD_SETSIZE de la select())
 * - accepta clienti noi si pastreaza conexiunile deschise pt cereri multiple
 * - dispatch fiecare operatie video (trim/filter/merge/mixaudio) catre un worker fork()-at
 * - asteapta raspunsul workerului si trimite statusul inapoi la client
 * - face reap pe procesele copil terminate (waitpid cu WNOHANG) pt a evita zombii
 * Erori tratate explicit:
 * - socket/setsockopt/bind/listen/accept esuate
 * - poll() intrerupt de semnal (EINTR -> reincerc)
 * - alocare memorie esuata pt vectorul de pollfd
 * - client deconectat sau cerere invalida
 * - SIGPIPE ignorat (clientul poate inchide brut conexiunea)
 * - SIGINT/SIGTERM -> shutdown curat
 */

// activeaza API-uri POSIX in headerele de sistem
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L // nivel POSIX tinta pentru proiect
#endif

#define LISTEN_BACKLOG 16 // cati clienti pot sta in coada de accept
#define POLL_TIMEOUT_MS 1000 // timeout poll() in ms (verific periodic semnale + zombii)

// stari pe care le punem in proto_status_t.state (int32 semnat)
#define TASK_STATE_DONE 2 // task terminat cu succes
#define TASK_STATE_ERR (-1) // task esuat

#include "net_server.h" // server_cfg_t + declaratia net_server_run()
#include "proto.h" // protocolul binar (header, payload-uri, framing)
#include "worker.h" // worker_spawn/wait/dispatch_merge -- fork()+execv()
#include "video_ops.h" // video_ctx_t + argv builders pt ffmpeg

#include <errno.h> // errno, EINTR pt retry dupa semnale
#include <netinet/in.h> // sockaddr_in, htons, htonl pt adrese IPv4
#include <signal.h> // sigaction, sig_atomic_t pt handler semnale
#include <stdint.h> // uint16_t, uint32_t pt campuri protocol
#include <stdio.h> // fprintf, snprintf pt output/erori
#include <stdlib.h> // calloc, free pt vectorul de pollfd
#include <string.h> // memset pt initializare structuri
#include <sys/poll.h> // poll(), struct pollfd, POLLIN/POLLERR
#include <sys/socket.h> // socket, bind, listen, accept, setsockopt
#include <sys/types.h> // pid_t
#include <sys/wait.h> // waitpid, WNOHANG pt reap zombii
#include <unistd.h> // close, getpid

static volatile sig_atomic_t g_stop = 0; // flag global pt shutdown curat

// handler pt SIGINT/SIGTERM, setez flagul si ies din poll()
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// curata procesele copil terminate ca sa nu raman zombii
// waitpid cu WNOHANG = nu blocheaza daca nu e nimeni terminat
static void reap_children(void)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // am recuperat statusul dar nu il folosesc pt nimic
        (void)pid;
        (void)status;
    }
}

// deschide socketul de listen pe portul dat
static int open_listener(int port)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        return -1; // socket() esuat
    }

    // SO_REUSEADDR ca sa pot reporni serverul rapid fara "address already in use"
    int yes = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        (void)close(sfd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); // initializez complet (core.uninitialized)
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port); // network byte order
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // ascult pe toate interfetele

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(sfd);
        return -1;
    }
    if (listen(sfd, LISTEN_BACKLOG) < 0) {
        (void)close(sfd);
        return -1;
    }
    return sfd;
}

// trimite un raspuns de status catre client (rcode=0 -> done, altfel -> eroare)
static int write_status_reply(int sock, const proto_header_t *hdr, int rcode, const char *result_path)
{
    proto_header_t reply = *hdr;
    reply.msg_size = (uint32_t)(sizeof(reply) + sizeof(proto_status_t));

    proto_status_t sts;
    memset(&sts, 0, sizeof(sts));
    sts.task_id = htonl(hdr->task_id);
    sts.state = (int32_t)htonl((uint32_t)(rcode == 0 ? TASK_STATE_DONE : TASK_STATE_ERR));
    (void)snprintf(sts.result_path, sizeof(sts.result_path), "%s", result_path != NULL ? result_path : "");

    if (proto_write_header(sock, &reply) < 0) {
        return -1;
    }
    return proto_write_full(sock, &sts, sizeof(sts));
}

// handler TRIM: citesc payload-ul, construiesc argv-ul pt ffmpeg, fork + execv
static int handle_trim(int sock, const video_ctx_t *vctx, const proto_header_t *hdr)
{
    proto_trim_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    char **argv = video_build_trim_argv(vctx, &req);
    if (argv == NULL) {
        return -1;
    }
    pid_t pid = worker_spawn(vctx, argv);
    int rcode = (pid > 0) ? worker_wait(pid) : -1;
    video_argv_free(argv);
    return write_status_reply(sock, hdr, rcode, req.output);
}

// handler FILTER: aplica un filtru video via ffmpeg
static int handle_filter(int sock, const video_ctx_t *vctx, const proto_header_t *hdr)
{
    proto_filter_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    char **argv = video_build_filter_argv(vctx, &req);
    if (argv == NULL) {
        return -1;
    }
    pid_t pid = worker_spawn(vctx, argv);
    int rcode = (pid > 0) ? worker_wait(pid) : -1;
    video_argv_free(argv);
    return write_status_reply(sock, hdr, rcode, req.output);
}

// handler MERGE: concatenare clipuri cu paralelism (vezi worker_dispatch_merge)
static int handle_merge(int sock, const video_ctx_t *vctx, const proto_header_t *hdr)
{
    proto_merge_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    int rcode = worker_dispatch_merge(vctx, &req);
    return write_status_reply(sock, hdr, rcode, req.output);
}

// handler MIXAUDIO: suprapune audio pe video
static int handle_mixaudio(int sock, const video_ctx_t *vctx, const proto_header_t *hdr)
{
    proto_mixaudio_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    char **argv = video_build_mixaudio_argv(vctx, &req);
    if (argv == NULL) {
        return -1;
    }
    pid_t pid = worker_spawn(vctx, argv);
    int rcode = (pid > 0) ? worker_wait(pid) : -1;
    video_argv_free(argv);
    return write_status_reply(sock, hdr, rcode, req.output);
}

static int dispatch_one(int sock, const video_ctx_t *vctx)
{
    proto_header_t hdr;
    if (proto_read_header(sock, &hdr) < 0) {
        return -1;
    }

    switch (hdr.op_id) {
        case OPR_CONNECT: {
            proto_header_t reply = hdr;
            reply.client_id = (uint32_t)getpid() ^ (uint32_t)sock;
            reply.msg_size = (uint32_t)sizeof(reply);
            return proto_write_header(sock, &reply);
        }
        case OPR_TRIM:     return handle_trim(sock, vctx, &hdr);
        case OPR_FILTER:   return handle_filter(sock, vctx, &hdr);
        case OPR_MERGE:    return handle_merge(sock, vctx, &hdr);
        case OPR_MIXAUDIO: return handle_mixaudio(sock, vctx, &hdr);
        case OPR_BYE:
        default:
            return -1;
    }
}

// instalez handlerii pt semnale: SIGINT/SIGTERM -> shutdown, SIGPIPE -> ignorat
static void install_signal_handlers(void)
{
    struct sigaction sact;
    memset(&sact, 0, sizeof(sact));
    sact.sa_handler = on_signal;
    (void)sigaction(SIGINT, &sact, NULL);
    (void)sigaction(SIGTERM, &sact, NULL);

    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &ign, NULL);
}

// accepta un client nou si il adauga in vectorul de pollfd
static void accept_new_client(int listen_fd, struct pollfd *fds, nfds_t *nfds, nfds_t limit)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    socklen_t len = sizeof(peer);
    int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &len);
    if (client_fd < 0) {
        return;
    }
    if (*nfds < limit) {
        fds[*nfds].fd = client_fd; // adaug fd clientului
        fds[*nfds].events = POLLIN; // vreau sa fiu notificat cand are date
        (*nfds)++;
    } else {
        (void)close(client_fd); // prea multi clienti, refuz conexiunea
    }
}

int net_server_run(const server_cfg_t *cfg)
{
    install_signal_handlers();

    int listen_fd = open_listener(cfg->listen_port);
    if (listen_fd < 0) {
        (void)fprintf(stderr, "net_server: listener open failed\n");
        return -1;
    }

    // aloc vectorul de pollfd: slot 0 = listen, restul = clienti
    nfds_t limit = (nfds_t)cfg->max_clients + 1U;
    struct pollfd *fds = (struct pollfd *)calloc((size_t)limit, sizeof(struct pollfd));
    if (fds == NULL) {
        (void)close(listen_fd);
        return -1;
    }

    fds[0].fd = listen_fd; // primul slot e mereu socketul de listen
    fds[0].events = POLLIN;
    nfds_t nfds = 1;

    (void)fprintf(stderr, "vps_server: listening on port %d (max_clients=%d)\n", cfg->listen_port, cfg->max_clients);

    // bucla principala: poll() asteapta evenimente pe toti fd
    while (g_stop == 0) {
        int ready = poll(fds, nfds, POLL_TIMEOUT_MS);
        reap_children(); // la fiecare iteratie curatam zombii

        if (ready < 0) {
            if (errno == EINTR) {
                continue; // intrerupt de semnal, reincerc
            }
            break; // eroare fatala
        }
        if (ready == 0) {
            continue; // timeout, reincerc (verific g_stop)
        }

        // daca socketul de listen are date, acceptam un client nou
        if ((fds[0].revents & POLLIN) != 0) {
            accept_new_client(listen_fd, fds, &nfds, limit);
        }

        // parcurg toti clientii conectati
        for (nfds_t ix = 1; ix < nfds; ix++) {
            if ((fds[ix].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue; // fara eveniment pe acest fd
            }
            int sock = fds[ix].fd;
            if (dispatch_one(sock, &cfg->video) < 0) {
                // dispatch esuat sau client deconectat -> inchid si scot din vector
                (void)close(sock);
                fds[ix] = fds[nfds - 1]; // swap cu ultimul
                nfds--;
                ix--; // reverific pozitia curenta
            }
        }
    }

    for (nfds_t ix = 0; ix < nfds; ix++) {
        (void)close(fds[ix].fd);
    }
    free(fds);
    return 0;
}

/*
 *> Compilare si exemple de rulare:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * (net_server.c este compilat ca parte din vps_server)
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/net_server.c -o build/net_server.o
 *
 * --- Rulare cu succes ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ nohup bin/vps_server -v >/tmp/final_vps_server.log 2>&1 &
 * [1] 19224
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ head -3 /tmp/final_vps_server.log
 * nohup: ignoring input
 * vps_server: port=18081 max_clients=64 merge_par=4 ffmpeg=/usr/bin/ffmpeg
 * vps_server: running as user=vladb
 *
 * --- Rulare cu esec ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ nohup bin/vps_server >/tmp/net_a.log 2>&1 &
 * [1] 19583
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_server >/tmp/net_b.log 2>&1
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ cat /tmp/net_b.log
 * net_server: listener open failed
 */
