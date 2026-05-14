/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Dispatch fork()+execv() pt operatii video
 * Cerinta Nivel A: fiecare operatie video e delegata unui proces copil.
 * Operatia MERGE imparte munca in mai multi copii (worker_dispatch_merge).
 * system() este interzis de cert-env33-c; folosim exclusiv familia exec().
 */

#ifndef WORKER_H
#define WORKER_H

#include "proto.h" // PROTO_MAX_PATH, proto_merge_t
#include "video_ops.h" // video_ctx_t

#include <sys/types.h> // pid_t

// ruleaza ffmpeg intr-un copil fork-at; argv ramane in proprietatea caller-ului (PID >0 succes, -1 eroare)
pid_t worker_spawn(const video_ctx_t *ctx, char **argv);

// asteapta blocant PID-ul (0 daca copilul a iesit cu 0, -1 altfel)
int worker_wait(pid_t pid);

// dispatch merge: imparte in ctx->merge_parallelism segmente paralele, apoi concateneaza
int worker_dispatch_merge(const video_ctx_t *ctx,
                          const proto_merge_t *op);

#endif /* WORKER_H */
