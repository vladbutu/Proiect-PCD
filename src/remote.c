/**
 * Echipa 11 · IR3 2026 · PCD
 * vps_remote: clientul REMOTE (IN) cu transfer fisiere bidirectional
 * Workflow tipic pentru o operatie video:
 *   1. UPLOAD trimite fisierele locale (input + opt audio + opt clipuri)
 *   2. SUBMIT cere serverului sa proceseze, primeste task_id
 *   3. PPOLL intreaba STATUS pana state == DONE / ERR
 *   4. DOWNLOAD descarca rezultatul local
 *
 * Operatii suportate: trim, filter, merge, mixaudio.
 * Asta acopera 4/6 din operatiile SRS (>70%, prag obligatoriu in milestone 2).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "proto.h" // protocol binar + helpers send_file/recv_file
#include "getopt_compat.h" // declaratii explicite getopt/optarg/optind

#include <errno.h> // errno pt strerror la connect/open
#include <fcntl.h> // open flags pt upload/download
#include <libgen.h> // basename pt extragere nume fisier la upload
#include <limits.h> // INT_MAX/INT_MIN pt validare strtol
#include <netdb.h> // getaddrinfo / freeaddrinfo (thread-safe DNS)
#include <netinet/in.h> // htonl/ntohl pt conversie endianness
#include <stdint.h> // uint32_t, uint64_t pt task_id si dimensiuni fisier
#include <stdio.h> // fprintf, snprintf pt feedback CLI
#include <stdlib.h> // EXIT_SUCCESS/EXIT_FAILURE, strtol
#include <string.h> // memset, strcmp, strerror pt CLI dispatch
#include <sys/socket.h> // socket, connect pt TCP catre vps_server
#include <sys/stat.h> // fstat, S_ISREG pt verificare fisier upload
#include <time.h> // nanosleep, struct timespec pt poll STATUS
#include <unistd.h> // close, getpid pt CLI flow

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 18081
#define PORT_STR_LEN 16
#define STRTOL_BASE_DEC 10
#define POLL_SLEEP_NSEC 500000000L // 0.5s intre interogari STATUS (nanosleep)
#define POLL_MAX_ATTEMPTS 600      // 600 * 0.5s = 5 min limita totala
#define DEFAULT_END_MS 5000        // valoare implicita pt -e (trim end)
#define FILE_MODE_DEFAULT 0644

typedef struct cli_args {
    const char *host;
    int  port;
    const char *op;             // trim / filter / merge / mixaudio
    const char *input;          // fisier local de uploadat
    const char *audio;          // fisier audio local (mixaudio)
    const char *output_local;   // unde scriu rezultatul local
    const char *filter;
    const char *transition;
    char clips_local[PROTO_MAX_CLIPS][PROTO_MAX_PATH];
    uint32_t clip_count;
    uint32_t start_ms;
    uint32_t end_ms;
    int no_download;            // skip download (test mode)
} cli_args_t;

static void usage(const char *argv0)
{
    (void)fprintf(stderr,
        "vps_remote (Echipa 11) - clientul REMOTE cu transfer fisiere\n"
        "Usage: %s [-H host] [-P port] -o <op> [options]\n"
        "Operatii: trim, filter, merge, mixaudio\n"
        "  -H <host>   server host (default: %s)\n"
        "  -P <port>   server port (default: %d)\n"
        "  -o <op>     operatia (trim|filter|merge|mixaudio)\n"
        "  -i <file>   fisier video local (urcat pe server)\n"
        "  -a <file>   fisier audio local (mixaudio)\n"
        "  -c <file>   clip local pt merge (repetabil)\n"
        "  -O <file>   unde scriu rezultatul local dupa download\n"
        "  -f <name>   nume filtru ffmpeg (filter)\n"
        "  -T <name>   nume tranzitie (merge)\n"
        "  -s <ms>     start ms (trim)\n"
        "  -e <ms>     end ms (trim)\n"
        "  -N          skip download (just submit + poll)\n",
        argv0, DEFAULT_HOST, DEFAULT_PORT);
}

static int parse_int_safe(const char *str, int fallback)
{
    if (str == NULL || *str == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(str, &end, STRTOL_BASE_DEC);
    if (errno != 0 || end == str || *end != '\0' || val < INT_MIN || val > INT_MAX) {
        return fallback;
    }
    return (int)val;
}

static int dial(const char *host, int port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[PORT_STR_LEN];
    (void)snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        (void)close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

static int send_simple(int sock, uint32_t op_id, const void *payload, size_t plen)
{
    proto_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_size = (uint32_t)(sizeof(hdr) + plen);
    hdr.op_id = op_id;
    hdr.task_id = (uint32_t)getpid();
    if (proto_write_header(sock, &hdr) < 0) {
        return -1;
    }
    if (plen > 0 && proto_write_full(sock, payload, plen) < 0) {
        return -1;
    }
    return 0;
}

// urca un fisier local pe server; iese cu stored_path (calea pe server)
static int upload_file(int sock, const char *local_path, char *stored_out)
{
    int fd = open(local_path, O_RDONLY);
    if (fd < 0) {
        (void)fprintf(stderr, "vps_remote: cannot open %s: %s\n",
                      local_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return -1;
    }

    proto_upload_t up;
    memset(&up, 0, sizeof(up));
    up.size_bytes = (uint64_t)st.st_size;
    char tmp[PROTO_MAX_PATH];
    (void)snprintf(tmp, sizeof(tmp), "%s", local_path);
    (void)snprintf(up.filename, sizeof(up.filename), "%s", basename(tmp));

    if (send_simple(sock, OPR_UPLOAD, &up, sizeof(up)) < 0) {
        (void)close(fd);
        return -1;
    }
    if (proto_send_file(sock, fd, up.size_bytes) < 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);

    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_upload_reply_t rr;
    if (proto_read_full(sock, &rr, sizeof(rr)) < 0) {
        return -1;
    }
    int32_t state = (int32_t)ntohl((uint32_t)rr.state);
    if (state != TASK_STATE_DONE) {
        (void)fprintf(stderr, "vps_remote: upload failed for %s\n", local_path);
        return -1;
    }
    (void)snprintf(stored_out, PROTO_MAX_PATH, "%s", rr.stored_path);
    (void)fprintf(stdout, "  uploaded: %s -> %s (%llu bytes)\n",
                  local_path, stored_out, (unsigned long long)up.size_bytes);
    return 0;
}

// asteapta ca task_id sa termine; pune calea finala in out_path
static int poll_until_done(int sock, uint32_t task_id, char *out_path)
{
    struct timespec sleep_spec = { .tv_sec = 0, .tv_nsec = POLL_SLEEP_NSEC };
    for (int attempts = 0; attempts < POLL_MAX_ATTEMPTS; attempts++) {
        proto_query_t query;
        memset(&query, 0, sizeof(query));
        query.task_id = htonl(task_id);
        if (send_simple(sock, OPR_STATUS, &query, sizeof(query)) < 0) {
            return -1;
        }
        proto_header_t rep;
        if (proto_read_header(sock, &rep) < 0) {
            return -1;
        }
        proto_status_t sts;
        if (proto_read_full(sock, &sts, sizeof(sts)) < 0) {
            return -1;
        }
        int32_t state = (int32_t)ntohl((uint32_t)sts.state);
        if (state == TASK_STATE_DONE) {
            (void)snprintf(out_path, PROTO_MAX_PATH, "%s", sts.result_path);
            return 0;
        }
        if (state == TASK_STATE_ERR) {
            return -1;
        }
        (void)nanosleep(&sleep_spec, NULL);
    }
    return -1;
}

static int download_file(int sock, const char *server_path, const char *local_path)
{
    proto_download_t req;
    memset(&req, 0, sizeof(req));
    (void)snprintf(req.path, sizeof(req.path), "%s", server_path);
    if (send_simple(sock, OPR_DOWNLOAD, &req, sizeof(req)) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_download_reply_t rr;
    if (proto_read_full(sock, &rr, sizeof(rr)) < 0) {
        return -1;
    }
    int32_t state = (int32_t)ntohl((uint32_t)rr.state);
    if (state != TASK_STATE_DONE) {
        (void)fprintf(stderr, "vps_remote: download refused\n");
        return -1;
    }
    int fd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE_DEFAULT);
    if (fd < 0) {
        (void)fprintf(stderr, "vps_remote: cannot write %s: %s\n",
                      local_path, strerror(errno));
        return -1;
    }
    int rc = proto_recv_file(sock, fd, rr.size_bytes);
    (void)close(fd);
    if (rc < 0) {
        return -1;
    }
    (void)fprintf(stdout, "  downloaded: %s -> %s (%llu bytes)\n",
                  server_path, local_path, (unsigned long long)rr.size_bytes);
    return 0;
}

// genereaza o cale server-side pt outputul ffmpeg (clientul nu vede outputs_dir,
// dar server-ul construieste o cale relativa la el; folosim un basename unic)
static void make_output_path(char *out, size_t out_sz, const char *suffix)
{
    (void)snprintf(out, out_sz, "./data/outputs/remote_%d_%s",
                   (int)getpid(), suffix);
}

// ---------- per-op flow ----------

static int op_trim(int sock, const cli_args_t *params)
{
    if (params->input == NULL) {
        (void)fprintf(stderr, "trim: -i input required\n");
        return -1;
    }
    char server_in[PROTO_MAX_PATH];
    if (upload_file(sock, params->input, server_in) < 0) {
        return -1;
    }

    proto_trim_t req;
    memset(&req, 0, sizeof(req));
    req.start_ms = params->start_ms;
    req.end_ms = params->end_ms;
    (void)snprintf(req.input, sizeof(req.input), "%s", server_in);
    make_output_path(req.output, sizeof(req.output), "trim.mp4");
    if (send_simple(sock, OPR_TRIM, &req, sizeof(req)) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_status_t sts;
    if (proto_read_full(sock, &sts, sizeof(sts)) < 0) {
        return -1;
    }
    uint32_t task_id = ntohl(sts.task_id);
    (void)fprintf(stdout, "  submitted trim task_id=%u\n", task_id);

    char final_path[PROTO_MAX_PATH];
    if (poll_until_done(sock, task_id, final_path) < 0) {
        (void)fprintf(stderr, "trim: task failed\n");
        return -1;
    }
    (void)fprintf(stdout, "  task done: %s\n", final_path);
    if (params->no_download != 0 || params->output_local == NULL) {
        return 0;
    }
    return download_file(sock, final_path, params->output_local);
}

static int op_filter(int sock, const cli_args_t *params)
{
    if (params->input == NULL || params->filter == NULL) {
        (void)fprintf(stderr, "filter: -i input and -f filter required\n");
        return -1;
    }
    char server_in[PROTO_MAX_PATH];
    if (upload_file(sock, params->input, server_in) < 0) {
        return -1;
    }
    proto_filter_t req;
    memset(&req, 0, sizeof(req));
    (void)snprintf(req.filter, sizeof(req.filter), "%s", params->filter);
    (void)snprintf(req.input, sizeof(req.input), "%s", server_in);
    make_output_path(req.output, sizeof(req.output), "filter.mp4");
    if (send_simple(sock, OPR_FILTER, &req, sizeof(req)) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_status_t sts;
    if (proto_read_full(sock, &sts, sizeof(sts)) < 0) {
        return -1;
    }
    uint32_t task_id = ntohl(sts.task_id);
    (void)fprintf(stdout, "  submitted filter task_id=%u\n", task_id);

    char final_path[PROTO_MAX_PATH];
    if (poll_until_done(sock, task_id, final_path) < 0) {
        return -1;
    }
    (void)fprintf(stdout, "  task done: %s\n", final_path);
    if (params->no_download != 0 || params->output_local == NULL) {
        return 0;
    }
    return download_file(sock, final_path, params->output_local);
}

static int op_merge(int sock, const cli_args_t *params)
{
    if (params->clip_count == 0) {
        (void)fprintf(stderr, "merge: at least one -c clip required\n");
        return -1;
    }
    proto_merge_t req;
    memset(&req, 0, sizeof(req));
    req.count = params->clip_count;
    (void)snprintf(req.transition, sizeof(req.transition), "%s",
                   params->transition != NULL ? params->transition : "");
    for (uint32_t i = 0; i < params->clip_count; i++) {
        char server_path[PROTO_MAX_PATH];
        if (upload_file(sock, params->clips_local[i], server_path) < 0) {
            return -1;
        }
        (void)snprintf(req.clips[i], sizeof(req.clips[i]), "%s", server_path);
    }
    make_output_path(req.output, sizeof(req.output), "merge.mp4");
    if (send_simple(sock, OPR_MERGE, &req, sizeof(req)) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_status_t sts;
    if (proto_read_full(sock, &sts, sizeof(sts)) < 0) {
        return -1;
    }
    uint32_t task_id = ntohl(sts.task_id);
    (void)fprintf(stdout, "  submitted merge task_id=%u\n", task_id);

    char final_path[PROTO_MAX_PATH];
    if (poll_until_done(sock, task_id, final_path) < 0) {
        return -1;
    }
    (void)fprintf(stdout, "  task done: %s\n", final_path);
    if (params->no_download != 0 || params->output_local == NULL) {
        return 0;
    }
    return download_file(sock, final_path, params->output_local);
}

static int op_mixaudio(int sock, const cli_args_t *params)
{
    if (params->input == NULL || params->audio == NULL) {
        (void)fprintf(stderr, "mixaudio: -i video and -a audio required\n");
        return -1;
    }
    char server_v[PROTO_MAX_PATH];
    char server_a[PROTO_MAX_PATH];
    if (upload_file(sock, params->input, server_v) < 0) {
        return -1;
    }
    if (upload_file(sock, params->audio, server_a) < 0) {
        return -1;
    }
    proto_mixaudio_t req;
    memset(&req, 0, sizeof(req));
    (void)snprintf(req.input_video, sizeof(req.input_video), "%s", server_v);
    (void)snprintf(req.input_audio, sizeof(req.input_audio), "%s", server_a);
    make_output_path(req.output, sizeof(req.output), "mix.mp4");
    if (send_simple(sock, OPR_MIXAUDIO, &req, sizeof(req)) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_status_t sts;
    if (proto_read_full(sock, &sts, sizeof(sts)) < 0) {
        return -1;
    }
    uint32_t task_id = ntohl(sts.task_id);
    (void)fprintf(stdout, "  submitted mixaudio task_id=%u\n", task_id);

    char final_path[PROTO_MAX_PATH];
    if (poll_until_done(sock, task_id, final_path) < 0) {
        return -1;
    }
    (void)fprintf(stdout, "  task done: %s\n", final_path);
    if (params->no_download != 0 || params->output_local == NULL) {
        return 0;
    }
    return download_file(sock, final_path, params->output_local);
}

int main(int argc, char **argv)
{
    cli_args_t args;
    memset(&args, 0, sizeof(args));
    args.host = DEFAULT_HOST;
    args.port = DEFAULT_PORT;
    args.end_ms = DEFAULT_END_MS;

    int opt;
    while ((opt = getopt(argc, argv, "H:P:o:i:a:c:O:f:T:s:e:Nh")) != -1) {
        switch (opt) {
            case 'H': args.host = optarg; break;
            case 'P': args.port = parse_int_safe(optarg, DEFAULT_PORT); break;
            case 'o': args.op = optarg; break;
            case 'i': args.input = optarg; break;
            case 'a': args.audio = optarg; break;
            case 'c':
                if (args.clip_count < PROTO_MAX_CLIPS) {
                    (void)snprintf(args.clips_local[args.clip_count],
                                   PROTO_MAX_PATH, "%s", optarg);
                    args.clip_count++;
                }
                break;
            case 'O': args.output_local = optarg; break;
            case 'f': args.filter = optarg; break;
            case 'T': args.transition = optarg; break;
            case 's': args.start_ms = (uint32_t)parse_int_safe(optarg, 0); break;
            case 'e': args.end_ms = (uint32_t)parse_int_safe(optarg, DEFAULT_END_MS); break;
            case 'N': args.no_download = 1; break;
            case 'h': usage(argv[0]); return EXIT_SUCCESS;
            default:  usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (args.op == NULL) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int sock = dial(args.host, args.port);
    if (sock < 0) {
        (void)fprintf(stderr, "vps_remote: cannot connect to %s:%d (%s)\n",
                      args.host, args.port, strerror(errno));
        return EXIT_FAILURE;
    }

    int rc = -1;
    if (strcmp(args.op, "trim") == 0) {
        rc = op_trim(sock, &args);
    } else if (strcmp(args.op, "filter") == 0) {
        rc = op_filter(sock, &args);
    } else if (strcmp(args.op, "merge") == 0) {
        rc = op_merge(sock, &args);
    } else if (strcmp(args.op, "mixaudio") == 0) {
        rc = op_mixaudio(sock, &args);
    } else {
        (void)fprintf(stderr, "vps_remote: unknown op '%s'\n", args.op);
    }

    (void)close(sock);
    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
