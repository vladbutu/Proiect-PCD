/**
 * Echipa 11 · IR3 2026 · PCD
 * vps_admin: clientul de administrare (UX, fara transfer fisiere)
 * Operatii suportate:
 *   - ping               health-check rapid
 *   - status <task_id>   interogheaza starea unui job
 *   - list               afiseaza tabela curenta de joburi
 *   - stats              contoare server (uptime, jobs done/failed)
 *   - shutdown           cere serverului sa iasa gracios
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "proto.h" // proto_header_t, proto_status_t, proto_query_t, OPR_*
#include "getopt_compat.h" // declaratii explicite getopt/optarg/optind

#include <errno.h> // errno pt strerror la connect esuat
#include <limits.h> // INT_MAX pt validare strtol
#include <netdb.h> // getaddrinfo / freeaddrinfo (thread-safe DNS)
#include <netinet/in.h> // ntohl pt conversia campurilor protocolului
#include <stdint.h> // uint32_t, int32_t pt task_id si state
#include <stdio.h> // fprintf, snprintf pt output catre user
#include <stdlib.h> // EXIT_SUCCESS/EXIT_FAILURE, strtol
#include <string.h> // memset, strcmp, strerror pt CLI dispatch
#include <sys/socket.h> // socket, connect pt TCP catre vps_server
#include <unistd.h> // close, getpid (task_id local)

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 18081
#define PORT_STR_LEN 16
#define STRTOL_BASE_DEC 10

typedef struct cli_args {
    const char *host;
    int port;
    const char *cmd;
    uint32_t task_id;
} cli_args_t;

static void usage(const char *argv0)
{
    (void)fprintf(stderr,
        "vps_admin (Echipa 11) - client de administrare\n"
        "Usage: %s [-H host] [-P port] <command> [args]\n"
        "Commands:\n"
        "  ping              health-check rapid\n"
        "  status <task_id>  raporteaza starea unui task\n"
        "  list              afiseaza joburile din tabela serverului\n"
        "  stats             contoare server (uptime, done, failed)\n"
        "  shutdown          opreste serverul (gracios)\n"
        "Options:\n"
        "  -H <host>         host server (default: %s)\n"
        "  -P <port>         port server (default: %d)\n",
        argv0, DEFAULT_HOST, DEFAULT_PORT);
}

static int dial(const cli_args_t *args)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[PORT_STR_LEN];
    (void)snprintf(port_str, sizeof(port_str), "%d", args->port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(args->host, port_str, &hints, &res) != 0) {
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

static const char *state_label(int32_t st)
{
    switch (st) {
        case TASK_STATE_QUEUED:  return "queued";
        case TASK_STATE_RUNNING: return "running";
        case TASK_STATE_DONE:    return "done";
        case TASK_STATE_ERR:     return "error";
        default:                 return "?";
    }
}

static const char *op_label(uint32_t op)
{
    switch (op) {
        case OPR_TRIM:     return "trim";
        case OPR_FILTER:   return "filter";
        case OPR_MERGE:    return "merge";
        case OPR_MIXAUDIO: return "mixaudio";
        default:           return "?";
    }
}

static int cmd_ping(int sock)
{
    if (send_simple(sock, OPR_PING, NULL, 0) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    (void)fprintf(stdout, "pong (op=%u)\n", rep.op_id);
    return 0;
}

static int cmd_status(int sock, uint32_t task_id)
{
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
    uint32_t tid = ntohl(sts.task_id);
    (void)fprintf(stdout, "task_id=%u state=%s path=%s\n",
                  tid, state_label(state), sts.result_path);
    return 0;
}

static int cmd_list(int sock)
{
    if (send_simple(sock, OPR_LIST_JOBS, NULL, 0) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_list_jobs_reply_t rl;
    if (proto_read_full(sock, &rl, sizeof(rl)) < 0) {
        return -1;
    }
    uint32_t cnt = ntohl(rl.count);
    (void)fprintf(stdout, "jobs (count=%u):\n", cnt);
    for (uint32_t i = 0; i < cnt && i < PROTO_LIST_MAX_JOBS; i++) {
        uint32_t tid = ntohl(rl.jobs[i].task_id);
        uint32_t op = ntohl(rl.jobs[i].op_id);
        int32_t st = (int32_t)ntohl((uint32_t)rl.jobs[i].state);
        (void)fprintf(stdout, "  [%u] op=%s state=%s output=%s\n",
                      tid, op_label(op), state_label(st), rl.jobs[i].output);
    }
    return 0;
}

static int cmd_stats(int sock)
{
    if (send_simple(sock, OPR_STATS, NULL, 0) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    proto_stats_reply_t rs;
    if (proto_read_full(sock, &rs, sizeof(rs)) < 0) {
        return -1;
    }
    (void)fprintf(stdout,
        "uptime=%us queued=%u running=%u done=%u failed=%u\n",
        ntohl(rs.uptime_sec), ntohl(rs.jobs_queued),
        ntohl(rs.jobs_running), ntohl(rs.jobs_done),
        ntohl(rs.jobs_failed));
    return 0;
}

static int cmd_shutdown(int sock)
{
    if (send_simple(sock, OPR_SHUTDOWN, NULL, 0) < 0) {
        return -1;
    }
    proto_header_t rep;
    if (proto_read_header(sock, &rep) < 0) {
        return -1;
    }
    (void)fprintf(stdout, "shutdown ack (op=%u)\n", rep.op_id);
    return 0;
}

static int parse_int_safe(const char *str, int fallback)
{
    if (str == NULL || *str == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(str, &end, STRTOL_BASE_DEC);
    if (errno != 0 || end == str || *end != '\0' || val < 0 || val > INT_MAX) {
        return fallback;
    }
    return (int)val;
}

int main(int argc, char **argv)
{
    cli_args_t args;
    memset(&args, 0, sizeof(args));
    args.host = DEFAULT_HOST;
    args.port = DEFAULT_PORT;

    int opt;
    while ((opt = getopt(argc, argv, "H:P:h")) != -1) {
        switch (opt) {
            case 'H': args.host = optarg; break;
            case 'P': args.port = parse_int_safe(optarg, DEFAULT_PORT); break;
            case 'h': usage(argv[0]); return EXIT_SUCCESS;
            default:  usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    args.cmd = argv[optind++];
    if (strcmp(args.cmd, "status") == 0) {
        if (optind >= argc) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        args.task_id = (uint32_t)parse_int_safe(argv[optind], 0);
    }

    int sock = dial(&args);
    if (sock < 0) {
        (void)fprintf(stderr, "vps_admin: cannot connect to %s:%d (%s)\n",
                      args.host, args.port, strerror(errno));
        return EXIT_FAILURE;
    }

    int rc = -1;
    if (strcmp(args.cmd, "ping") == 0) {
        rc = cmd_ping(sock);
    } else if (strcmp(args.cmd, "status") == 0) {
        rc = cmd_status(sock, args.task_id);
    } else if (strcmp(args.cmd, "list") == 0) {
        rc = cmd_list(sock);
    } else if (strcmp(args.cmd, "stats") == 0) {
        rc = cmd_stats(sock);
    } else if (strcmp(args.cmd, "shutdown") == 0) {
        rc = cmd_shutdown(sock);
    } else {
        (void)fprintf(stderr, "vps_admin: unknown command '%s'\n", args.cmd);
        usage(argv[0]);
    }

    (void)close(sock);
    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
