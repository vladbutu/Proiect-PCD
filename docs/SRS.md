# Multi-Video Operations — SRS

**Echipa 11 · IR3 2026 · PCD**

Aplicatie pentru operatii video (trim, filtre, imbinare cu tranzitii, sunet de fundal),
cu distributia task-urilor in procese fiu, integrare biblioteca externa (FFmpeg) si
transfer de fisiere bidirectional intre client si server.

> Versiunea Milestone 3. Diff fata de Milestone 2:
> - listener AF_UNIX paralel pe `/tmp/vps_admin.sock` (cerinta UX socket)
> - INotify watcher pe `outputs_dir` (notificare finalizare joburi)
> - REST extins: `POST /upload` si `GET /download` (transfer fisiere bidirectional)
> - OPR_INFO (handshake admin: version, build, pid, uptime)
> - client REMOTE in alt limbaj (Python, in `clients/python/vps_remote.py`)
> - captura + analiza Wireshark documentata in `docs/WIRESHARK.md`

---

## 1. Despre proiect

Server video in C care ruleaza operatii FFmpeg la cererea clientilor.
Doua modalitati de acces:

1. protocol binar custom peste TCP (`vps_server` + `vps_admin` + `vps_remote`)
2. REST shim HTTP minimal (`vps_rest`)

Operatiile video se executa in procese copil (`fork + execv`). Merge-ul foloseste
workeri paraleli pe segmente. Serverul raspunde imediat cu `task_id` si actualizeaza
state-ul jobului asincron.

## 2. Utilizatori

| Tip | Binar | Responsabilitati |
|---|---|---|
| Administrator (UX) | `vps_admin` | health-check, status job, list, stats, shutdown — fara transfer fisiere |
| Client remote (IN) | `vps_remote` | trim/filter/merge/mixaudio cu upload + download |
| Client HTTP        | `curl` / tooling REST | endpoint-uri HTTP simple |
| Operator server    | shell             | configurare, pornire/oprire |

## 3. Platforma tinta

- Linux (Ubuntu 22.04+)
- C11 + POSIX (`_POSIX_C_SOURCE=200809L`)
- FFmpeg instalat local; libavformat/libavcodec/libavutil/libavfilter
- libconfig pt fisier de configurare
- Valgrind (memcheck, helgrind) si sanitizere GCC pt validare runtime

## 4. Cerinte functionale

### 4.1 Server

| # | Cerinta | Prioritate | Status M2 |
|---|---|---|---|
| 1 | Server TCP asculta pe port configurabil | Must | OK |
| 2 | Multiplexare cu `poll()` pt clienti concurenti | Must | OK |
| 3 | Configuratie din `server.conf` (libconfig) | Must | OK |
| 4 | Fiecare operatie video ruleaza in proces copil (`fork`+`execv`) | Must | OK |
| 5 | Merge cu paralelizare pe segmente | Must | OK |
| 6 | Coada de joburi (FIFO, capacitate fixa) cu state machine QUEUED/RUNNING/DONE/ERR | Must | OK |
| 7 | STATUS / RESULT raporteaza starea unui task | Must | OK |
| 8 | UPLOAD: client trimite fisier streaming pe socket | Must | OK |
| 9 | DOWNLOAD: client primeste fisier streaming de pe server | Must | OK |
| 10 | Admin ops: PING, LIST_JOBS, STATS, SHUTDOWN, INFO | Should | OK |
| 11 | REST shim cu `/health` + trim/filter/merge/mixaudio | Should | OK |
| 12 | Listener AF_UNIX paralel pt admin local | M3 | OK |
| 13 | INotify watcher pe `outputs_dir` (cerinta INotify) | M3 | OK |
| 14 | REST: `POST /upload` + `GET /download` (transfer fisiere) | M3 | OK |
| 15 | Client REMOTE in alt limbaj (Python, paritate cu cel C) | M3 | OK |

### 4.2 Client ADMIN (UX)

| # | Operatie | Tip |
|---|---|---|
| 1 | ping | health-check |
| 2 | status `<task_id>` | starea unui job |
| 3 | list | toata tabela de joburi |
| 4 | stats | contoare server (uptime, done, failed) |
| 5 | shutdown | comanda graceful exit pt server |
| 6 | info | identitate server (version + build + pid + uptime) |

Clientul ADMIN nu face transfer de fisiere (conform cerintei de proiect).

### 4.3 Client REMOTE (IN)

| # | Operatie | Flux |
|---|---|---|
| 1 | trim | upload input → submit OPR_TRIM → poll STATUS → download rezultat |
| 2 | filter | upload input → submit OPR_FILTER → poll STATUS → download rezultat |
| 3 | merge | upload N clipuri → submit OPR_MERGE → poll STATUS → download rezultat |
| 4 | mixaudio | upload video+audio → submit OPR_MIXAUDIO → poll STATUS → download rezultat |

Acopera 4 din 6 operatii proto definite (`>=70%`, prag obligatoriu in M2).
STATUS / RESULT sunt operatii suplimentare folosite implicit prin poll.

Transfer fisiere **bidirectional** (client → server prin UPLOAD, server → client prin DOWNLOAD).

## 5. Cerinte non-functionale

| # | Cerinta | Tip |
|---|---|---|
| 1 | Build cu `-Wall -Wextra -Wpedantic -Werror`, zero warning | Cod |
| 2 | `clang-tidy` fara erori pe codul user | Cod |
| 3 | `valgrind --leak-check=full` fara leaks | Memorie |
| 4 | `valgrind --tool=helgrind` fara data races | Concurency |
| 5 | ASan + LSan + UBSan fara findings | Memorie + UB |
| 6 | ThreadSanitizer fara findings | Concurency |
| 7 | Fara `system()`, doar `execv()` | Securitate |
| 8 | Conversii numerice robuste (`strtol` cu validare) | Securitate |
| 9 | Reap copii (waitpid + WNOHANG), zero zombies | Robustete |
| 10 | Anti path-traversal pe `OPR_DOWNLOAD` | Securitate |
| 11 | Transfer fisiere pana la cateva sute MB | Performanta |

## 6. Interfete

### 6.1 Server TCP

```bash
bin/vps_server [-c config] [-p port] [-v] [-h]
```

### 6.2 Client ADMIN

```bash
bin/vps_admin [-H host] [-P port] <command> [args]
  ping
  status <task_id>
  list
  stats
  shutdown
```

### 6.3 Client REMOTE

```bash
bin/vps_remote [-H host] [-P port] -o <op> [options]
  -i <file>   input video local (uploaded)
  -a <file>   audio local (mixaudio)
  -c <file>   clip local (repeatable, merge)
  -O <file>   destinatie locala pt download
  -f <name>   filter ffmpeg
  -T <name>   tranzitie (merge)
  -s <ms>     start ms (trim)
  -e <ms>     end ms (trim)
  -N          skip download
```

### 6.4 REST endpoints

- `GET  /health`
- `POST /trim?input=...&output=...&start_ms=...&end_ms=...`
- `POST /filter?input=...&output=...&filter=...`
- `POST /merge?output=...&transition=...&clips=a.mp4,b.mp4,c.mp4`
- `POST /mixaudio?input_video=...&input_audio=...&output=...`

## 7. Limitari cunoscute / out-of-scope M2

- Webservices S/R (cerinta Nivel C) — planificat pt Milestone 3
- Skelet client OTH (Python/alt limbaj) — planificat M3
- Autentificare clienti (out-of-scope)
- Quota disk per client (out-of-scope M2)

## 8. Referinte

- `docs/SDD.md` — design tehnic
- `docs/openapi.yaml`, `docs/api.raml` — specificatii REST + binary
- `include/proto.h` — definitii protocol
- `config/server.conf` — runtime config
