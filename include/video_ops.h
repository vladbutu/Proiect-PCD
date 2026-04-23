/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Interfata FFmpeg/libav* si constructori argv pt ffmpeg CLI
 * Doua stiluri de integrare (cerinta: cel putin o biblioteca externa):
 *   1. libavformat este linkat si folosit la runtime prin video_probe()
 *      ca sa inspectez metadatele clipurilor (durata, codec-uri, rezolutie)
 *   2. operatiile grele (trim/filter/concat/mixaudio) sunt delegate
 *      binarului ffmpeg prin execv(), fork-at dintr-un worker
 *      (izoleaza esecurile per-fork, cerinta Nivel A: procese)
 */

#ifndef VIDEO_OPS_H
#define VIDEO_OPS_H

#include "proto.h"  // PROTO_MAX_PATH pt dimensiunile bufferelor

#include <stdint.h> // int64_t pt durata in milisecunde

// structura populata de video_probe() dupa parsarea headerului clipului
typedef struct video_info {
    int64_t duration_ms;       // durata totala in milisecunde
    int     width;             // latime video (pixeli)
    int     height;            // inaltime video (pixeli)
    char    video_codec[32];   // numele codec-ului video (ex: "h264")
    char    audio_codec[32];   // numele codec-ului audio (ex: "aac")
} video_info_t;

// tine calea ffmpeg + directoarele de upload/output din libconfig
typedef struct video_ctx {
    char ffmpeg_binary[PROTO_MAX_PATH]; // calea absoluta la binarul ffmpeg
    char uploads_dir[PROTO_MAX_PATH];   // directorul de uploaduri
    char outputs_dir[PROTO_MAX_PATH];   // directorul de outputuri
    int  merge_parallelism;             // cate segmente in paralel la merge
} video_ctx_t;

// initializare unica: porneste subsistemul de retea libav
// returneaza 0 la succes, -1 la eroare
int video_init(void);

// inspectez un clip cu libavformat; populez 'out' la succes
int video_probe(const char *path, video_info_t *out);

// aceste functii NU fac fork; construiesc argv-ul pt ffmpeg
// returneaza un vector NULL-terminated de stringuri pe heap
// caller-ul trebuie sa elibereze vectorul SI fiecare string (video_argv_free)
char **video_build_trim_argv(const video_ctx_t *ctx,
                             const proto_trim_t *op);
char **video_build_filter_argv(const video_ctx_t *ctx,
                               const proto_filter_t *op);
char **video_build_concat_argv(const video_ctx_t *ctx,
                               const proto_merge_t *op,
                               const char *concat_list_path);
char **video_build_mixaudio_argv(const video_ctx_t *ctx,
                                 const proto_mixaudio_t *op);

// elibereaza un vector argv produs de helperii de mai sus
void video_argv_free(char **argv);

// construieste argv[] pt merge cu tranzitii xfade (probeaza duratele, re-encodeaza)
char **video_build_xfade_argv(const video_ctx_t *ctx,
                               const proto_merge_t *op);

// scrie un fisier lista concat temporar si pune calea in path_out (PROTO_MAX_PATH)
int video_write_concat_list(const proto_merge_t *op,
                            const video_ctx_t *ctx,
                            char *path_out);

#endif /* VIDEO_OPS_H */
