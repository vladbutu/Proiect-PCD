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

#define PROTO_MAX_PATH 512 // lungime maxima cale fisier
#define PROTO_MAX_FILTER 64 // lungime maxima nume filtru
#define PROTO_MAX_CLIPS 16 // nr maxim clipuri pt merge
#define PROTO_MAX_MSG 4096 // dimensiune maxima mesaj


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
    int32_t  state;      // 0=in coada, 1=in executie, 2=gata, -1=eroare
    char     result_path[PROTO_MAX_PATH]; // calea rezultatului
} proto_status_t;

// citeste/scrie un header complet cu conversie endianness
// returneaza 0 la succes, -1 la eroare I/O sau citire/scriere partiala
int proto_read_header(int sock, proto_header_t *hdr);
int proto_write_header(int sock, const proto_header_t *hdr);

// citeste/scrie exact len bytes pe socket
int proto_read_full(int sock, void *buf, size_t len);
int proto_write_full(int sock, const void *buf, size_t len);

#endif /* PROTO_H */
