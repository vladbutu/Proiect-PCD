/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Entry point server Video Processing Server
 * Ce face programul:
 * - parseaza optiunile din linia de comanda cu getopt (-c config, -p port, -v verbose, -h help)
 * - incarca configuratia din fisierul libconfig (config/server.conf) -- lib extern obligatoriu
 * - initializeaza libavformat (video_init) pt probing metadate video
 * - preda controlul buclei poll() din net_server_run(), care face fork() pt fiecare task
 * Erori tratate explicit:
 * - optiuni invalide sau lipsa pe linia de comanda
 * - fisier de configurare lipsa sau cu erori de sintaxa
 * - campuri lipsa in config (fallback pe valori implicite)
 * - conversie numerica esuata (strtol in loc de atoi)
 * - initializare libav esuata
 */

#include "net_server.h" // server_cfg_t + net_server_run() -- bucla poll()
#include "video_ops.h" // video_ctx_t + video_init() -- init FFmpeg
#include "getopt_compat.h" // declaratii explicite getopt/optarg

#include <libconfig.h> // config_t, config_read_file etc. -- lib extern obligatoriu

#include <errno.h> // errno pt validari erori
#include <limits.h> // INT_MAX/INT_MIN pt validare overflow la strtol
#include <stdint.h> // tipuri fixe (uint32_t etc.)
#include <stdio.h> // fprintf, snprintf pt output/erori
#include <stdlib.h> // EXIT_SUCCESS/EXIT_FAILURE, strtol, getenv
#include <string.h> // snprintf, memset pt operatii string

#define DEFAULT_CONFIG_PATH  "config/server.conf" // calea implicita pt fisierul de configurare
#define DEFAULT_PORT 18081 // portul implicit daca nu e in config
#define DEFAULT_MAX_CLIENTS 64 // nr maxim de clienti concurenti
#define DEFAULT_MAX_WORKERS 8 // nr maxim de workeri 
#define DEFAULT_MERGE_PAR 4 // paralelism implicit la merge
#define STRTOL_BASE_DEC 10 // baza 10 pt strtol, sa nu am magic number
#define STRERR_BUF_LEN 256 // buffer pt strerror_r

// structura care grupeaza toate sursele de configurare
// (evit parametri multipli cu tipuri usor de incurcat -- bugprone-easily-swappable-parameters)
typedef struct cli_opts {
    const char *config_path; // calea la fisierul libconfig
    int override_port; // port dat pe linia de comanda (0 = nu s-a dat)
    int verbose; // mod verbose (afiseaza info suplimentare pe stderr)
} cli_opts_t;

static void usage (const char *argv0)
{
    (void) fprintf ( stderr,
        "T5 Video Processing Server\n"
        "Usage: %s [options]\n"
        "  -c <file> path to libconfig configuration (default: %s)\n"
        "  -p <port> override listen port from the config file\n"
        "  -v        verbose logging to stderr\n"
        "  -h        show this help message\n",
        argv0, DEFAULT_CONFIG_PATH );
}

// strtol in loc de atoi ca sa pot prinde erori de format/overflow
// daca conversia pica, returnez fallback
static int parse_int(const char *str, int fallback)
{
    if (str == NULL || *str == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(str, &end, STRTOL_BASE_DEC);
    if (errno != 0 || end == str || *end != '\0' || val > INT_MAX || val < INT_MIN) {
        return fallback;
    }
    return (int)val;
}

static int parse_cli(int argc, char **argv, cli_opts_t *out)
{
    out->config_path = DEFAULT_CONFIG_PATH;
    out->override_port = 0;
    out->verbose = 0;

    int opt;
    while ((opt = getopt(argc, argv, "c:p:vh")) != -1) {
        switch (opt) {
            case 'c': out->config_path = optarg; break;
            case 'p': out->override_port = parse_int(optarg, 0); break;
            case 'v': out->verbose = 1; break;
            case 'h': usage(argv[0]); return 1;
            default:  usage(argv[0]); return -1;
        }
    }
    return 0;
}

// tine o pereche cheie + valoare implicita pt cautarile in config
// (evit parametri multipli cu tipuri usor de incurcat)
typedef struct str_field {
    const char *key; // cheia din libconfig (ex: "storage.uploads_dir")
    const char *fallback; // valoarea implicita daca cheia lipseste
    char *dst; // bufferul destinatie unde copii valoarea
    size_t dst_len; // dimensiunea bufferului destinatie
} str_field_t;

// cauta o cheie string in config; daca nu exista, pune valoarea implicita
static void lookup_string_field(const config_t *cfg, const str_field_t *fld)
{
    const char *value = NULL;
    if (config_lookup_string(cfg, fld->key, &value) == CONFIG_TRUE && value != NULL) {
        (void)snprintf(fld->dst, fld->dst_len, "%s", value);
    } else {
        (void)snprintf(fld->dst, fld->dst_len, "%s", fld->fallback);
    }
}

// incarca configuratia din fisierul libconfig si populeaza structura server_cfg_t
static int load_config(const cli_opts_t *cli, server_cfg_t *scfg)
{
    config_t cfg;
    config_init(&cfg);

    if (config_read_file(&cfg, cli->config_path) != CONFIG_TRUE) {
        (void)fprintf(stderr, "config: %s:%d - %s\n", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return -1;
    }

    int port = DEFAULT_PORT;
    int max_clients = DEFAULT_MAX_CLIENTS;
    int merge_par = DEFAULT_MERGE_PAR;
    int max_workers = DEFAULT_MAX_WORKERS;

    (void)config_lookup_int(&cfg, "server.listen_port", &port);
    (void)config_lookup_int(&cfg, "server.max_clients", &max_clients);
    (void)config_lookup_int(&cfg, "server.max_workers", &max_workers);
    (void)config_lookup_int(&cfg, "server.merge_parallelism", &merge_par);

    (void)max_workers; // citit din config dar inca nefolosit; va fi pt limitarea fork()-urilor

    scfg->listen_port = (cli->override_port > 0) ? cli->override_port : port;
    scfg->max_clients = max_clients;
    scfg->video.merge_parallelism = merge_par;

    str_field_t fields[] = {
        {
            "storage.uploads_dir",
            "./data/uploads",
            scfg->video.uploads_dir,
            sizeof(scfg->video.uploads_dir),
        },
        {
            "storage.outputs_dir",
            "./data/outputs",
            scfg->video.outputs_dir,
            sizeof(scfg->video.outputs_dir),
        },
        {
            "storage.ffmpeg_binary",
            "/usr/bin/ffmpeg",
            scfg->video.ffmpeg_binary,
            sizeof(scfg->video.ffmpeg_binary),
        },
    };
    for (size_t ix = 0; ix < sizeof(fields) / sizeof(fields[0]); ix++) {
        lookup_string_field(&cfg, &fields[ix]);
    }

    config_destroy(&cfg);
    return 0;
}

int main(int argc, char **argv)
{
    cli_opts_t cli;
    int rcode = parse_cli(argc, argv, &cli);
    if (rcode != 0) {
        return (rcode == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    server_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    if (load_config(&cli, &scfg) < 0) {
        return EXIT_FAILURE;
    }

    if (cli.verbose != 0) {
        (void)fprintf(stderr,
            "vps_server: port=%d max_clients=%d merge_par=%d ffmpeg=%s\n",
            scfg.listen_port, scfg.max_clients,
            scfg.video.merge_parallelism, scfg.video.ffmpeg_binary);
        // afisez informatii mediu; getenv("USER") e safe aici pt ca suntem
        // inca in faza single-threaded, inainte de fork()
        // (secure_getenv ar fi preferabil dar nu e portabil)
        const char *user = getenv("USER");           /* NOLINT */
        if (user != NULL) {
            (void)fprintf(stderr, "vps_server: running as user=%s\n", user);
        }
    }

    if (video_init() < 0) {
        (void)fprintf(stderr, "vps_server: libav init failed\n");
        return EXIT_FAILURE;
    }

    return (net_server_run(&scfg) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 *> Compilare si exemple de rulare:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/server.c -o build/server.o
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude build/server.o build/net_server.o build/proto.o build/video_ops.o build/worker.o -o bin/vps_server -lconfig -lavformat -lavcodec -lavutil -lavfilter
 *
 * --- Rulare cu succes ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_server -h
 * T5 Video Processing Server
 * Usage: bin/vps_server [options]
 *   -c <file>  path to libconfig configuration (default: config/server.conf)
 *   -p <port>  override listen port from the config file
 *   -v         verbose logging to stderr
 *   -h         show this help message
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ nohup bin/vps_server -v >/tmp/final_vps_server.log 2>&1 &
 * [1] 19224
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ head -3 /tmp/final_vps_server.log
 * nohup: ignoring input
 * vps_server: port=18081 max_clients=64 merge_par=4 ffmpeg=/usr/bin/ffmpeg
 * vps_server: running as user=vladb
 *
 * --- Rulare cu esec ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_server -c /tmp/inexistent.conf
 * config: (null):0 - file I/O error
 */
