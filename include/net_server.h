/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Bucla poll() TCP accept pt serverul video
 */

#ifndef NET_SERVER_H
#define NET_SERVER_H

#include "video_ops.h" // video_ctx_t cu caile ffmpeg si directoarele

// configuratia serverului (populata din libconfig + CLI)
typedef struct server_cfg {
    int listen_port;   // portul TCP pe care asculta
    int max_clients;   // nr maxim de clienti concurenti in poll()
    video_ctx_t video; // contextul video (ffmpeg binary, dirs, paralelism)
} server_cfg_t;

// intra in bucla poll(); blocheaza pana la SIGTERM sau eroare fatala
// returneaza 0 la shutdown curat, -1 la eroare fatala
int net_server_run(const server_cfg_t *cfg);

#endif /* NET_SERVER_H */
