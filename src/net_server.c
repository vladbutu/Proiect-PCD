/**
 * Echipa 11 · IR3 2026 · PCD
 * Server TCP cu poll() (I/O multiplexing).
 *  - coada de joburi async: fork()+execv() pt ffmpeg, raspunde imediat cu task_id
 *  - operatii admin (UX): PING, LIST_JOBS, STATS, SHUTDOWN
 *  - transfer fisiere (REMOTE/IN): UPLOAD / DOWNLOAD streaming pe socket
 *  - STATUS / RESULT functional pe baza tabelei de joburi
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define LISTEN_BACKLOG 16
#define POLL_TIMEOUT_MS 1000
// permisiuni standard pt directoarele/fisierele scrise de server
#define DIR_MODE_DEFAULT  0755
#define FILE_MODE_DEFAULT 0644

#include "net_server.h" // server_cfg_t + net_server_run()
#include "proto.h" // header binar, payload-uri, helpers I/O
#include "worker.h" // worker_spawn / worker_dispatch_merge
#include "video_ops.h" // video_ctx_t + argv builders pt ffmpeg
#include "jobs.h" // tabela de joburi async + cleanup

#include <errno.h> // EINTR pt poll retry
#include <fcntl.h> // open flags pt upload/download
#include <libgen.h> // basename pt sanitize_basename (anti path traversal)
#include <netinet/in.h> // sockaddr_in, htons, htonl pt IPv4 listen
#include <signal.h> // sigaction, sig_atomic_t pt SIGINT/SIGTERM/SIGPIPE
#include <sys/inotify.h> // inotify_init1/add_watch pt watch outputs_dir
#include <sys/un.h> // sockaddr_un pt AF_UNIX (admin local)
#include <stdint.h> // uint16_t, uint32_t pt campuri protocol
#include <stdio.h> // fprintf, snprintf pt log si paths
#include <stdlib.h> // calloc/free pt vectorul de pollfd
#include <string.h> // memset, strncmp, strstr pt path checks
#include <sys/poll.h> // poll(), struct pollfd, POLLIN/POLLERR
#include <sys/socket.h> // socket, bind, listen, accept, setsockopt
#include <sys/stat.h> // mkdir, fstat, S_ISREG pt directoare si download
#include <sys/types.h> // pid_t
#include <time.h> // time(NULL) pt sweep cleanup periodic
#include <unistd.h> // close, getpid, read, write

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// ---------- I/O helpers pt raspunsuri ----------

// trimite un raspuns: header + payload de dimensiune fixa
static int reply_with(int sock, const proto_header_t *req, uint32_t op_id,
                      const void *payload, size_t plen)
{
    proto_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_size = (uint32_t)(sizeof(hdr) + plen);
    hdr.client_id = req->client_id;
    hdr.op_id = op_id;
    hdr.task_id = req->task_id;
    if (proto_write_header(sock, &hdr) < 0) {
        return -1;
    }
    if (plen > 0 && proto_write_full(sock, payload, plen) < 0) {
        return -1;
    }
    return 0;
}

static int reply_status(int sock, const proto_header_t *req, uint32_t task_id,
                        int32_t state, const char *path)
{
    proto_status_t sts;
    memset(&sts, 0, sizeof(sts));
    sts.task_id = htonl(task_id);
    sts.state = (int32_t)htonl((uint32_t)state);
    (void)snprintf(sts.result_path, sizeof(sts.result_path), "%s",
                   path != NULL ? path : "");
    return reply_with(sock, req, OPR_STATUS, &sts, sizeof(sts));
}

// ---------- Submit async pt video ops (TRIM/FILTER/MIXAUDIO) ----------

// fork+execv direct pe ffmpeg, inregistreaza in tabela, returneaza task_id
static uint32_t submit_simple(jobs_table_t *jobs, const video_ctx_t *vctx,
                              uint32_t op_id, char **argv, const char *output)
{
    pid_t pid = worker_spawn(vctx, argv);
    if (pid <= 0) {
        video_argv_free(argv);
        return 0U;
    }
    uint32_t tid = jobs_submit(jobs, op_id, output, argv, pid);
    if (tid == 0U) {
        // tabela plina; nu putem urmari jobul, ucidem copilul
        (void)kill(pid, SIGTERM);
        video_argv_free(argv);
        return 0U;
    }
    return tid;
}

// MERGE async: fork un orchestrator (sincron intern); parintele tine doar pid-ul lui
static uint32_t submit_merge(jobs_table_t *jobs, const video_ctx_t *vctx,
                             const proto_merge_t *op)
{
    pid_t pid = fork();
    if (pid < 0) {
        return 0U;
    }
    if (pid == 0) {
        int rc = worker_dispatch_merge(vctx, op);
        _exit(rc == 0 ? 0 : 1);
    }
    // parinte: argv NULL pt merge (orchestratorul isi gestioneaza singur)
    return jobs_submit(jobs, OPR_MERGE, op->output, NULL, pid);
}

// ---------- Handlere video (TRIM/FILTER/MERGE/MIXAUDIO) ----------

static int handle_trim(int sock, const video_ctx_t *vctx, jobs_table_t *jobs,
                       const proto_header_t *hdr)
{
    proto_trim_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    char **argv = video_build_trim_argv(vctx, &req);
    if (argv == NULL) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    uint32_t tid = submit_simple(jobs, vctx, OPR_TRIM, argv, req.output);
    if (tid == 0U) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    return reply_status(sock, hdr, tid, TASK_STATE_RUNNING, req.output);
}

static int handle_filter(int sock, const video_ctx_t *vctx, jobs_table_t *jobs,
                         const proto_header_t *hdr)
{
    proto_filter_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    char **argv = video_build_filter_argv(vctx, &req);
    if (argv == NULL) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    uint32_t tid = submit_simple(jobs, vctx, OPR_FILTER, argv, req.output);
    if (tid == 0U) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    return reply_status(sock, hdr, tid, TASK_STATE_RUNNING, req.output);
}

static int handle_merge(int sock, const video_ctx_t *vctx, jobs_table_t *jobs,
                        const proto_header_t *hdr)
{
    proto_merge_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    uint32_t tid = submit_merge(jobs, vctx, &req);
    if (tid == 0U) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    return reply_status(sock, hdr, tid, TASK_STATE_RUNNING, req.output);
}

static int handle_mixaudio(int sock, const video_ctx_t *vctx, jobs_table_t *jobs,
                           const proto_header_t *hdr)
{
    proto_mixaudio_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    char **argv = video_build_mixaudio_argv(vctx, &req);
    if (argv == NULL) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    uint32_t tid = submit_simple(jobs, vctx, OPR_MIXAUDIO, argv, req.output);
    if (tid == 0U) {
        return reply_status(sock, hdr, 0U, TASK_STATE_ERR, "");
    }
    return reply_status(sock, hdr, tid, TASK_STATE_RUNNING, req.output);
}

// ---------- STATUS / RESULT ----------

static int handle_status(int sock, jobs_table_t *jobs, const proto_header_t *hdr)
{
    proto_query_t query;
    if (proto_read_full(sock, &query, sizeof(query)) < 0) {
        return -1;
    }
    uint32_t task_id = ntohl(query.task_id);
    job_entry_t *slot = jobs_find(jobs, task_id);
    if (slot == NULL) {
        return reply_status(sock, hdr, task_id, TASK_STATE_ERR, "");
    }
    return reply_status(sock, hdr, slot->task_id, slot->state, slot->output);
}

// ---------- Admin ops (UX): PING / LIST_JOBS / STATS / SHUTDOWN ----------

static int handle_ping(int sock, const proto_header_t *hdr)
{
    return reply_with(sock, hdr, OPR_PING, NULL, 0);
}

static int handle_list_jobs(int sock, jobs_table_t *jobs, const proto_header_t *hdr)
{
    proto_list_jobs_reply_t rep;
    memset(&rep, 0, sizeof(rep));
    uint32_t cnt = 0;
    for (size_t ix = 0; ix < JOBS_MAX && cnt < PROTO_LIST_MAX_JOBS; ix++) {
        if (jobs->slots[ix].used == 0) {
            continue;
        }
        rep.jobs[cnt].task_id = htonl(jobs->slots[ix].task_id);
        rep.jobs[cnt].op_id = htonl(jobs->slots[ix].op_id);
        rep.jobs[cnt].state = (int32_t)htonl((uint32_t)jobs->slots[ix].state);
        (void)snprintf(rep.jobs[cnt].output, sizeof(rep.jobs[cnt].output),
                       "%s", jobs->slots[ix].output);
        cnt++;
    }
    rep.count = htonl(cnt);
    return reply_with(sock, hdr, OPR_LIST_JOBS, &rep, sizeof(rep));
}

static int handle_stats(int sock, jobs_table_t *jobs, const proto_header_t *hdr)
{
    proto_stats_reply_t rep;
    memset(&rep, 0, sizeof(rep));
    uint32_t queued = 0;
    uint32_t running = 0;
    for (size_t ix = 0; ix < JOBS_MAX; ix++) {
        if (jobs->slots[ix].used == 0) {
            continue;
        }
        if (jobs->slots[ix].state == TASK_STATE_QUEUED) {
            queued++;
        }
        if (jobs->slots[ix].state == TASK_STATE_RUNNING) {
            running++;
        }
    }
    rep.jobs_queued = htonl(queued);
    rep.jobs_running = htonl(running);
    rep.jobs_done = htonl(jobs->counter_done);
    rep.jobs_failed = htonl(jobs->counter_failed);
    rep.uptime_sec = htonl((uint32_t)(time(NULL) - jobs->started_at));
    return reply_with(sock, hdr, OPR_STATS, &rep, sizeof(rep));
}

static int handle_shutdown(int sock, const proto_header_t *hdr)
{
    // raspund ACK inainte de a seta flagul de shutdown
    int rc = reply_with(sock, hdr, OPR_SHUTDOWN, NULL, 0);
    g_stop = 1;
    return rc;
}

// OPR_INFO: identitate server pt admin (M3) - version/build/pid/uptime
static int handle_info(int sock, jobs_table_t *jobs, const proto_header_t *hdr)
{
    proto_info_reply_t rep;
    memset(&rep, 0, sizeof(rep));
    (void)snprintf(rep.version, sizeof(rep.version), "echipa11-m3");
    (void)snprintf(rep.build_date, sizeof(rep.build_date), "%s", __DATE__);
    rep.pid = htonl((uint32_t)getpid());
    rep.uptime_sec = htonl((uint32_t)(time(NULL) - jobs->started_at));
    return reply_with(sock, hdr, OPR_INFO, &rep, sizeof(rep));
}

// ---------- File transfer (REMOTE/IN): UPLOAD / DOWNLOAD ----------

// extrage doar basename-ul fara directoare, ca sa nu permit path traversal
static void sanitize_basename(char *out, size_t out_sz, const char *raw)
{
    char tmp[PROTO_MAX_PATH];
    (void)snprintf(tmp, sizeof(tmp), "%s", raw != NULL ? raw : "file");
    char *base = basename(tmp);
    // refuz secventele suspecte
    if (base == NULL || base[0] == '\0' || base[0] == '.') {
        (void)snprintf(out, out_sz, "upload.bin");
        return;
    }
    (void)snprintf(out, out_sz, "%s", base);
}

static int handle_upload(int sock, const video_ctx_t *vctx, const proto_header_t *hdr)
{
    proto_upload_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    uint64_t size = req.size_bytes; // payload host-order; clientul scrie host-order
    char safe[PROTO_MAX_PATH];
    sanitize_basename(safe, sizeof(safe), req.filename);

    // subdir dedicat + prefix pid/counter ca sa evit coliziune cu fixtures si intre clienti
    char incoming[PROTO_MAX_PATH];
    (void)snprintf(incoming, sizeof(incoming), "%.400s/incoming", vctx->uploads_dir);
    (void)mkdir(vctx->uploads_dir, DIR_MODE_DEFAULT);
    (void)mkdir(incoming, DIR_MODE_DEFAULT);

    static uint32_t upload_counter = 0;
    upload_counter++;
    char stored[PROTO_MAX_PATH];
    (void)snprintf(stored, sizeof(stored), "%.380s/%u_%u_%.80s",
                   incoming, (uint32_t)getpid(), upload_counter, safe);

    int fd = open(stored, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE_DEFAULT);
    if (fd < 0) {
        // tot trebuie sa drenez datele de pe socket ca sa nu desincronizam protocolul
        // dar daca size e mare, mai bine inchidem conexiunea
        proto_upload_reply_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.state = (int32_t)htonl((uint32_t)TASK_STATE_ERR);
        (void)reply_with(sock, hdr, OPR_UPLOAD, &rep, sizeof(rep));
        return -1;
    }
    int rc = proto_recv_file(sock, fd, size);
    (void)close(fd);

    proto_upload_reply_t rep;
    memset(&rep, 0, sizeof(rep));
    if (rc < 0) {
        rep.state = (int32_t)htonl((uint32_t)TASK_STATE_ERR);
        rep.bytes_received = 0;
        (void)reply_with(sock, hdr, OPR_UPLOAD, &rep, sizeof(rep));
        return -1;
    }
    rep.state = (int32_t)htonl((uint32_t)TASK_STATE_DONE);
    rep.bytes_received = size;
    (void)snprintf(rep.stored_path, sizeof(rep.stored_path), "%s", stored);
    return reply_with(sock, hdr, OPR_UPLOAD, &rep, sizeof(rep));
}

// permite descarcari doar din uploads_dir / outputs_dir (anti path-traversal simplu)
static int path_is_allowed(const video_ctx_t *vctx, const char *path)
{
    if (path == NULL || path[0] == '\0' || strstr(path, "..") != NULL) {
        return 0;
    }
    // permit doar download din outputs_dir (rezultate de ffmpeg)
    if (strncmp(path, vctx->outputs_dir, strlen(vctx->outputs_dir)) == 0) {
        return 1;
    }
    return 0;
}

static int handle_download(int sock, const video_ctx_t *vctx, const proto_header_t *hdr)
{
    proto_download_t req;
    if (proto_read_full(sock, &req, sizeof(req)) < 0) {
        return -1;
    }
    proto_download_reply_t rep;
    memset(&rep, 0, sizeof(rep));

    if (path_is_allowed(vctx, req.path) == 0) {
        rep.state = (int32_t)htonl((uint32_t)TASK_STATE_ERR);
        return reply_with(sock, hdr, OPR_DOWNLOAD, &rep, sizeof(rep));
    }

    int fd = open(req.path, O_RDONLY);
    if (fd < 0) {
        rep.state = (int32_t)htonl((uint32_t)TASK_STATE_ERR);
        return reply_with(sock, hdr, OPR_DOWNLOAD, &rep, sizeof(rep));
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        rep.state = (int32_t)htonl((uint32_t)TASK_STATE_ERR);
        return reply_with(sock, hdr, OPR_DOWNLOAD, &rep, sizeof(rep));
    }
    rep.state = (int32_t)htonl((uint32_t)TASK_STATE_DONE);
    rep.size_bytes = (uint64_t)st.st_size;
    if (reply_with(sock, hdr, OPR_DOWNLOAD, &rep, sizeof(rep)) < 0) {
        (void)close(fd);
        return -1;
    }
    int rc = proto_send_file(sock, fd, (uint64_t)st.st_size);
    (void)close(fd);
    return rc;
}

// ---------- Listener & dispatch ----------

#define VPS_UNIX_PATH "/tmp/vps_admin.sock"

static int open_listener(int port)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        return -1;
    }
    int yes = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        (void)close(sfd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

// AF_UNIX listener pt admin local (cerinta UX socket M3). Path: /tmp/vps_admin.sock
static int open_unix_listener(const char *path)
{
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        return -1;
    }
    (void)unlink(path); // sterg socketul orfan dintr-o rulare anterioara
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
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

// INotify pt outputs_dir: notifica in log cand un job termina si scrie fisierul
static int open_inotify_watch(const char *outputs_dir)
{
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        return -1;
    }
    int wd = inotify_add_watch(ifd, outputs_dir, IN_CLOSE_WRITE | IN_CREATE);
    if (wd < 0) {
        (void)close(ifd);
        return -1;
    }
    return ifd;
}

// citeste si raporteaza evenimentele inotify acumulate (fara block)
static void drain_inotify(int ifd)
{
    char buf[4096] __attribute__((aligned(8)));
    for (;;) {
        ssize_t n = read(ifd, buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        for (ssize_t off = 0; off < n; ) {
            const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
            if (ev->len > 0) {
                const char *kind = (ev->mask & IN_CLOSE_WRITE) ? "CLOSE_WRITE" : "CREATE";
                (void)fprintf(stderr, "vps_server: inotify %s outputs/%s\n", kind, ev->name);
            }
            off += (ssize_t)(sizeof(*ev) + ev->len);
        }
    }
}

static int dispatch_one(int sock, const video_ctx_t *vctx, jobs_table_t *jobs)
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
        case OPR_TRIM:        return handle_trim(sock, vctx, jobs, &hdr);
        case OPR_FILTER:      return handle_filter(sock, vctx, jobs, &hdr);
        case OPR_MERGE:       return handle_merge(sock, vctx, jobs, &hdr);
        case OPR_MIXAUDIO:    return handle_mixaudio(sock, vctx, jobs, &hdr);
        case OPR_STATUS:      // STATUS si RESULT au acelasi raspuns
        case OPR_RESULT:      return handle_status(sock, jobs, &hdr);
        case OPR_PING:        return handle_ping(sock, &hdr);
        case OPR_LIST_JOBS:   return handle_list_jobs(sock, jobs, &hdr);
        case OPR_STATS:       return handle_stats(sock, jobs, &hdr);
        case OPR_INFO:        return handle_info(sock, jobs, &hdr);
        case OPR_SHUTDOWN:    return handle_shutdown(sock, &hdr);
        case OPR_UPLOAD:      return handle_upload(sock, vctx, &hdr);
        case OPR_DOWNLOAD:    return handle_download(sock, vctx, &hdr);
        case OPR_BYE:
        default:
            return -1;
    }
}

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

static void accept_new_client(int listen_fd, struct pollfd *fds,
                              nfds_t *nfds, nfds_t limit)
{
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        return;
    }
    if (*nfds < limit) {
        fds[*nfds].fd = client_fd;
        fds[*nfds].events = POLLIN;
        (*nfds)++;
    } else {
        (void)close(client_fd);
    }
}

int net_server_run(const server_cfg_t *cfg)
{
    install_signal_handlers();

    // creez directoarele de upload/output ca sa pot scrie fisierele primite
    (void)mkdir(cfg->video.uploads_dir, DIR_MODE_DEFAULT);
    (void)mkdir(cfg->video.outputs_dir, DIR_MODE_DEFAULT);

    jobs_table_t jobs;
    jobs_init(&jobs);

    int listen_fd = open_listener(cfg->listen_port);
    if (listen_fd < 0) {
        (void)fprintf(stderr, "net_server: listener open failed\n");
        return -1;
    }
    int unix_fd = open_unix_listener(VPS_UNIX_PATH);
    if (unix_fd < 0) {
        (void)fprintf(stderr, "net_server: unix listener open failed (continui doar pe INET)\n");
    }
    int inotify_fd = open_inotify_watch(cfg->video.outputs_dir);
    if (inotify_fd < 0) {
        (void)fprintf(stderr, "net_server: inotify init failed (continui fara watch)\n");
    }

    // sloturi pollfd: +1 INET, +1 UNIX, +1 inotify, +max_clients pt sesiuni
    nfds_t limit = (nfds_t)cfg->max_clients + 3U;
    struct pollfd *fds = (struct pollfd *)calloc((size_t)limit, sizeof(struct pollfd));
    if (fds == NULL) {
        (void)close(listen_fd);
        if (unix_fd >= 0) { (void)close(unix_fd); }
        if (inotify_fd >= 0) { (void)close(inotify_fd); }
        return -1;
    }
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;
    nfds_t nfds = 1;
    nfds_t unix_ix = 0;
    nfds_t inotify_ix = 0;
    if (unix_fd >= 0) {
        fds[nfds].fd = unix_fd;
        fds[nfds].events = POLLIN;
        unix_ix = nfds;
        nfds++;
    }
    if (inotify_fd >= 0) {
        fds[nfds].fd = inotify_fd;
        fds[nfds].events = POLLIN;
        inotify_ix = nfds;
        nfds++;
    }
    nfds_t sessions_start = nfds; // primul slot dedicat sesiunilor (peste listenere/inotify)

    // Cleanup config (0 din libconfig -> default). outputs/ NU este atins.
    unsigned ret_sec = (cfg->jobs_retention_sec > 0)
                       ? (unsigned)cfg->jobs_retention_sec
                       : JOBS_RETENTION_SEC_DEFAULT;
    char incoming_dir[PROTO_MAX_PATH];
    (void)snprintf(incoming_dir, sizeof(incoming_dir), "%.400s/incoming",
                   cfg->video.uploads_dir);
    time_t last_cleanup = time(NULL);

    (void)fprintf(stderr,
        "vps_server: listening on port %d (max_clients=%d, jobs_retention=%us)\n",
        cfg->listen_port, cfg->max_clients, ret_sec);
    if (unix_fd >= 0) {
        (void)fprintf(stderr, "vps_server: also listening on AF_UNIX %s\n", VPS_UNIX_PATH);
    }
    if (inotify_fd >= 0) {
        (void)fprintf(stderr, "vps_server: inotify watch active on %s\n", cfg->video.outputs_dir);
    }

    while (g_stop == 0) {
        int ready = poll(fds, nfds, POLL_TIMEOUT_MS);
        jobs_reap_running(&jobs);

        // sweep cleanup: elibereaza sloturi vechi + sterge duplicate din incoming/ (outputs/ neatins)
        time_t now = time(NULL);
        if ((now - last_cleanup) >= CLEANUP_INTERVAL_SEC) {
            unsigned freed = jobs_evict_old(&jobs, ret_sec);
            unsigned dups = cleanup_incoming_duplicates(incoming_dir,
                                                       cfg->video.uploads_dir);
            if (freed != 0U || dups != 0U) {
                (void)fprintf(stderr,
                    "vps_server: cleanup slots_freed=%u dup_removed=%u\n",
                    freed, dups);
            }
            last_cleanup = now;
        }

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            accept_new_client(listen_fd, fds, &nfds, limit);
        }
        if (unix_ix != 0 && (fds[unix_ix].revents & POLLIN) != 0) {
            accept_new_client(unix_fd, fds, &nfds, limit);
        }
        if (inotify_ix != 0 && (fds[inotify_ix].revents & POLLIN) != 0) {
            drain_inotify(inotify_fd);
        }
        for (nfds_t ix = sessions_start; ix < nfds; ix++) {
            if ((fds[ix].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            int sock = fds[ix].fd;
            if (dispatch_one(sock, &cfg->video, &jobs) < 0) {
                (void)close(sock);
                fds[ix] = fds[nfds - 1];
                nfds--;
                ix--;
            }
        }
    }

    for (nfds_t ix = 0; ix < nfds; ix++) {
        (void)close(fds[ix].fd);
    }
    free(fds);
    if (unix_fd >= 0) {
        (void)unlink(VPS_UNIX_PATH);
    }
    jobs_destroy(&jobs);
    return 0;
}
