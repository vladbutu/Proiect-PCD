/**
 * Echipa 11 · IR3 2026 · PCD
 * Coada de joburi async pentru serverul video.
 * Tinem un tabel fix de joburi (capacitate JOBS_MAX). Cand un client cere
 * o operatie video, serverul aloca un slot, face fork+execv pt ffmpeg si
 * raspunde imediat cu task_id (state=RUNNING). Bucla principala face
 * waitpid(WNOHANG) periodic si actualizeaza state-ul jobului la DONE/ERR.
 *
 * Singur thread (proces parinte) atinge tabela -> nu e nevoie de mutex.
 */
#ifndef JOBS_H
#define JOBS_H

#include "proto.h" // TASK_STATE_*, PROTO_MAX_PATH

#include <stdint.h> // uint32_t pt task_id si counters
#include <sys/types.h> // pid_t pt worker PID
#include <time.h> // time_t pt timestamps job

#define JOBS_MAX 64
#define JOBS_RETENTION_SEC_DEFAULT 300
#define CLEANUP_INTERVAL_SEC       10

typedef struct job_entry {
    int      used;
    uint32_t task_id;
    uint32_t op_id;
    int32_t  state;
    pid_t    pid;
    char     output[PROTO_MAX_PATH];
    char   **argv;
    time_t   created_at;
    time_t   finished_at;
} job_entry_t;

typedef struct jobs_table {
    job_entry_t slots[JOBS_MAX];
    uint32_t    next_task_id;
    uint32_t    counter_done;
    uint32_t    counter_failed;
    time_t      started_at;
} jobs_table_t;

void jobs_init(jobs_table_t *tab);

// aloca slot, setez state=RUNNING; returneaza task_id sau 0 daca tabela e full
uint32_t jobs_submit(jobs_table_t *tab, uint32_t op_id, const char *output,
                     char **argv, pid_t pid);

job_entry_t *jobs_find(jobs_table_t *tab, uint32_t task_id);

// waitpid(WNOHANG) pe toti copiii RUNNING; marcheaza DONE/ERR si elibereaza argv
void jobs_reap_running(jobs_table_t *tab);

void jobs_destroy(jobs_table_t *tab);

// elibereaza sloturile DONE/ERR mai vechi de retention_sec
unsigned jobs_evict_old(jobs_table_t *tab, unsigned retention_sec);

// sterge fisierele din incoming/ care sunt duplicate exacte (basename+size) ale unui fixture din uploads/
unsigned cleanup_incoming_duplicates(const char *incoming_dir,
                                     const char *uploads_dir);

#endif
