/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Worker-i fork()+execv() pentru operatii video
 * Ce face modulul:
 * - spawn-eaza procese copil care executa ffmpeg via fork()+execv() (cerinta Nivel A)
 * - NICIODATA nu folosesc system() 
 * - worker_spawn() face fork, copilul face execv cu argv-ul construit in video_ops.c
 * - worker_wait() asteapta blocant un copil specific cu waitpid (retry pe EINTR)
 * - worker_dispatch_merge() imparte clipurile in N segmente, proceseaza fiecare
 *   segment intr-un copil separat (paralelism), apoi concateneaza rezultatele
 * Erori tratate explicit:
 * - fork() esuat (limita de procese)
 * - execv() esuat (cale gresita, permisiuni) -> _exit() in copil
 * - waitpid() intrerupt de semnal (EINTR -> reincerc)
 * - copil terminat cu exit code != 0
 * - alocare memorie esuata pt vectorul de PID-uri
 */

#include "worker.h" // declaratiile publice (worker_spawn/wait/dispatch_merge)
#include "proto.h" // proto_merge_t, PROTO_MAX_PATH
#include "video_ops.h" // video_ctx_t, video_argv_free, argv builders

#include <errno.h> // errno, EINTR pt retry waitpid
#include <fcntl.h> // open flags pt /dev/null
#include <stdint.h> // uint32_t
#include <stdio.h> // fprintf, snprintf pt output/erori
#include <stdlib.h> // calloc, free, _exit
#include <string.h> // memset, snprintf
#include <sys/types.h> // pid_t
#include <sys/wait.h> // waitpid, WIFEXITED, WEXITSTATUS pt asteptare copii
#include <unistd.h> // fork, execv, getpid, _exit, dup2, close

#define WORKER_CHILD_EXIT_FAIL 127 // cod de exit cand execv esueaza in copil

// ruleaza o comanda ffmpeg intr-un proces copil creat cu fork()
// returneaza PID-ul copilului (>0) la succes sau -1 la eroare
pid_t worker_spawn(const video_ctx_t *ctx, char **argv)
{
    if (ctx == NULL || argv == NULL || argv[0] == NULL) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        video_argv_free(argv); // fork esuat, eliberez argv
        return -1;
    }
    if (pid == 0) {
        // Evit blocarea in background din cauza stdin-ului legat la terminal.
        // ffmpeg poate incerca sa citeasca stdin pentru comenzi interactive.
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            (void)dup2(nullfd, STDIN_FILENO);
            (void)close(nullfd);
        }

        // copil: inlocuiesc imaginea procesului cu ffmpeg
        (void)execv(argv[0], argv);
        // daca am ajuns aici, execv a esuat; ies cu _exit (nu exit,
        // ca sa nu rulez atexit handlers mosteniti de la parinte)
        _exit(WORKER_CHILD_EXIT_FAIL);
    }
    // parinte: argv-ul ramane al caller-ului, nu eliberam aici
    return pid;
}

// asteapta blocant un copil specific; returnez 0 daca a iesit cu succes, -1 altfel
// folosesc waitpid (nu wait simplu) ca sa astept fix copilul pe care l-am creat
int worker_wait(pid_t pid)
{
    int status = 0;
    pid_t ret;
    // bucla pe EINTR: daca waitpid e intrerupt de semnal, reincerc
    do {
        ret = waitpid(pid, &status, 0);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    return -1;
}

// proceseaza un segment de clipuri (de la 'from' la 'to') intr-un copil separat
static int build_segment(const video_ctx_t *ctx, const proto_merge_t *op, uint32_t from, uint32_t to, char *seg_out)
{
    proto_merge_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.count = to - from;
    (void)snprintf(seg.transition, sizeof(seg.transition), "%s", op->transition);
    (void)snprintf(seg_out, PROTO_MAX_PATH, "%.400s/segment_%ld_%u.mp4", ctx->outputs_dir, (long)getpid(), from);
    (void)snprintf(seg.output, sizeof(seg.output), "%s", seg_out);
    for (uint32_t ix = 0; ix < seg.count; ix++) {
        (void)snprintf(
            seg.clips[ix], PROTO_MAX_PATH, "%s", op->clips[from + ix]);
    }

    char list_path[PROTO_MAX_PATH];
    if (video_write_concat_list(&seg, ctx, list_path) < 0) {
        return -1;
    }
    char **argv = video_build_concat_argv(ctx, &seg, list_path);
    if (argv == NULL) {
        return -1;
    }
    pid_t pid = worker_spawn(ctx, argv);
    if (pid < 0) {
        return -1;
    }
    int ret = worker_wait(pid);
    video_argv_free(argv);
    return ret;
}

// dispatch-ul principal pt MERGE: imparte clipurile si proceseaza in paralel
int worker_dispatch_merge(const video_ctx_t *ctx, const proto_merge_t *op)
{
    if (ctx == NULL || op == NULL || op->count == 0) {
        return -1;
    }
    // cate parti (segmente) fac; limitez la nr de clipuri daca e mai mic
    uint32_t parts = (uint32_t)ctx->merge_parallelism;
    if (parts == 0 || parts > op->count) {
        parts = op->count;
    }
    uint32_t per = op->count / parts; // clipuri per segment
    uint32_t rem = op->count % parts; // restul se distribuie primelor segmente

    // structura pt operatia finala de concat a segmentelor
    proto_merge_t final_op;
    memset(&final_op, 0, sizeof(final_op));
    final_op.count = parts;
    (void)snprintf(final_op.transition, sizeof(final_op.transition), "%s", op->transition);
    (void)snprintf(final_op.output, sizeof(final_op.output), "%s", op->output);

    // aloc un vector de PID-uri ca sa tin minte copiii fork-ati
    pid_t *pids = (pid_t *)calloc(parts, sizeof(pid_t));
    if (pids == NULL) {
        return -1;
    }

    uint32_t cursor = 0;
    int overall = 0;
    char final_list_path[PROTO_MAX_PATH];
    int final_list_created = 0;

    for (uint32_t ix = 0; ix < parts; ix++) {
        uint32_t take = per + ((ix < rem) ? 1U : 0U);
        uint32_t from = cursor;
        uint32_t to = cursor + take;
        cursor = to;

        // fiecare segment e procesat intr-un copil separat care la randul lui
        // face fork+execv pe ffmpeg prin build_segment()
        pid_t pid = fork();
        if (pid < 0) {
            overall = -1;
            break;
        }
        if (pid == 0) {
            char out_path[PROTO_MAX_PATH];
            int rcode = build_segment(ctx, op, from, to, out_path);
            _exit(rcode == 0 ? 0 : 1);
        }
        pids[ix] = pid;
        (void)snprintf(final_op.clips[ix], PROTO_MAX_PATH, "%.400s/segment_%ld_%u.mp4", ctx->outputs_dir, (long)pid, from);
    }

    // astept toti copiii sa termine
    for (uint32_t ix = 0; ix < parts; ix++) {
        if (pids[ix] > 0 && worker_wait(pids[ix]) != 0) {
            overall = -1;
        }
    }

    if (overall < 0) {
        goto cleanup_temps;
    }

    // concatul final: daca avem tranzitie folosim xfade, altfel concat simplu
    char **argv = NULL;
    if (op->transition[0] != '\0') {
        argv = video_build_xfade_argv(ctx, &final_op);
    } else {
        if (video_write_concat_list(&final_op, ctx, final_list_path) < 0) {
            overall = -1;
            goto cleanup_temps;
        }
        final_list_created = 1;
        argv = video_build_concat_argv(ctx, &final_op, final_list_path);
    }
    if (argv == NULL) {
        overall = -1;
        goto cleanup_temps;
    }
    pid_t fpid = worker_spawn(ctx, argv);
    if (fpid < 0) {
        overall = -1;
        video_argv_free(argv);
        goto cleanup_temps;
    }
    int ret = worker_wait(fpid);
    video_argv_free(argv);
    if (ret != 0) {
        overall = -1;
    }

cleanup_temps:
    // Curata fisierele temporare rezultate din faza de segmentare.
    // Acestea sunt artefacte intermediare, nu output final pt client.
    for (uint32_t ix = 0; ix < parts; ix++) {
        if (final_op.clips[ix][0] != '\0') {
            (void)remove(final_op.clips[ix]);
        }
        if (pids[ix] > 0) {
            char seg_list_path[PROTO_MAX_PATH];
            (void)snprintf(seg_list_path, PROTO_MAX_PATH, "%.400s/concat_%ld.txt", ctx->outputs_dir, (long)pids[ix]);
            (void)remove(seg_list_path);
        }
    }

    // Daca s-a folosit concat simplu la pasul final, sterg si lista finala.
    if (final_list_created != 0) {
        (void)remove(final_list_path);
    }

    free(pids);
    return (overall == 0) ? 0 : -1;
}

/*
 *> Compilare si exemple de rulare:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * (worker.c este compilat ca parte din vps_server si vps_rest)
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/worker.c -o build/worker.o
 *
 * --- Exercitare cu succes ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o trim -i data/uploads/clip1.mp4 -O data/outputs/final_trimmed.mp4 -s 0 -e 1200
 * reply: task_id=17918 state=2 path=data/outputs/final_trimmed.mp4
 * (worker_spawn() + worker_wait() ruleaza ffmpeg in copil si asteapta finalizarea)
 *
 * --- Exercitare cu succes (merge paralel) ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o merge -c data/uploads/clip1.mp4 -c data/uploads/clip2.mp4 -c data/uploads/clip3.mp4 -O data/outputs/final_merged.mp4 -T fade
 * reply: task_id=20644 state=2 path=data/outputs/final_merged.mp4
 * (worker_dispatch_merge() imparte clipurile in segmente si face concatenarea finala)
 */
