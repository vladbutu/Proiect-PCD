/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - REST shim minimal peste worker/video_ops
 * Ce face programul:
 * - expune endpoint-uri HTTP simple (health + operatii video)
 * - mapeaza query params in structurile interne proto_*_t
 * - reutilizeaza motorul existent (video_ops + worker fork/execv)
 * - raspunde JSON, fara framework extern HTTP
 * Acest binar nu inlocuieste protocolul TCP existent, il completeaza.
 * Erori tratate explicit:
 * - request invalid / endpoint necunoscut -> 400 JSON
 * - parametri lipsa sau invalizi -> 400 JSON
 * - esec worker/ffmpeg -> 400 JSON
 * - open/bind/listen/accept esuate -> exit cu eroare
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "proto.h" // constante si structuri proto_*_t
#include "getopt_compat.h" // declaratii getopt/optarg explicite
#include "video_ops.h" // buildere argv ffmpeg + video_init
#include "worker.h" // worker_spawn/worker_wait/dispatch_merge

#include <ctype.h> // isxdigit pt %XX decoding in query string
#include <errno.h> // errno pt open/bind/listen/accept/strtol
#include <fcntl.h> // open flags pt /upload si /download
#include <sys/stat.h> // mkdir, fstat pt /upload si /download
#include <libconfig.h> // config_t, config_read_file -- lib extern obligatoriu
#include <netinet/in.h> // sockaddr_in, htons, htonl pt IPv4 listen socket
#include <stdint.h> // uint32_t pt mapare in structurile proto_*_t
#include <stdio.h> // fprintf, snprintf pt output si raspunsuri
#include <stdlib.h> // strtol, EXIT_SUCCESS/EXIT_FAILURE
#include <string.h> // memset, strchr, strcmp, strncmp, strtok_r
#include <sys/socket.h> // socket, bind, listen, accept, setsockopt
#include <sys/types.h> // tipuri POSIX pt socket API
#include <unistd.h> // read, write, close

#define REST_DEFAULT_PORT 18082 // port implicit pt shim-ul HTTP
#define REST_BACKLOG 16 // coada maxima de conexiuni in listen()
#define REQ_BUF_SZ 8192 // buffer brut pentru request-ul HTTP
#define RESP_BUF_SZ 1024 // buffer pentru headere/raspuns JSON simplu
#define METHOD_BUF_SZ 8 // buffer pt metoda HTTP (GET/POST)
#define TARGET_BUF_SZ 1024 // buffer pt target-ul din request line
#define PARSE_INT_BASE 10 // baza zecimala pt strtol
#define PARSE_INT_MIN 1 // valoare minima valida pt porturi/numere pozitive
#define PARSE_INT_MAX 65535 // valoare maxima valida pt port IPv4/uint16
#define HEX_BASE 16 // baza folosita la decode %XX in URL
#define TMP_BUF_SZ 128 // buffer temporar pt query params numerici
#define HTTP_OK 200 // cod HTTP succes
#define HTTP_BAD_REQUEST 400 // cod HTTP request invalid
#define END_MS_BUF_OFFSET 64 // offset in bufferul temporar pentru al doilea numar
#define UPLOAD_CHUNK 65536 // chunk size pentru streaming /upload + /download
#define DIR_MODE 0755 // permisiuni dir create de /upload
#define FILE_MODE 0644 // permisiuni fisier create de /upload

typedef struct rest_cfg {
    int listen_port; // portul pe care asculta shim-ul REST
    video_ctx_t video; // context video reutilizat de worker/video_ops
} rest_cfg_t;

// afiseaza utilizarea CLI pentru shim-ul REST
static void usage(const char *argv0)
{
    (void)fprintf(stderr,
        "Usage: %s [-c config/server.conf] [-p port]\n"
    "  -c <file> path config (default: config/server.conf)\n"
    "  -p <port> override REST listen port (default: 18082)\n"
    "  -h help\n",
        argv0);
}

// parseaza robust un int (strtol + validari), altfel intoarce fallback
static int parse_int(const char *str, int fallback)
{
    char *end = NULL;
    long val;

    if (str == NULL || *str == '\0') {
        return fallback;
    }

    errno = 0;
    val = strtol(str, &end, PARSE_INT_BASE);
    if (errno != 0 || end == str || *end != '\0' ||val < PARSE_INT_MIN || val > PARSE_INT_MAX) {
        return fallback;
    }

    return (int)val;
}

// valori implicite cand lipsesc din config sau din CLI
static void set_defaults(rest_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = REST_DEFAULT_PORT;
    cfg->video.merge_parallelism = 4;
    (void)snprintf(cfg->video.ffmpeg_binary, sizeof(cfg->video.ffmpeg_binary), "%s", "/usr/bin/ffmpeg");
    (void)snprintf(cfg->video.uploads_dir, sizeof(cfg->video.uploads_dir), "%s", "./data/uploads");
    (void)snprintf(cfg->video.outputs_dir, sizeof(cfg->video.outputs_dir), "%s", "./data/outputs");
}

// citeste configuratia din libconfig; la erori ramane pe default-uri
static void load_config(rest_cfg_t *cfg, const char *path)
{
    config_t config_obj;
    const char *str_val = NULL;
    int int_val = 0;

    config_init(&config_obj);
    if (config_read_file(&config_obj, path) == CONFIG_FALSE) {
        config_destroy(&config_obj);
        return;
    }

    if (config_lookup_int(&config_obj, "server.merge_parallelism", &int_val) == CONFIG_TRUE && int_val > 0) {
        cfg->video.merge_parallelism = int_val;
    }
    if (config_lookup_string(&config_obj, "storage.ffmpeg_binary", &str_val) == CONFIG_TRUE) {
        (void)snprintf(cfg->video.ffmpeg_binary, sizeof(cfg->video.ffmpeg_binary), "%s", str_val);
    }
    if (config_lookup_string(&config_obj, "storage.uploads_dir", &str_val) == CONFIG_TRUE) {
        (void)snprintf(cfg->video.uploads_dir, sizeof(cfg->video.uploads_dir), "%s", str_val);
    }
    if (config_lookup_string(&config_obj, "storage.outputs_dir", &str_val) == CONFIG_TRUE) {
        (void)snprintf(cfg->video.outputs_dir, sizeof(cfg->video.outputs_dir), "%s", str_val);
    }

    config_destroy(&config_obj);
}

// deschide socket de listen IPv4 pentru serverul REST
static int open_listener(int port)
{
    int sfd;
    int yes = 1;
    struct sockaddr_in addr;

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        return -1;
    }

    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        (void)close(sfd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(sfd);
        return -1;
    }
    if (listen(sfd, REST_BACKLOG) < 0) {
        (void)close(sfd);
        return -1;
    }

    return sfd;
}

// decode minim pentru query string (%XX si +)
static void url_decode(char *encoded)
{
    char *src = encoded;
    char *dst = encoded;

    while (*src != '\0') {
        if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3];
            hex[0] = src[1];
            hex[1] = src[2];
            hex[2] = '\0';
            *dst++ = (char)strtol(hex, NULL, HEX_BASE);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

// extrage un parametru key=value din query string
static int query_get(char *query, const char *key, char *out, size_t out_sz)
{
    char *cur;
    size_t klen;

    if (query == NULL || key == NULL || out == NULL || out_sz == 0) {
        return -1;
    }

    klen = strlen(key);
    cur = query;

    while (cur != NULL && *cur != '\0') {
        char *amp = strchr(cur, '&');
        char *eq = strchr(cur, '=');
        size_t seg_len;

        if (amp != NULL) {
            *amp = '\0';
        }

        if (eq != NULL && (size_t)(eq - cur) == klen && strncmp(cur, key, klen) == 0) {
            (void)snprintf(out, out_sz, "%s", eq + 1);
            url_decode(out);
            if (amp != NULL) {
                *amp = '&';
            }
            return 0;
        }

        seg_len = strlen(cur);
        (void)seg_len;
        if (amp != NULL) {
            *amp = '&';
            cur = amp + 1;
        } else {
            break;
        }
    }

    return -1;
}

// trimite raspuns JSON simplu, cu Connection: close
static void send_json(int cfd, int code, const char *json)
{
    char hdr[RESP_BUF_SZ];
    const char *status = (code == HTTP_OK) ? "OK" : "Bad Request";
    size_t len = strlen(json);

    (void) snprintf ( hdr, sizeof ( hdr ), "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", code, status, len );
    (void)write(cfd, hdr, strlen(hdr));
    (void)write(cfd, json, len);
}

// endpoint logic pentru /trim
static int run_trim(const video_ctx_t *ctx, char *query, char *out_path)
{
    proto_trim_t req;
    char tmp[TMP_BUF_SZ];
    char **argv;
    pid_t pid;

    memset(&req, 0, sizeof(req));
    if (query_get(query, "input", req.input, sizeof(req.input)) < 0 ||
        query_get(query, "output", req.output, sizeof(req.output)) < 0 ||
        query_get(query, "start_ms", tmp, sizeof(tmp)) < 0 ||
        query_get(query, "end_ms", tmp + END_MS_BUF_OFFSET, sizeof(tmp) - END_MS_BUF_OFFSET) < 0) {
        return -1;
    }

    req.start_ms = (uint32_t)parse_int(tmp, 0);
    req.end_ms = (uint32_t)parse_int(tmp + END_MS_BUF_OFFSET, 0);

    argv = video_build_trim_argv(ctx, &req);
    if (argv == NULL) {
        return -1;
    }
    pid = worker_spawn(ctx, argv);
    video_argv_free(argv);
    if (pid <= 0) {
        return -1;
    }

    (void)snprintf(out_path, PROTO_MAX_PATH, "%s", req.output);
    return worker_wait(pid);
}

// endpoint logic pentru /filter
static int run_filter(const video_ctx_t *ctx, char *query, char *out_path)
{
    proto_filter_t req;
    char **argv;
    pid_t pid;

    memset(&req, 0, sizeof(req));
    if (query_get(query, "input", req.input, sizeof(req.input)) < 0 ||
        query_get(query, "output", req.output, sizeof(req.output)) < 0 ||
        query_get(query, "filter", req.filter, sizeof(req.filter)) < 0) {
        return -1;
    }

    argv = video_build_filter_argv(ctx, &req);
    if (argv == NULL) {
        return -1;
    }
    pid = worker_spawn(ctx, argv);
    video_argv_free(argv);
    if (pid <= 0) {
        return -1;
    }

    (void)snprintf(out_path, PROTO_MAX_PATH, "%s", req.output);
    return worker_wait(pid);
}

// endpoint logic pentru /mixaudio
static int run_mixaudio(const video_ctx_t *ctx, char *query, char *out_path)
{
    proto_mixaudio_t req;
    char **argv;
    pid_t pid;

    memset(&req, 0, sizeof(req));
    if (query_get(query, "input_video", req.input_video, sizeof(req.input_video)) < 0
        || query_get(query, "input_audio", req.input_audio, sizeof(req.input_audio)) < 0
        || query_get(query, "output", req.output, sizeof(req.output)) < 0) {
        return -1;
    }

    argv = video_build_mixaudio_argv(ctx, &req);
    if (argv == NULL) {
        return -1;
    }
    pid = worker_spawn(ctx, argv);
    video_argv_free(argv);
    if (pid <= 0) {
        return -1;
    }

    (void)snprintf(out_path, PROTO_MAX_PATH, "%s", req.output);
    return worker_wait(pid);
}

// endpoint logic pentru /merge (clips separate prin virgula)
static int run_merge(const video_ctx_t *ctx, char *query, char *out_path)
{
    proto_merge_t req;
    char clips_buf[PROTO_MAX_PATH * 2];
    char *token;
    char *saveptr = NULL;
    uint32_t clip_count = 0;

    memset(&req, 0, sizeof(req));
    if (query_get(query, "output", req.output, sizeof(req.output)) < 0
        || query_get(query, "transition", req.transition, sizeof(req.transition)) < 0
        || query_get(query, "clips", clips_buf, sizeof(clips_buf)) < 0) {
        return -1;
    }

    token = strtok_r(clips_buf, ",", &saveptr);
    while (token != NULL && clip_count < PROTO_MAX_CLIPS) {
        (void)snprintf(req.clips[clip_count], sizeof(req.clips[clip_count]), "%s", token);
        clip_count++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    req.count = clip_count;
    if (req.count == 0) {
        return -1;
    }

    (void)snprintf(out_path, PROTO_MAX_PATH, "%s", req.output);
    return worker_dispatch_merge(ctx, &req);
}

// extrage Content-Length din buffer-ul de request. -1 daca lipseste/invalid.
// nu pot folosi parse_int() pt ca acela cere *end='\0', dar aici urmeaza \r\n.
static long parse_content_length(const char *req)
{
    static const char hdr_name[] = "Content-Length:";
    const char *cl = strcasestr(req, hdr_name);
    if (cl == NULL) {
        return -1;
    }
    const char *num = cl + (sizeof(hdr_name) - 1);
    char *end = NULL;
    errno = 0;
    long val = strtol(num, &end, PARSE_INT_BASE);
    if (errno != 0 || end == num || val < 0) {
        return -1;
    }
    return val;
}

// gaseste inceputul body-ului HTTP (dupa \r\n\r\n). Returneaza NULL daca lipseste.
static const char *find_body(const char *req)
{
    const char *pos = strstr(req, "\r\n\r\n");
    return (pos != NULL) ? (pos + 4) : NULL;
}

// sanitize basename pt path: doar litere/cifre/._- ; refuza .. si /
static int safe_basename(const char *raw, char *out, size_t out_sz)
{
    if (raw == NULL || raw[0] == '\0') {
        return -1;
    }
    for (const char *pc = raw; *pc; pc++) {
        if (*pc == '/' || *pc == '\\') {
            return -1;
        }
        if (*pc == '.' && pc[1] == '.') {
            return -1;
        }
    }
    (void)snprintf(out, out_sz, "%s", raw);
    return 0;
}

// POST /upload?path=name.bin -- raw body in fisier in <uploads_dir>/incoming/
static int run_upload(const video_ctx_t *ctx, char *query, const char *req_buf,
                      ssize_t already_read, int cfd, char *stored_out)
{
    char fname[PROTO_MAX_PATH];
    if (query_get(query, "path", fname, sizeof(fname)) < 0) {
        return -1;
    }
    char safe[PROTO_MAX_PATH];
    if (safe_basename(fname, safe, sizeof(safe)) < 0) {
        return -1;
    }
    long clen = parse_content_length(req_buf);
    if (clen < 0) {
        return -1;
    }
    const char *body = find_body(req_buf);
    if (body == NULL) {
        return -1;
    }

    char dest_dir[PROTO_MAX_PATH];
    (void)snprintf(dest_dir, sizeof(dest_dir), "%.400s/incoming", ctx->uploads_dir);
    (void)mkdir(ctx->uploads_dir, DIR_MODE);
    (void)mkdir(dest_dir, DIR_MODE);

    char dest_path[PROTO_MAX_PATH];
    (void)snprintf(dest_path, sizeof(dest_path), "%.400s/rest_%d_%.80s",
                   dest_dir, (int)getpid(), safe);
    int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
    if (fd < 0) {
        return -1;
    }

    size_t header_len = (size_t)(body - req_buf);
    ssize_t body_in_buf = already_read - (ssize_t)header_len;
    if (body_in_buf > 0) {
        if (write(fd, body, (size_t)body_in_buf) < 0) {
            (void)close(fd);
            return -1;
        }
    }
    long left = clen - (body_in_buf > 0 ? body_in_buf : 0);
    char buf[UPLOAD_CHUNK];
    while (left > 0) {
        size_t want = (left > (long)sizeof(buf)) ? sizeof(buf) : (size_t)left;
        ssize_t nread = read(cfd, buf, want);
        if (nread <= 0) {
            (void)close(fd);
            return -1;
        }
        if (write(fd, buf, (size_t)nread) < 0) {
            (void)close(fd);
            return -1;
        }
        left -= nread;
    }
    (void)close(fd);
    (void)snprintf(stored_out, PROTO_MAX_PATH, "%s", dest_path);
    return 0;
}

// GET /download?path=foo.mp4 -- citeste fisierul din outputs_dir si streameaza pe socket
static int run_download(const video_ctx_t *ctx, char *query, int cfd)
{
    char fname[PROTO_MAX_PATH];
    if (query_get(query, "path", fname, sizeof(fname)) < 0) {
        return -1;
    }
    char safe[PROTO_MAX_PATH];
    if (safe_basename(fname, safe, sizeof(safe)) < 0) {
        return -1;
    }
    char src_path[PROTO_MAX_PATH];
    (void)snprintf(src_path, sizeof(src_path), "%.400s/%.100s", ctx->outputs_dir, safe);
    int fd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return -1;
    }
    char hdr[RESP_BUF_SZ];
    (void)snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n\r\n",
        (long long)st.st_size, safe);
    (void)write(cfd, hdr, strlen(hdr));
    char buf[UPLOAD_CHUNK];
    off_t left = st.st_size;
    while (left > 0) {
        size_t want = (left > (off_t)sizeof(buf)) ? sizeof(buf) : (size_t)left;
        ssize_t nread = read(fd, buf, want);
        if (nread <= 0) {
            break;
        }
        if (write(cfd, buf, (size_t)nread) < 0) {
            (void)close(fd);
            return -1;
        }
        left -= nread;
    }
    (void)close(fd);
    return 0;
}

// trateaza un client HTTP; request minim (metoda + target), fara keep-alive
static void handle_client(int cfd, const video_ctx_t *ctx)
{
    char req[REQ_BUF_SZ];
    char method[METHOD_BUF_SZ];
    char target[TARGET_BUF_SZ];
    char *path;
    char *query;
    char out_path[PROTO_MAX_PATH];
    ssize_t nread;
    int ret = -1;

    memset(req, 0, sizeof(req));
    nread = read(cfd, req, sizeof(req) - 1);
    if (nread <= 0) {
        return;
    }

    if (sscanf(req, "%7s %1023s", method, target) != 2) {
        send_json(cfd, HTTP_BAD_REQUEST, "{\"status\":\"error\",\"message\":\"bad request\"}");
        return;
    }

    path = target;
    query = strchr(target, '?');
    if (query != NULL) {
        *query = '\0';
        query++;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        send_json(cfd, HTTP_OK, "{\"status\":\"ok\",\"service\":\"vps_rest\"}");
        return;
    }

    // /download e GET cu query, separat de POST-urile de procesare
    if (strcmp(method, "GET") == 0 && strcmp(path, "/download") == 0 && query != NULL) {
        if (run_download(ctx, query, cfd) < 0) {
            send_json(cfd, HTTP_BAD_REQUEST, "{\"status\":\"error\",\"message\":\"file not found\"}");
        }
        return;
    }

    if (strcmp(method, "POST") != 0 || query == NULL) {
        send_json(cfd, HTTP_BAD_REQUEST, "{\"status\":\"error\",\"message\":\"unsupported endpoint\"}");
        return;
    }

    // /upload streameaza body raw; nu trece prin runner-ele ffmpeg
    if (strcmp(path, "/upload") == 0) {
        char stored[PROTO_MAX_PATH];
        if (run_upload(ctx, query, req, nread, cfd, stored) < 0) {
            send_json(cfd, HTTP_BAD_REQUEST, "{\"status\":\"error\",\"message\":\"upload failed\"}");
            return;
        }
        char json[RESP_BUF_SZ];
        (void)snprintf(json, sizeof(json), "{\"status\":\"ok\",\"stored\":\"%s\"}", stored);
        send_json(cfd, HTTP_OK, json);
        return;
    }

    memset(out_path, 0, sizeof(out_path));
    if (strcmp(path, "/trim") == 0) {
        ret = run_trim(ctx, query, out_path);
    } else if (strcmp(path, "/filter") == 0) {
        ret = run_filter(ctx, query, out_path);
    } else if (strcmp(path, "/mixaudio") == 0) {
        ret = run_mixaudio(ctx, query, out_path);
    } else if (strcmp(path, "/merge") == 0) {
        ret = run_merge(ctx, query, out_path);
    } else {
        send_json(cfd, HTTP_BAD_REQUEST,
            "{\"status\":\"error\",\"message\":\"unknown path\"}");
        return;
    }

    if (ret == 0) {
        char json[RESP_BUF_SZ];
        (void)snprintf(json, sizeof(json), "{\"status\":\"ok\",\"output\":\"%s\"}", out_path);
        send_json(cfd, HTTP_OK, json);
    } else {
        send_json(cfd, HTTP_BAD_REQUEST, "{\"status\":\"error\",\"message\":\"processing failed\"}");
    }
}

int main(int argc, char **argv)
{
    const char *cfg_path = "config/server.conf";
    rest_cfg_t cfg;
    int sfd;
    int cli_opt;

    set_defaults(&cfg);

    while ((cli_opt = getopt(argc, argv, "c:p:h")) != -1) {
        switch (cli_opt) {
            case 'c':
                cfg_path = optarg;
                break;
            case 'p':
                cfg.listen_port = parse_int(optarg, REST_DEFAULT_PORT);
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    load_config(&cfg, cfg_path);

    if (video_init() < 0) {
        (void)fprintf(stderr, "vps_rest: video_init failed\n");
        return 1;
    }

    sfd = open_listener(cfg.listen_port);
    if (sfd < 0) {
        (void)fprintf(stderr, "vps_rest: listen failed on port %d\n", cfg.listen_port);
        return 1;
    }

    (void)fprintf(stderr, "vps_rest: listening on port %d\n", cfg.listen_port);
    // model simplu, secvential: accept -> handle -> close
    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        handle_client(cfd, &cfg.video);
        (void)close(cfd);
    }

    (void)close(sfd);
    return 0;
}

/*
 *> Compilare si exemple de rular:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/rest_server.c -o build/rest_server.o
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude build/rest_server.o build/video_ops.o build/worker.o -o bin/vps_rest -lconfig -lavformat -lavcodec -lavutil -lavfilter
 *
 * --- Rulare cu succes ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ nohup bin/vps_rest -p 18082 >/tmp/final_vps_rest.log 2>&1 &
 * [1] 18025
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ curl -sS http://127.0.0.1:18082/health
 * {"status":"ok","service":"vps_rest"}
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ curl -sS -X POST "http://127.0.0.1:18082/trim?input=data/uploads/clip1.mp4&output=data/outputs/final_rest_trimmed.mp4&start_ms=0&end_ms=1000"
 * {"status":"ok","output":"data/outputs/final_rest_trimmed.mp4"}
 *
 * --- Rulare cu esec ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ curl -sS -X POST http://127.0.0.1:18082/trim
 * {"status":"error","message":"unsupported endpoint"}
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_rest -p 18082
 * vps_rest: listen failed on port 18082
 */
