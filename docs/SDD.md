# Multi-Video Operations — SDD (Software Design Document)

**Echipa 11 · IR3 2026 · PCD · Milestone 2 (preliminar)**

Acest document descrie design-ul tehnic care implementeaza cerintele din `SRS.md`.

---

## 1. Arhitectura

```text
                +-----------+        binar TCP            +------------+
   vps_admin -->|           |<--- maintenance ops --------|            |
                |  vps_     |                              |  worker.c |
   vps_remote ->|  server   |--- fork()+execv() ---------->|  + ffmpeg  |
                |  (poll)   |                              +------------+
                |  + jobs[] |
   curl/REST -->|  vps_rest |--- (sincron) --------------> worker/ffmpeg
                +-----------+
```

Un singur motor de procesare (`worker` + `video_ops`) este folosit atat de serverul
binar TCP cat si de REST shim. Diferenta principala fata de M1: serverul binar este
**asincron** — accepta cererea, fork-eaza workerul, returneaza imediat `task_id`,
si raporteaza state-ul prin STATUS/RESULT.

## 2. Module

| Modul | Fisier(e) | Rol |
|---|---|---|
| Entry server | `src/server.c` | parse CLI + libconfig + init + start net loop |
| Network    | `src/net_server.c` | `poll()`, accept, dispatch protocol binar |
| Job queue  | `src/jobs.c`, `include/jobs.h` | tabela fixa de joburi, state machine, reap async |
| Protocol   | `src/proto.c`, `include/proto.h` | framing binar, conversie endianness, streaming fisiere |
| Video Ops  | `src/video_ops.c`, `include/video_ops.h` | construire argv FFmpeg, probe metadata |
| Workers    | `src/worker.c`, `include/worker.h` | `fork`+`execv` pt ffmpeg, dispatch merge paralel |
| Client ADMIN | `src/admin.c` | UX maintenance (ping/status/list/stats/shutdown) |
| Client REMOTE | `src/remote.c` | upload + submit + poll + download |
| REST shim  | `src/rest_server.c` | endpoint HTTP, reuses worker/video_ops |
| Config     | `config/server.conf` | porturi, directoare, ffmpeg path |

## 3. Protocol binar

### 3.1 Header

Header fix 16 bytes, network byte order:

| Offset | Camp | Tip |
|---|---|---|
| 0 | msg_size | uint32 |
| 4 | client_id | uint32 |
| 8 | op_id | uint32 |
| 12 | task_id | uint32 |

### 3.2 Coduri operatii (`OPR_*`)

| Cod | Nume | Categorie | Direct |
|---|---|---|---|
| 0 | CONNECT | session | client→server |
| 1 | BYE | session | client→server |
| 2 | TRIM | video | client→server (REMOTE) |
| 3 | FILTER | video | client→server (REMOTE) |
| 4 | MERGE | video | client→server (REMOTE) |
| 5 | MIXAUDIO | video | client→server (REMOTE) |
| 6 | STATUS | query | client→server |
| 7 | RESULT | query | client→server |
| 8 | PING | admin | ADMIN |
| 9 | LIST_JOBS | admin | ADMIN |
| 10 | SHUTDOWN | admin | ADMIN |
| 11 | STATS | admin | ADMIN |
| 12 | UPLOAD | transfer | client→server (REMOTE) |
| 13 | DOWNLOAD | transfer | server→client (REMOTE) |

### 3.3 Payload-uri principale

| Operatie | Cerere | Raspuns |
|---|---|---|
| TRIM/FILTER/MERGE/MIXAUDIO | structura specifica | `proto_status_t` (task_id, state=RUNNING) |
| STATUS / RESULT | `proto_query_t {task_id}` | `proto_status_t` |
| LIST_JOBS | (vid) | `proto_list_jobs_reply_t` (max 32 entries) |
| STATS | (vid) | `proto_stats_reply_t` (uptime, done, failed) |
| UPLOAD | `proto_upload_t {size, filename}` + `size` bytes raw | `proto_upload_reply_t {state, bytes, stored_path}` |
| DOWNLOAD | `proto_download_t {path}` | `proto_download_reply_t {state, size}` + `size` bytes raw |

State machine pt joburi:

```
   submit
QUEUED ──> RUNNING ──> DONE
                  └──> ERR
```

In M2, joburile intra direct in `RUNNING` la submit (fork imediat); `QUEUED` e rezervat
pentru extinderi viitoare (e.g. M3, daca depasim `max_workers`).

## 4. Fluxuri principale

### 4.1 Operatie REMOTE (trim, cu transfer bidirectional)

```
Client                                Server
  |  OPR_UPLOAD + proto_upload_t         |
  |--------------------------------------->|
  |  file bytes (streaming)              |
  |--------------------------------------->|
  |       proto_upload_reply_t            |
  |<---------------------------------------|
  |  OPR_TRIM + proto_trim_t (paths server)
  |--------------------------------------->|
  |       proto_status_t (task_id, RUNNING)
  |<---------------------------------------|
  | (loop)                                |
  |  OPR_STATUS + proto_query_t           |
  |--------------------------------------->|
  |       proto_status_t (state=DONE)     |
  |<---------------------------------------|
  |  OPR_DOWNLOAD + proto_download_t      |
  |--------------------------------------->|
  |       proto_download_reply_t + bytes  |
  |<---------------------------------------|
```

### 4.2 Operatie ADMIN (status)

```
ADMIN                                  Server
  |  OPR_STATUS + proto_query_t         |
  |------------------------------------->|
  |       proto_status_t                |
  |<-------------------------------------|
```

## 5. Coada de joburi

`jobs_table_t` este o tabela fixa cu `JOBS_MAX=64` sloturi. Fiecare slot tine:
`task_id`, `op_id`, `state`, `pid` worker, `output` path, `argv` ffmpeg (pt
eliberare la reap).

Operatii:

- `jobs_submit()` — gaseste slot liber, atribuie `task_id` incremental, marcheaza `RUNNING`.
- `jobs_find()` — lookup dupa `task_id` (folosit de STATUS/RESULT).
- `jobs_reap_running()` — apelat la fiecare iteratie poll(); `waitpid(WNOHANG)`
  pe fiecare pid running, actualizeaza `state` la DONE/ERR si elibereaza `argv`.

**Concurency:** un singur thread (procesul parinte) acceseaza tabela. Workerii sunt
procese separate, comunica doar prin exit code → fara mutex.

### 5.1 Submit MERGE async

MERGE-ul foloseste `worker_dispatch_merge()` care e sincron (orchestreaza multiple
fork-uri pe segmente). Ca sa-l facem async din punctul de vedere al serverului:
parintele fork-eaza **un orchestrator**, copilul ruleaza `worker_dispatch_merge`
si `_exit(rc)`. Parintele stocheaza doar pid-ul orchestratorului in tabela.

## 6. Streaming fisiere (UPLOAD / DOWNLOAD)

`proto_send_file` / `proto_recv_file` lucreaza in chunk-uri de `PROTO_UPLOAD_CHUNK = 64 KiB`,
fara buffer-e in user-space mari. Suporta fisiere de ordinul sutelor de MB fara probleme.

Anti path-traversal pe DOWNLOAD: serverul accepta numai cai care incep cu `outputs_dir`
si nu contin `..`. UPLOAD foloseste `basename()` plus prefix `<pid>_<counter>_` pt
unicitate si izolare intre clienti.

## 7. Gestionarea proceselor si erorilor

- fiecare operatie video ruleaza in proces copil
- reap periodic via `jobs_reap_running` (la fiecare iteratie poll())
- `SIGPIPE` ignorat (clientul poate inchide brut)
- `SIGINT` / `SIGTERM` → flag `g_stop` → exit gracios; tabela e drenata via `jobs_destroy`
- erori ffmpeg → exit code != 0 → state = ERR
- protocol errors pe socket → close conn (fara crash)
- shutdown via OPR_SHUTDOWN → server raspunde ACK apoi seteaza `g_stop`

## 8. Validare runtime

Build-uri suplimentare pt sanitizers (target-uri makefile):

- `make asan` — `vps_*_asan` cu `-fsanitize=address,leak,undefined`
- `make tsan` — `vps_*_tsan` cu `-fsanitize=thread -pthread`

Valgrind (host machine, dupa instalarea pachetului):

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./bin/vps_server
valgrind --tool=helgrind ./bin/vps_server
```

Pana acum (M2): ASan + UBSan + LSan + TSan zero findings dupa rularea pipeline-ului
complet (upload, trim, filter, merge, mixaudio, admin ops, shutdown).

## 9. Structura proiect

```text
.
├── bin/                # vps_server, vps_admin, vps_remote, vps_rest, (+_asan, +_tsan)
├── build/              # objects + intermediates
├── config/server.conf
├── data/uploads/       # fixture inputs
├── data/uploads/incoming/  # uploads primite (creat la runtime)
├── data/outputs/       # rezultate ffmpeg
├── docs/
│   ├── SRS.md
│   ├── SDD.md
│   ├── openapi.yaml
│   └── api.raml
├── include/
│   ├── proto.h, net_server.h, video_ops.h, worker.h, jobs.h
├── src/
│   ├── server.c net_server.c proto.c video_ops.c worker.c jobs.c
│   ├── admin.c remote.c rest_server.c
└── makefile
```

## 10. Build si rulare

```bash
sudo apt install ffmpeg libconfig-dev libavformat-dev libavcodec-dev \
                 libavutil-dev libavfilter-dev valgrind
make clean && make all

# Rulare server
bin/vps_server -v

# ADMIN
bin/vps_admin ping
bin/vps_admin list
bin/vps_admin stats
bin/vps_admin shutdown

# REMOTE (transfer bidirectional)
bin/vps_remote -o trim   -i data/uploads/clip1.mp4 -O /tmp/out_trim.mp4   -s 0 -e 1500
bin/vps_remote -o filter -i data/uploads/clip1.mp4 -O /tmp/out_filter.mp4 -f hflip
bin/vps_remote -o merge  -c data/uploads/clip1.mp4 -c data/uploads/clip2.mp4 -O /tmp/out_merge.mp4
bin/vps_remote -o mixaudio -i data/uploads/clip1.mp4 -a data/uploads/background.aac -O /tmp/out_mix.mp4
```

## 11. Biblioteci externe

| Biblioteca | Utilizare |
|---|---|
| libconfig | parsare `server.conf` |
| libavformat / libavcodec / libavutil / libavfilter | probe metadata video |
| ffmpeg (binar) | executia operatiilor video prin `execv` |

## 12. Roadmap M3 (orientativ)

- finalizare server + admin + remote (REMOTE/IN deja in M2)
- schelet client REMOTE in alt limbaj (Python/Java/Go)
- WebServices REST extinse + testare cu tool / client HTTP
- fisiere mari (1+ GB) + benchmarks
