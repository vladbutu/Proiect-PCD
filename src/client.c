/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Client TCP pentru Video Processing Server
 * Rolul aplicatiei:
 * - construieste request-ul pentru operatiile trim/filter/merge/mixaudio
 * - parseaza argumentele CLI cu getopt (host, port, operatie, fisiere)
 * - deschide conexiunea TCP folosind getaddrinfo (thread-safe)
 * - trimite header + payload conform protocolului binar din proto.h
 * - citeste raspunsul serverului si il afiseaza in consola
 * Cazuri de eroare acoperite explicit:
 * - optiuni lipsa sau invalide pe linia de comanda
 * - conversie numerica invalida (port, start_ms, end_ms) via strtol
 * - getaddrinfo / socket / connect esuate (DNS sau retea)
 * - trimitere/citire partiala pe socket (proto_write/read_full)
 * - raspuns lipsa sau incomplet de la server
 */

// activeaza API-urile POSIX necesare in headerele sistemului
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L // nivelul POSIX tinta pentru proiect
#endif

#define DEFAULT_HOST "127.0.0.1" // host implicit cand lipseste -H
#define DEFAULT_PORT 18081 // port implicit cand lipseste -P
#define TRIM_START_DEFAULT 0 // start implicit pentru trim (ms)
#define TRIM_END_DEFAULT 5000 // end implicit pentru trim (ms)
#define STRTOL_BASE_DEC 10 // baza zecimala pentru strtol
#define PORT_STR_LEN 16 // buffer pentru port convertit in string
#define STRERR_BUF_LEN 256 // buffer pentru strerror_r (thread-safe)

#include "proto.h" // protocolul binar: headere, payload-uri, utilitare I/O
#include "getopt_compat.h" // declaratii getopt/optarg pentru portabilitate

#include <errno.h> // errno pentru diagnosticul erorilor
#include <limits.h> // INT_MAX/INT_MIN pentru validarea strtol
#include <netdb.h> // getaddrinfo/freeaddrinfo pentru rezolvare DNS
#include <netinet/in.h> // ntohl pentru conversii network->host order
#include <stdint.h> // uint32_t pentru campurile protocolului
#include <stdio.h> // fprintf, snprintf pentru output/erori
#include <stdlib.h> // EXIT_SUCCESS, EXIT_FAILURE, strtol
#include <string.h> // strcmp, memset, snprintf pentru string-uri
#include <sys/socket.h> // socket, connect, send, recv
#include <unistd.h> // close

// agregheaza toate argumentele preluate din linia de comanda
typedef struct cli_args {
    const char *host; // adresa serverului
    int port; // portul serverului
    const char *op; // operatia solicitata (trim/filter/merge/mixaudio)
    const char *input; // fisier video de intrare
    const char *output; // fisier video de iesire
    const char *audio; // fisier audio pentru mixaudio
    const char *filter; // numele filtrului pentru operatia filter
    const char *transition; // tranzitia dintre clipuri pentru merge
    uint32_t start_ms; // momentul de start pentru trim (ms)
    uint32_t end_ms; // momentul de end pentru trim (ms)
    char clips[PROTO_MAX_CLIPS][PROTO_MAX_PATH]; // clipurile incluse la merge
    uint32_t clip_count; // numarul de clipuri adaugate
} cli_args_t;

static void usage(const char *argv0)
{
    (void)fprintf(stderr,
        "Usage: %s -o <trim|filter|merge|mixaudio> [options]\n"
    "  -H <host> server host (default: %s)\n"
    "  -P <port> server port (default: %d)\n"
    "  -i <file> input video\n"
    "  -O <file> output video\n"
    "  -a <file> audio file (mixaudio)\n"
    "  -f <name> filter name (filter op)\n"
    "  -T <name> transition name (merge op)\n"
    "  -c <file> append a clip to the merge list (repeatable)\n"
    "  -s <ms> start ms (trim)\n"
    "  -e <ms> end ms (trim)\n"
    "  -h show this help\n",
        argv0, DEFAULT_HOST, DEFAULT_PORT);
}

// foloseste strtol in loc de atoi pentru a detecta erori de conversie
// la input invalid se intoarce fallback, fara a intrerupe executia
static int parse_int(const char *str, int fallback)
{
    if (str == NULL || *str == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(str, &end, STRTOL_BASE_DEC);
    if (errno != 0 || end == str || *end != '\0' ||
        val > INT_MAX || val < INT_MIN) {
        return fallback;
    }
    return (int)val;
}

static int parse_args(int argc, char **argv, cli_args_t *args)
{
    memset(args, 0, sizeof(*args));
    args->host = DEFAULT_HOST;
    args->port = DEFAULT_PORT;
    args->start_ms = TRIM_START_DEFAULT;
    args->end_ms = TRIM_END_DEFAULT;

    int opt;
    while ((opt = getopt(argc, argv, "H:P:o:i:O:a:f:T:c:s:e:h")) != -1) {
        switch (opt) {
            case 'H': args->host = optarg; break;
            case 'P': args->port = parse_int(optarg, DEFAULT_PORT); break;
            case 'o': args->op = optarg; break;
            case 'i': args->input = optarg; break;
            case 'O': args->output = optarg; break;
            case 'a': args->audio = optarg; break;
            case 'f': args->filter = optarg; break;
            case 'T': args->transition = optarg; break;
            case 's': args->start_ms = (uint32_t)parse_int(optarg, 0); break;
            case 'e': args->end_ms = (uint32_t)parse_int(optarg, 0); break;
            case 'c':
                if (args->clip_count >= PROTO_MAX_CLIPS) {
                    return -1;
                }
                {
                    uint32_t idx = args->clip_count;
                    (void)snprintf(args->clips[idx], PROTO_MAX_PATH, "%s", optarg);
                    args->clip_count = idx + 1U;
                }
                break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 1 : -1;
        }
    }
    if (args->op == NULL) {
        return -1;
    }
    return 0;
}

// rezolva host-ul si incearca sa stabileasca o conexiune TCP
// getaddrinfo este preferat fata de gethostbyname (thread-safe)
static int dial(const cli_args_t *args)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints)); // initializeaza complet structura hints
    hints.ai_family = AF_INET; // doar IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    // converteste portul in string pentru getaddrinfo
    char port_str[PORT_STR_LEN];
    (void)snprintf(port_str, sizeof(port_str), "%d", args->port);

    // rezolva DNS-ul; rezultatul este o lista de adrese candidate
    struct addrinfo *res = NULL;
    if (getaddrinfo(args->host, port_str, &hints, &res) != 0) {
        return -1;
    }

    // incearca fiecare adresa pana cand connect reuseste
    int sock = -1;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock < 0) {
            continue; // socket() esuat, continua cu urmatoarea adresa
        }
        if (connect(sock, it->ai_addr, it->ai_addrlen) == 0) {
            break; // conexiune stabilita
        }
        (void)close(sock); // connect esuat, inchide si continua
        sock = -1;
    }
    freeaddrinfo(res); // elibereaza lista de adrese
    return sock;
}

// construieste si trimite payload-ul pentru operatia TRIM
static int do_trim(int sock, const cli_args_t *args, uint32_t task)
{
    proto_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_size = (uint32_t)(sizeof(hdr) + sizeof(proto_trim_t));
    hdr.op_id = OPR_TRIM;
    hdr.task_id = task;

    proto_trim_t trim;
    memset(&trim, 0, sizeof(trim));
    trim.start_ms = args->start_ms;
    trim.end_ms = args->end_ms;
    (void)snprintf(trim.input, sizeof(trim.input), "%s", args->input != NULL ? args->input : "");
    (void)snprintf(trim.output, sizeof(trim.output), "%s", args->output != NULL ? args->output : "");
    if (proto_write_header(sock, &hdr) < 0) {
        return -1;
    }
    return proto_write_full(sock, &trim, sizeof(trim));
}

// construieste si trimite payload-ul pentru operatia FILTER
static int do_filter(int sock, const cli_args_t *args, uint32_t task)
{
    proto_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_size = (uint32_t)(sizeof(hdr) + sizeof(proto_filter_t));
    hdr.op_id = OPR_FILTER;
    hdr.task_id = task;

    proto_filter_t flt;
    memset(&flt, 0, sizeof(flt));
    (void)snprintf(flt.filter, sizeof(flt.filter), "%s", args->filter != NULL ? args->filter : "");
    (void)snprintf(flt.input, sizeof(flt.input), "%s", args->input != NULL ? args->input : "");
    (void)snprintf(flt.output, sizeof(flt.output), "%s", args->output != NULL ? args->output : "");
    if (proto_write_header(sock, &hdr) < 0) {
        return -1;
    }
    return proto_write_full(sock, &flt, sizeof(flt));
}

// construieste si trimite payload-ul pentru operatia MERGE
static int do_merge(int sock, const cli_args_t *args, uint32_t task)
{
    proto_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_size = (uint32_t)(sizeof(hdr) + sizeof(proto_merge_t));
    hdr.op_id = OPR_MERGE;
    hdr.task_id = task;

    proto_merge_t mrg;
    memset(&mrg, 0, sizeof(mrg));
    mrg.count = args->clip_count;
    (void)snprintf(mrg.transition, sizeof(mrg.transition), "%s", args->transition != NULL ? args->transition : "none");
    (void)snprintf(mrg.output, sizeof(mrg.output), "%s", args->output != NULL ? args->output : "");
    for (uint32_t ix = 0; ix < args->clip_count; ix++) {
        (void)snprintf(mrg.clips[ix], PROTO_MAX_PATH, "%s", args->clips[ix]);
    }
    if (proto_write_header(sock, &hdr) < 0) {
        return -1;
    }
    return proto_write_full(sock, &mrg, sizeof(mrg));
}

// construieste si trimite payload-ul pentru operatia MIXAUDIO
static int do_mixaudio(int sock, const cli_args_t *args, uint32_t task)
{
    proto_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_size = (uint32_t)(sizeof(hdr) + sizeof(proto_mixaudio_t));
    hdr.op_id = OPR_MIXAUDIO;
    hdr.task_id = task;

    proto_mixaudio_t mix;
    memset(&mix, 0, sizeof(mix));
    (void)snprintf(mix.input_video, sizeof(mix.input_video), "%s", args->input != NULL ? args->input : "");
    (void)snprintf(mix.input_audio, sizeof(mix.input_audio), "%s", args->audio != NULL ? args->audio : "");
    (void)snprintf(mix.output, sizeof(mix.output), "%s", args->output != NULL ? args->output : "");
    if (proto_write_header(sock, &hdr) < 0) {
        return -1;
    }
    return proto_write_full(sock, &mix, sizeof(mix));
}

// citeste raspunsul serverului (header + status) si il afiseaza
static void print_reply(int sock)
{
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        (void)fprintf(stderr, "client: no reply header\n");
        return;
    }
    proto_status_t sts;
    if (proto_read_full(sock, &sts, sizeof(sts)) < 0) {
        (void)fprintf(stderr, "client: no status payload\n");
        return;
    }
    // converteste valorile numerice din network byte order
    int state = (int)ntohl((uint32_t)sts.state);
    uint32_t task_id = ntohl(sts.task_id);
    (void)fprintf(stdout, "reply: task_id=%u state=%d path=%s\n", task_id, state, sts.result_path);
}

int main(int argc, char **argv)
{
    // parseaza argumentele de pe linia de comanda
    cli_args_t args;
    int prc = parse_args(argc, argv, &args);
    if (prc != 0) {
        if (prc < 0) {
            usage(argv[0]);
        }
        return (prc == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // initiaza conexiunea catre server
    int sock = dial(&args);
    if (sock < 0) {
        // strerror_r este folosit pentru varianta thread-safe
        char errbuf[STRERR_BUF_LEN];
        (void)strerror_r(errno, errbuf, sizeof(errbuf));
        (void)fprintf(stderr, "client: cannot connect to %s:%d (%s)\n", args.host, args.port, errbuf);
        return EXIT_FAILURE;
    }

    // foloseste PID-ul local drept task_id
    const uint32_t task_id = (uint32_t)getpid();
    int rcode = -1;
    const char *op = args.op;
    // trimite request-ul potrivit operatiei cerute
    if (strcmp(op, "trim") == 0) {
        rcode = do_trim(sock, &args, task_id);
    } else if (strcmp(op, "filter") == 0) {
        rcode = do_filter(sock, &args, task_id);
    } else if (strcmp(op, "merge") == 0) {
        rcode = do_merge(sock, &args, task_id);
    } else if (strcmp(op, "mixaudio") == 0) {
        rcode = do_mixaudio(sock, &args, task_id);
    } else {
        (void)fprintf(stderr, "client: unknown op '%s'\n", op);
    }

    if (rcode == 0) {
        print_reply(sock);
    }

    (void)close(sock);
    return (rcode == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 *> Build + exemple de executie:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/client.c -o build/client.o
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude build/client.o build/proto.o -o bin/vps_client
 *
 * --- Scenarii cu rezultat OK ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -h
 * Usage: bin/vps_client -o <trim|filter|merge|mixaudio> [options]
 *   -H <host>   server host        (default: 127.0.0.1)
 *   -P <port>   server port        (default: 18081)
 *   -i <file>   input video
 *   -O <file>   output video
 *   -a <file>   audio file (mixaudio)
 *   -f <name>   filter name (filter op)
 *   -T <name>   transition name (merge op)
 *   -c <file>   append a clip to the merge list (repeatable)
 *   -s <ms>     start ms (trim)
 *   -e <ms>     end ms   (trim)
 *   -h          show this help
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o trim -i data/uploads/clip1.mp4 -O data/outputs/final_trimmed.mp4 -s 0 -e 1200
 * reply: task_id=17918 state=2 path=data/outputs/final_trimmed.mp4
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o filter -i data/uploads/clip1.mp4 -O data/outputs/final_filtered.mp4 -f hflip
 * reply: task_id=17922 state=2 path=data/outputs/final_filtered.mp4
 *
 * --- Scenariu cu eroare controlata ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client
 * Usage: bin/vps_client -o <trim|filter|merge|mixaudio> [options]
 * (exit code 1 - lipsa parametru obligatoriu -o)
 */
