/**
 * Echipa 11 · IR3 2026 · PCD
 * Tabela de joburi async. Vezi jobs.h pt contract.
 */
#include "jobs.h" // jobs_table_t, job_entry_t, JOBS_MAX
#include "proto.h" // TASK_STATE_*, PROTO_MAX_PATH
#include "video_ops.h" // video_argv_free pt eliberare argv la reap

#include <dirent.h> // opendir/readdir/closedir pt cleanup_incoming_duplicates
#include <errno.h> // EINTR pt waitpid retry
#include <stdint.h> // uint32_t pt task counters
#include <stdio.h> // snprintf pt copierea output path
#include <string.h> // memset pt zero-out slot
#include <sys/stat.h> // stat, S_ISREG pt verificare duplicate
#include <sys/types.h> // pid_t pt worker PID-uri
#include <sys/wait.h> // waitpid, WNOHANG, WIFEXITED, WEXITSTATUS
#include <time.h> // time(NULL) pt timestamps
#include <unistd.h> // unlink pt stergere fisier duplicat

#define JOBS_PATH_BUF_LEN 1024 // buffer pt path-uri pe stack

void jobs_init(jobs_table_t *tab)
{
    memset(tab, 0, sizeof(*tab));
    tab->next_task_id = 1U; // 0 e rezervat pt "invalid"
    tab->started_at = time(NULL);
}

uint32_t jobs_submit(jobs_table_t *tab, uint32_t op_id, const char *output,
                     char **argv, pid_t pid)
{
    for (size_t ix = 0; ix < JOBS_MAX; ix++) {
        if (tab->slots[ix].used == 0) {
            job_entry_t *slot = &tab->slots[ix];
            slot->used = 1;
            slot->task_id = tab->next_task_id++;
            if (tab->next_task_id == 0U) {
                tab->next_task_id = 1U; // skip 0
            }
            slot->op_id = op_id;
            slot->state = TASK_STATE_RUNNING;
            slot->pid = pid;
            slot->argv = argv;
            (void)snprintf(slot->output, sizeof(slot->output), "%s",
                           output != NULL ? output : "");
            slot->created_at = time(NULL);
            return slot->task_id;
        }
    }
    return 0U; // tabela plina
}

job_entry_t *jobs_find(jobs_table_t *tab, uint32_t task_id)
{
    for (size_t ix = 0; ix < JOBS_MAX; ix++) {
        if (tab->slots[ix].used != 0 && tab->slots[ix].task_id == task_id) {
            return &tab->slots[ix];
        }
    }
    return NULL;
}

void jobs_reap_running(jobs_table_t *tab)
{
    for (size_t ix = 0; ix < JOBS_MAX; ix++) {
        job_entry_t *slot = &tab->slots[ix];
        if (slot->used == 0 || slot->state != TASK_STATE_RUNNING || slot->pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t ret = waitpid(slot->pid, &status, WNOHANG);
        if (ret == 0) {
            continue; // inca ruleaza
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            // copilul deja reap-uit altundeva sau dispărut -> marcheaza failed
            slot->state = TASK_STATE_ERR;
            slot->pid = 0;
            slot->finished_at = time(NULL);
            tab->counter_failed++;
            if (slot->argv != NULL) {
                video_argv_free(slot->argv);
                slot->argv = NULL;
            }
            continue;
        }
        // copil terminat
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            slot->state = TASK_STATE_DONE;
            tab->counter_done++;
        } else {
            slot->state = TASK_STATE_ERR;
            tab->counter_failed++;
        }
        slot->pid = 0;
        slot->finished_at = time(NULL);
        if (slot->argv != NULL) {
            video_argv_free(slot->argv);
            slot->argv = NULL;
        }
    }
}

void jobs_destroy(jobs_table_t *tab)
{
    for (size_t ix = 0; ix < JOBS_MAX; ix++) {
        if (tab->slots[ix].argv != NULL) {
            video_argv_free(tab->slots[ix].argv);
            tab->slots[ix].argv = NULL;
        }
    }
}

// elibereaza sloturile DONE/ERR mai vechi de retention_sec
unsigned jobs_evict_old(jobs_table_t *tab, unsigned retention_sec)
{
    time_t now = time(NULL);
    unsigned freed = 0;
    for (size_t ix = 0; ix < JOBS_MAX; ix++) {
        job_entry_t *slot = &tab->slots[ix];
        if (slot->used == 0 || slot->finished_at == 0) {
            continue; // slot liber sau inca activ
        }
        if (slot->state != TASK_STATE_DONE && slot->state != TASK_STATE_ERR) {
            continue;
        }
        if ((unsigned)(now - slot->finished_at) < retention_sec) {
            continue; // inca in retentie
        }
        // elibereaza slotul (argv deja eliberat in reap)
        memset(slot, 0, sizeof(*slot));
        freed++;
    }
    return freed;
}

// extrage basename-ul dintr-un nume "<pid>_<counter>_<rest>"; NULL daca formatul nu se potriveste
static const char *strip_upload_prefix(const char *name)
{
    const char *cur = name;
    while (*cur >= '0' && *cur <= '9') {
        cur++;
    }
    if (*cur != '_' || cur == name) {
        return NULL;
    }
    cur++;
    const char *digits_start = cur;
    while (*cur >= '0' && *cur <= '9') {
        cur++;
    }
    if (*cur != '_' || cur == digits_start) {
        return NULL;
    }
    return cur + 1;
}

// sterge din incoming/ fisierele duplicate (acelasi basename + size) ale unui fixture din uploads/
unsigned cleanup_incoming_duplicates(const char *incoming_dir,
                                     const char *uploads_dir)
{
    DIR *dir = opendir(incoming_dir);
    if (dir == NULL) {
        return 0;
    }
    unsigned removed = 0;
    char in_path[JOBS_PATH_BUF_LEN];
    char up_path[JOBS_PATH_BUF_LEN];
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        const char *orig = strip_upload_prefix(ent->d_name);
        if (orig == NULL || orig[0] == '\0') {
            continue; // nume care nu respecta formatul nostru; nu atingem
        }
        (void)snprintf(in_path, sizeof(in_path), "%s/%s",
                       incoming_dir, ent->d_name);
        (void)snprintf(up_path, sizeof(up_path), "%s/%s",
                       uploads_dir, orig);
        struct stat st_in;
        struct stat st_up;
        if (stat(in_path, &st_in) < 0 || !S_ISREG(st_in.st_mode)) {
            continue;
        }
        if (stat(up_path, &st_up) < 0 || !S_ISREG(st_up.st_mode)) {
            continue; // nu exista fixture cu acelasi nume -> pastram
        }
        if (st_in.st_size != st_up.st_size) {
            continue; // dimensiuni diferite -> nu e duplicat
        }
        if (unlink(in_path) == 0) {
            removed++;
        }
    }
    (void)closedir(dir);
    return removed;
}
