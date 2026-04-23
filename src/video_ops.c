/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Integrare biblioteca externa FFmpeg/libav*
 * Ce face modulul:
 * - demonstreaza integrarea cu biblioteci externe (cerinta Nivel A):
 *   libavformat + libavcodec + libavutil sunt linkate si apelate direct
 * - video_probe() deschide un clip si extrage metadate (durata, codec-uri, rezolutie)
 *   folosind API-ul libavformat (avformat_open_input, avformat_find_stream_info)
 * - construieste vectori argv[] pt operatiile ffmpeg (trim, filter, concat, mixaudio)
 *   care sunt apoi executate prin fork()+execv() din worker.c
 * - genereaza fisiere lista concat (format ffmpeg concat demuxer)
 * Erori tratate explicit:
 * - avformat_open_input / avformat_find_stream_info esuate
 * - alocare memorie esuata (malloc/calloc) la construirea argv
 * - parametri NULL la intrare
 * - fopen/fprintf/fclose esuate la scrierea listei concat
 */

#include "video_ops.h" // declaratiile publice ale modulului
#include "proto.h" // proto_* tipuri si constante folosite explicit

#include <libavcodec/codec_id.h>  // enum AVCodecID, AV_CODEC_ID_NONE
#include <libavcodec/codec_par.h> // AVCodecParameters
#include <libavformat/avformat.h> // avformat_open_input, avformat_find_stream_info etc.
#include <libavutil/avutil.h>     // AV_TIME_BASE, AVMEDIA_TYPE_* constante

#include <stdint.h>  // uint32_t, int64_t
#include <stdio.h>   // fprintf, snprintf, fopen, fclose
#include <stdlib.h>  // malloc, calloc, free pt alocare dinamica
#include <string.h>  // strlen, memcpy, snprintf pt operatii string
#include <unistd.h>  // getpid pt generare nume fisiere temporare unice

#define VIDEO_TIMEBASE_MS 1000 // ffmpeg lucreaza in microsecunde, eu in milisecunde
#define VIDEO_SMALL_BUF   32   // buffer mic pt conversii ms -> "sec.ms"
#define VIDEO_FILTER_BUF  4096 // buffer pentru expresia -filter_complex

// initializeaza subsistemul de retea din libavformat
// (de la FFmpeg 4.x av_register_all() e no-op, dar tin hookul pt compatibilitate)
int video_init(void)
{
    /* Since FFmpeg 4.x av_register_all() is a no-op; we only keep the
     * hook here in case an older libav is installed on the lab machine. */
    avformat_network_init();
    return 0;
}

// copiaza numele codec-ului in bufferul destinatie
static void copy_codec_name(char *dst, size_t dst_len, enum AVCodecID cid)
{
    const char *name = avcodec_get_name(cid);
    if (name == NULL) {
        name = "unknown";
    }
    (void)snprintf(dst, dst_len, "%s", name);
}

// inspectez un clip cu libavformat: deschid containerul, citesc stream-urile,
// extrag durata, rezolutia si codec-urile audio/video
int video_probe(const char *path, video_info_t *out)
{
    if (path == NULL || out == NULL) {
        return -1;
    }

    // deschid fisierul multimedia
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        return -1; // fisier inexistent/corupt/format necunoscut
    }
    // analizez stream-urile pt codec info
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (fmt->duration > 0) {
        out->duration_ms = (int64_t)(fmt->duration / (AV_TIME_BASE / VIDEO_TIMEBASE_MS));
    }
    copy_codec_name(out->video_codec, sizeof(out->video_codec), AV_CODEC_ID_NONE);
    copy_codec_name(out->audio_codec, sizeof(out->audio_codec), AV_CODEC_ID_NONE);

    // parcurg stream-urile si extrag codec-urile video/audio
    for (unsigned int ix = 0; ix < fmt->nb_streams; ix++) {
        AVStream *str = fmt->streams[ix];
        AVCodecParameters *par = str->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            out->width  = par->width;
            out->height = par->height;
            copy_codec_name(out->video_codec, sizeof(out->video_codec), par->codec_id);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            copy_codec_name(out->audio_codec, sizeof(out->audio_codec), par->codec_id);
        }
    }

    avformat_close_input(&fmt);
    return 0;
}

// duplica un string pe heap (helper intern)
static char *dup_str(const char *src)
{
    if (src == NULL) {
        return NULL;
    }
    size_t num = strlen(src) + 1;
    char *ptr = (char *)malloc(num);
    if (ptr != NULL) {
        memcpy(ptr, src, num);
    }
    return ptr;
}

// converteste milisecunde in formatul "sec.ms" acceptat de ffmpeg (ex: "12.345")
static char *format_ms(uint32_t ms)
{    
    char buf[VIDEO_SMALL_BUF];
    (void)snprintf(buf, sizeof(buf), "%u.%03u", ms / VIDEO_TIMEBASE_MS, ms % VIDEO_TIMEBASE_MS);
    return dup_str(buf);
}

// aloca un vector argv cu slots pozitii + NULL terminator
static char **argv_alloc(size_t slots)
{
    return (char **)calloc(slots + 1U, sizeof(char *));
}

// elibereaza un vector argv si toate stringurile din el
void video_argv_free(char **argv)
{
    if (argv == NULL) {
        return;
    }
    for (size_t ix = 0; argv[ix] != NULL; ix++) {
        free(argv[ix]);
    }
    free((void *)argv);
}

// adauga un string in argv[*idx] si avanseaza contorul
// (folosesc un contor in loc de indecsi numerici ca sa evit magic numbers)
static int argv_push(char **argv, size_t *idx, char *value)
{
    if (value == NULL) {
        return -1;
    }
    argv[*idx] = value;
    (*idx)++;
    return 0;
}

// construieste argv[] pt operatia TRIM: ffmpeg -y -ss start -t durata -i input -c copy output
char **video_build_trim_argv(const video_ctx_t *ctx, const proto_trim_t *op)
{
    enum { TRIM_SLOTS = 11 };
    char **argv = argv_alloc(TRIM_SLOTS);
    if (argv == NULL) {
        return NULL;
    }
    uint32_t dur = (op->end_ms > op->start_ms) ? (op->end_ms - op->start_ms) : 0U;

    size_t idx = 0;
    int ok = 0;
    ok |= argv_push(argv, &idx, dup_str(ctx->ffmpeg_binary));
    ok |= argv_push(argv, &idx, dup_str("-y"));
    ok |= argv_push(argv, &idx, dup_str("-ss"));
    ok |= argv_push(argv, &idx, format_ms(op->start_ms));
    ok |= argv_push(argv, &idx, dup_str("-t"));
    ok |= argv_push(argv, &idx, format_ms(dur));
    ok |= argv_push(argv, &idx, dup_str("-i"));
    ok |= argv_push(argv, &idx, dup_str(op->input));
    ok |= argv_push(argv, &idx, dup_str("-c"));
    ok |= argv_push(argv, &idx, dup_str("copy"));
    ok |= argv_push(argv, &idx, dup_str(op->output));
    if (ok != 0) {
        video_argv_free(argv);
        return NULL;
    }
    return argv;
}

// construieste argv[] pt operatia FILTER: ffmpeg -y -i input -vf filtru -c:a copy output
char **video_build_filter_argv(const video_ctx_t *ctx, const proto_filter_t *op)
{
    enum { FILTER_SLOTS = 9 };
    char **argv = argv_alloc(FILTER_SLOTS);
    if (argv == NULL) {
        return NULL;
    }
    size_t idx = 0;
    int ok = 0;
    ok |= argv_push(argv, &idx, dup_str(ctx->ffmpeg_binary));
    ok |= argv_push(argv, &idx, dup_str("-y"));
    ok |= argv_push(argv, &idx, dup_str("-i"));
    ok |= argv_push(argv, &idx, dup_str(op->input));
    ok |= argv_push(argv, &idx, dup_str("-vf"));
    ok |= argv_push(argv, &idx, dup_str(op->filter));
    ok |= argv_push(argv, &idx, dup_str("-c:a"));
    ok |= argv_push(argv, &idx, dup_str("copy"));
    ok |= argv_push(argv, &idx, dup_str(op->output));
    if (ok != 0) {
        video_argv_free(argv);
        return NULL;
    }
    return argv;
}

// construieste argv[] pt concat (merge): ffmpeg -y -f concat -safe 0 -i lista -c copy output
char **video_build_concat_argv(const video_ctx_t *ctx, const proto_merge_t *op, const char *concat_list_path)
{
    enum { CONCAT_SLOTS = 11 };
    char **argv = argv_alloc(CONCAT_SLOTS);
    if (argv == NULL) {
        return NULL;
    }
    size_t idx = 0;
    int ok = 0;
    ok |= argv_push(argv, &idx, dup_str(ctx->ffmpeg_binary));
    ok |= argv_push(argv, &idx, dup_str("-y"));
    ok |= argv_push(argv, &idx, dup_str("-f"));
    ok |= argv_push(argv, &idx, dup_str("concat"));
    ok |= argv_push(argv, &idx, dup_str("-safe"));
    ok |= argv_push(argv, &idx, dup_str("0"));
    ok |= argv_push(argv, &idx, dup_str("-i"));
    ok |= argv_push(argv, &idx, dup_str(concat_list_path));
    ok |= argv_push(argv, &idx, dup_str("-c"));
    ok |= argv_push(argv, &idx, dup_str("copy"));
    ok |= argv_push(argv, &idx, dup_str(op->output));
    if (ok != 0) {
        video_argv_free(argv);
        return NULL;
    }
    return argv;
}

// construieste argv[] pt MIXAUDIO: ffmpeg -y -i video -i audio -c:v copy -map 0:v:0 -shortest output
char **video_build_mixaudio_argv(const video_ctx_t *ctx, const proto_mixaudio_t *op)
{
    enum { MIX_SLOTS = 16 };
    char **argv = argv_alloc(MIX_SLOTS);
    if (argv == NULL) {
        return NULL;
    }
    size_t idx = 0;
    int ok = 0;
    ok |= argv_push(argv, &idx, dup_str(ctx->ffmpeg_binary));
    ok |= argv_push(argv, &idx, dup_str("-y"));
    ok |= argv_push(argv, &idx, dup_str("-i"));
    ok |= argv_push(argv, &idx, dup_str(op->input_video));
    ok |= argv_push(argv, &idx, dup_str("-i"));
    ok |= argv_push(argv, &idx, dup_str(op->input_audio));
    ok |= argv_push(argv, &idx, dup_str("-c:v"));
    ok |= argv_push(argv, &idx, dup_str("copy"));
    ok |= argv_push(argv, &idx, dup_str("-c:a"));
    ok |= argv_push(argv, &idx, dup_str("aac"));
    ok |= argv_push(argv, &idx, dup_str("-map"));
    ok |= argv_push(argv, &idx, dup_str("0:v:0"));
    ok |= argv_push(argv, &idx, dup_str("-map"));
    ok |= argv_push(argv, &idx, dup_str("1:a:0"));
    ok |= argv_push(argv, &idx, dup_str("-shortest"));
    ok |= argv_push(argv, &idx, dup_str(op->output));
    if (ok != 0) {
        video_argv_free(argv);
        return NULL;
    }
    return argv;
}

// scrie un fisier text cu caile clipurilor (format: "file 'cale'" pe fiecare linie)
// necesar pt ffmpeg -f concat -i lista.txt
int video_write_concat_list(const proto_merge_t *op, const video_ctx_t *ctx, char *path_out)
{
    (void)snprintf(path_out, PROTO_MAX_PATH, "%.400s/concat_%ld.txt", ctx->outputs_dir, (long)getpid());

    FILE *fp = fopen(path_out, "we");
    if (fp == NULL) {
        return -1;
    }
    for (uint32_t ix = 0; ix < op->count && ix < PROTO_MAX_CLIPS; ix++) {
        // ffmpeg concat rezolva cai relative fata de fisierul lista, nu CWD
        // construim cale absoluta ca sa mearga corect
        const char *clip = op->clips[ix];
        if (clip[0] == '/') {
            // deja absoluta
            if (fprintf(fp, "file '%s'\n", clip) < 0) {
                (void)fclose(fp);
                return -1;
            }
        } else {
            char cwd[PROTO_MAX_PATH];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                if (fprintf(fp, "file '%s/%s'\n", cwd, clip) < 0) {
                    (void)fclose(fp);
                    return -1;
                }
            } else {
                if (fprintf(fp, "file '%s'\n", clip) < 0) {
                    (void)fclose(fp);
                    return -1;
                }
            }
        }
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

// construieste argv[] pt MERGE cu tranzitii xfade intre clipuri
// probeaza durata fiecarui clip, apoi genereaza -filter_complex cu xfade+acrossfade
// necesita re-encoding (libx264+aac), deci dureaza mai mult decat concat simplu
char **video_build_xfade_argv(const video_ctx_t *ctx, const proto_merge_t *op)
{
    if (ctx == NULL || op == NULL || op->count < 2) {
        return NULL;
    }

    // probam durata fiecarui clip cu libavformat
    double dur[PROTO_MAX_CLIPS] = {0};
    for (uint32_t i = 0; i < op->count && i < PROTO_MAX_CLIPS; i++) {
        video_info_t info;
        if (video_probe(op->clips[i], &info) < 0) {
            return NULL;
        }
        dur[i] = (double)info.duration_ms / VIDEO_TIMEBASE_MS;
    }

    double td = 1.0; // durata tranzitiei in secunde

    // construim -filter_complex: xfade pt video, acrossfade pt audio
    char filter[VIDEO_FILTER_BUF];
    size_t fp = 0;
    double cum = dur[0]; // durata cumulativa pana la tranzitia curenta

    for (uint32_t i = 1; i < op->count; i++) {
        double offset = cum - td;
        if (offset < 0.0) {
            offset = 0.0;
        }

        // video xfade
        if (i == 1) {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp,
                "[0:v][1:v]xfade=transition=%s:duration=%.2f:offset=%.2f",
                op->transition, td, offset);
        } else {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp,
                "[vt%u][%u:v]xfade=transition=%s:duration=%.2f:offset=%.2f",
                i - 1, i, op->transition, td, offset);
        }
        if (i < op->count - 1) {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp, "[vt%u];", i);
        } else {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp, "[vout];");
        }

        // audio acrossfade
        if (i == 1) {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp,
                "[0:a][1:a]acrossfade=d=%.2f", td);
        } else {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp,
                "[at%u][%u:a]acrossfade=d=%.2f", i - 1, i, td);
        }
        if (i < op->count - 1) {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp, "[at%u];", i);
        } else {
            fp += (size_t)snprintf(filter + fp, sizeof(filter) - fp, "[aout]");
        }

        cum += dur[i] - td;
    }

    // argv: ffmpeg -y -i clip0 -i clip1 ... -filter_complex "..." -map [vout] -map [aout]
    //       -c:v libx264 -preset fast -c:a aac output
    enum { EXTRA_SLOTS = 16 };
    size_t total_slots = 2U * (size_t)op->count + EXTRA_SLOTS;
    char **argv = argv_alloc(total_slots);
    if (argv == NULL) {
        return NULL;
    }

    size_t idx = 0;
    int ok = 0;
    ok |= argv_push(argv, &idx, dup_str(ctx->ffmpeg_binary));
    ok |= argv_push(argv, &idx, dup_str("-y"));
    for (uint32_t i = 0; i < op->count; i++) {
        ok |= argv_push(argv, &idx, dup_str("-i"));
        ok |= argv_push(argv, &idx, dup_str(op->clips[i]));
    }
    ok |= argv_push(argv, &idx, dup_str("-filter_complex"));
    ok |= argv_push(argv, &idx, dup_str(filter));
    ok |= argv_push(argv, &idx, dup_str("-map"));
    ok |= argv_push(argv, &idx, dup_str("[vout]"));
    ok |= argv_push(argv, &idx, dup_str("-map"));
    ok |= argv_push(argv, &idx, dup_str("[aout]"));
    ok |= argv_push(argv, &idx, dup_str("-c:v"));
    ok |= argv_push(argv, &idx, dup_str("libx264"));
    ok |= argv_push(argv, &idx, dup_str("-preset"));
    ok |= argv_push(argv, &idx, dup_str("fast"));
    ok |= argv_push(argv, &idx, dup_str("-c:a"));
    ok |= argv_push(argv, &idx, dup_str("aac"));
    ok |= argv_push(argv, &idx, dup_str(op->output));
    if (ok != 0) {
        video_argv_free(argv);
        return NULL;
    }
    return argv;
}

/*
 *> Compilare si exemple de rulare:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * (video_ops.c este compilat ca parte din vps_server si vps_rest)
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/video_ops.c -o build/video_ops.o
 *
 * --- Exercitare cu succes ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o filter -i data/uploads/clip1.mp4 -O data/outputs/final_filtered.mp4 -f hflip
 * reply: task_id=17922 state=2 path=data/outputs/final_filtered.mp4
 * (video_build_filter_argv() construieste comanda ffmpeg, iar video_init() pregateste integrarea libav)
 *
 * --- Exercitare cu esec ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ curl -sS -X POST "http://127.0.0.1:18082/trim"
 * {"status":"error","message":"unsupported endpoint"}
 * (fara query params validi, logica de construire a requestului video nu poate continua)
 */
