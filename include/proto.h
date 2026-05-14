/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Protocolul binar intre vps_client si vps_server
 * Fiecare mesaj incepe cu un header de dimensiune fixa (campuri in network byte order)
 * urmat de un payload specific operatiei. Protocolul suporta 4 operatii video
 * (trim, filter, merge, mixaudio) plus management de sesiune (connect, bye, status, result).
 * Codurile de operatie si structurile payload sunt documentate mai jos.
 */

#ifndef PROTO_H
#define PROTO_H

#include <stdint.h> // uint32_t, int32_t pt campurile protocolului
#include <stddef.h> // size_t pt parametri functii

#define OPR_CONNECT 0 // handshake: clientul cere un ID de la server
#define OPR_BYE 1 // inchide sesiunea
#define OPR_TRIM 2 // taie un clip [start,end]
#define OPR_FILTER 3 // aplica un filtru video
#define OPR_MERGE 4 // concateneaza N clipuri cu tranzitii
#define OPR_MIXAUDIO 5 // suprapune audio pe video
#define OPR_STATUS 6 // interogheaza starea unui task
#define OPR_RESULT 7 // obtine calea/URL-ul rezultatului
// ---- Admin (UX) ops, no file transfer ----
#define OPR_PING 8 // health-check rapid
#define OPR_LIST_JOBS 9 // dump al cozii curente de joburi
#define OPR_SHUTDOWN 10 // server shutdown gracios (admin)
#define OPR_STATS 11 // statistici agregate server
// ---- File transfer ops (REMOTE, IN) ----
#define OPR_UPLOAD 12 // client trimite un fisier catre server
#define OPR_DOWNLOAD 13 // client cere un fisier de pe server

// Stari task in proto_status_t.state
#define TASK_STATE_QUEUED  0 // in coada, neinceput
#define TASK_STATE_RUNNING 1 // worker fork-at, ffmpeg ruleaza
#define TASK_STATE_DONE    2 // succes
#define TASK_STATE_ERR    (-1) // esuat

#define PROTO_MAX_PATH 512 // lungime maxima cale fisier
#define PROTO_MAX_FILTER 64 // lungime maxima nume filtru
#define PROTO_MAX_CLIPS 16 // nr maxim clipuri pt merge
#define PROTO_MAX_MSG 4096 // dimensiune maxima mesaj
#define PROTO_LIST_MAX_JOBS 32 // max joburi in raspunsul LIST_JOBS

// Chunk size pentru transferul streaming de fisiere (cateva sute MB realiste)
#define PROTO_UPLOAD_CHUNK (64U * 1024U)


typedef struct proto_header {
    uint32_t msg_size;   // dimensiune totala mesaj (inclusiv header)
    uint32_t client_id;  // atribuit de server la OPR_CONNECT
    uint32_t op_id;      // unul din OPR_* de mai sus
    uint32_t task_id;    // identificator task (0 pt operatii noi)
} proto_header_t;

// OPR_TRIM: taie fisierul de la start_ms la end_ms
typedef struct proto_trim {
    uint32_t start_ms;  // timpul de start in milisecunde
    uint32_t end_ms;    // timpul de sfarsit in milisecunde
    char     input[PROTO_MAX_PATH];  // calea fisierului de intrare
    char     output[PROTO_MAX_PATH]; // calea fisierului de iesire
} proto_trim_t;

// OPR_FILTER: aplica un filtru FFmpeg (ex: "grayscale")
typedef struct proto_filter {
    char filter[PROTO_MAX_FILTER];  // numele filtrului
    char input[PROTO_MAX_PATH];     // fisier intrare
    char output[PROTO_MAX_PATH];    // fisier iesire
} proto_filter_t;

// OPR_MERGE: concateneaza 'count' clipuri cu tranzitia data
// segmentele sunt procesate in paralel in procese copil (vezi worker.c)
typedef struct proto_merge {
    uint32_t count;                                // nr de clipuri valide
    char     transition[PROTO_MAX_FILTER];         // ex: "fade", "none"
    char     output[PROTO_MAX_PATH];               // fisier iesire final
    char     clips[PROTO_MAX_CLIPS][PROTO_MAX_PATH]; // lista de clipuri
} proto_merge_t;

// OPR_MIXAUDIO: suprapune o pista audio externa pe video
typedef struct proto_mixaudio {
    char input_video[PROTO_MAX_PATH]; // fisier video intrare
    char input_audio[PROTO_MAX_PATH]; // fisier audio intrare
    char output[PROTO_MAX_PATH];      // fisier iesire
} proto_mixaudio_t;

// OPR_STATUS / OPR_RESULT: payload raspuns cu starea taskului
typedef struct proto_status {
    uint32_t task_id;    // ID-ul taskului
    int32_t  state;      // TASK_STATE_*
    char     result_path[PROTO_MAX_PATH]; // calea rezultatului
} proto_status_t;

// OPR_STATUS / OPR_RESULT: cerere catre server (clientul intreaba pt task_id)
typedef struct proto_query {
    uint32_t task_id; // ID-ul taskului interogat
} proto_query_t;

// OPR_UPLOAD: header pt fisier ce va veni in streaming dupa
// dupa acest payload, urmeaza exact size_bytes de date raw
typedef struct proto_upload {
    uint64_t size_bytes;          // dimensiunea fisierului
    char     filename[PROTO_MAX_PATH]; // numele dorit pe server (basename)
} proto_upload_t;

// OPR_UPLOAD reply: serverul confirma + calea finala
typedef struct proto_upload_reply {
    int32_t  state;                       // TASK_STATE_DONE / TASK_STATE_ERR
    uint64_t bytes_received;              // bytes confirmati primitii
    char     stored_path[PROTO_MAX_PATH]; // calea pe disc-ul serverului
} proto_upload_reply_t;

// OPR_DOWNLOAD: cerere fisier de pe server
typedef struct proto_download {
    char path[PROTO_MAX_PATH]; // calea fisierului dorit (relativa la outputs_dir)
} proto_download_t;

// OPR_DOWNLOAD reply: header cu size, urmat de size bytes raw
typedef struct proto_download_reply {
    int32_t  state;       // TASK_STATE_DONE / TASK_STATE_ERR
    uint64_t size_bytes;  // 0 daca eroare
} proto_download_reply_t;

// OPR_LIST_JOBS reply: dump compact al joburilor active/finalizate recent
typedef struct proto_job_entry {
    uint32_t task_id;
    uint32_t op_id;
    int32_t  state;
    char     output[PROTO_MAX_PATH];
} proto_job_entry_t;

typedef struct proto_list_jobs_reply {
    uint32_t count;
    proto_job_entry_t jobs[PROTO_LIST_MAX_JOBS];
} proto_list_jobs_reply_t;

// OPR_STATS reply: contoare server
typedef struct proto_stats_reply {
    uint32_t jobs_queued;
    uint32_t jobs_running;
    uint32_t jobs_done;
    uint32_t jobs_failed;
    uint32_t uptime_sec;
} proto_stats_reply_t;

// I/O streaming pe fd (folosit la upload/download fisiere mari)
// nu folosesc buffere mari in user-space, transfer in chunks de PROTO_UPLOAD_CHUNK
int proto_send_file(int sock, int fd_in, uint64_t size);
int proto_recv_file(int sock, int fd_out, uint64_t size);

// citeste/scrie un header complet cu conversie endianness
// returneaza 0 la succes, -1 la eroare I/O sau citire/scriere partiala
int proto_read_header(int sock, proto_header_t *hdr);
int proto_write_header(int sock, const proto_header_t *hdr);

// citeste/scrie exact len bytes pe socket
int proto_read_full(int sock, void *buf, size_t len);
int proto_write_full(int sock, const void *buf, size_t len);

#endif /* PROTO_H */
