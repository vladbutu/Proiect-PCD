# Multi-Video Operations
## SRS + SDD

**Echipa 11 · IR3 2026 · PCD**

Cerinta: aplicatie pentru operatii video (trim, filtre, imbinare cu tranzitii, sunet de fundal), cu distributia task-urilor in procese fiu si integrare biblioteca externa.

---

## Partea 1 - SRS

### 1.1 Despre ce e proiectul

Proiectul implementeaza un server video in C care ruleaza operatii FFmpeg la cererea clientilor.
Exista doua modalitati de acces:

1. protocol binar custom peste TCP (`vps_server` + `vps_client`)
2. REST shim HTTP minimal (`vps_rest`) pentru demo WS/REST

Operatiile video sunt executate in procese copil (`fork + execv`). La merge, procesarea este impartita pe workeri paraleli.

### 1.2 Functionalitati

1. Trim: taie un segment dintr-un clip
2. Filter: aplica filtru video FFmpeg
3. Merge: concateneaza mai multe clipuri cu tranzitii
4. MixAudio: suprapune audio pe video
5. Status/Result: raportare stare task
6. REST shim: endpoint-uri HTTP pentru health si operatii video de baza

### 1.3 Utilizatori

- operator CLI binar (`vps_client`)
- client HTTP (`curl`/tooling REST)
- administrator server (configurare, pornire/oprire)

### 1.4 Platforma tinta

- Linux (Ubuntu 22.04+)
- C11 + POSIX (`_POSIX_C_SOURCE=200809L`)
- FFmpeg instalat local

### 1.5 Cerinte functionale

| # | Cerinta | Prioritate |
|---|---|---|
| 1 | Serverul TCP asculta pe port configurabil | Must |
| 2 | Clientul trimite cereri trim/filter/merge/mixaudio | Must |
| 3 | Protocolul binar foloseste header fix + payload specific | Must |
| 4 | Fiecare operatie video ruleaza in proces copil (fork/execv) | Must |
| 5 | Merge foloseste paralelizare pe segmente | Must |
| 6 | Serverul suporta clienti multipli prin poll() | Must |
| 7 | Configuratia se incarca din `server.conf` (libconfig) | Must |
| 8 | Exista endpoint REST `/health` | Should |
| 9 | Exista endpoint-uri REST pentru trim/filter/merge/mixaudio | Should |

### 1.6 Cerinte non-functionale

| # | Cerinta | Tip |
|---|---|---|
| 1 | Build cu `-Wall -Wextra -Wpedantic -Werror` fara warnings | Cod |
| 2 | `clang-tidy` fara erori pe cod user | Cod |
| 3 | Fara `system()`, doar `execv()` | Securitate |
| 4 | Conversii numerice robuste (`strtol`) | Securitate |
| 5 | Handling semnale + reap copii (fara zombie) | Robustete |
| 6 | Portabilitate prin API POSIX si `getaddrinfo` | Portabilitate |

### 1.7 Interfete

#### Server TCP CLI
```bash
bin/vps_server [options]
  -c <file>   config file (default: config/server.conf)
  -p <port>   override port
  -v          verbose
  -h          help
```

#### Client TCP CLI
```bash
bin/vps_client -o <trim|filter|merge|mixaudio> [options]
  -H <host>   server host
  -P <port>   server port
  -i <file>   input video
  -O <file>   output video
  -a <file>   audio file (mixaudio)
  -f <name>   filter name (filter)
  -T <name>   transition name (merge)
  -c <file>   clip (repeatable, merge)
  -s <ms>     start ms (trim)
  -e <ms>     end ms (trim)
```

#### REST Shim CLI
```bash
bin/vps_rest [options]
  -c <file>   config file (default: config/server.conf)
  -p <port>   REST port (default: 18082)
  -h          help
```

#### REST Endpoints

- `GET /health`
- `POST /trim?input=...&output=...&start_ms=...&end_ms=...`
- `POST /filter?input=...&output=...&filter=...`
- `POST /merge?output=...&transition=...&clips=a.mp4,b.mp4,c.mp4`
- `POST /mixaudio?input_video=...&input_audio=...&output=...`

---

## Partea 2 - SDD

### 2.1 Arhitectura

Arhitectura are un motor comun de procesare (`worker + video_ops`) si doua intrari:

1. intrare TCP binara (`vps_server`)
2. intrare HTTP simpla (`vps_rest`)

Ambele intrari trimit in final la aceleasi executii FFmpeg.

```text
vps_client --TCP--> vps_server --dispatch--> worker/video_ops --> ffmpeg
curl/http --------> vps_rest  --map query--> worker/video_ops --> ffmpeg
```

### 2.2 Module

| Modul | Fisier(e) | Rol |
|---|---|---|
| Entry TCP | `server.c` | parse CLI + config + init + start net loop |
| Network | `net_server.c` | poll(), accept, dispatch protocol binar |
| Protocol | `proto.c`, `proto.h` | framing binar, read/write full, endian |
| Video Ops | `video_ops.c`, `video_ops.h` | construire argv FFmpeg, probe metadata |
| Workers | `worker.c`, `worker.h` | fork/execv/wait, merge paralel |
| Client TCP | `client.c` | cereri protocol binar |
| REST Shim | `rest_server.c` | endpoint-uri HTTP, mapare query params |
| Config | `config/server.conf` | porturi, directoare, ffmpeg path |

### 2.3 Protocolul binar (S/R/proto)

Header fix 16 bytes (network byte order):

- `msg_size` (4B)
- `client_id` (4B)
- `op_id` (4B)
- `task_id` (4B)

Coduri `op_id`:

- 0 CONNECT
- 1 BYE
- 2 TRIM
- 3 FILTER
- 4 MERGE
- 5 MIXAUDIO
- 6 STATUS
- 7 RESULT

Dimensiuni payload (fara header):

| Operatie | Dimensiune |
|---|---|
| TRIM | 1032 B |
| FILTER | 1088 B |
| MERGE | 8772 B |
| MIXAUDIO | 1536 B |
| STATUS/RESULT raspuns | 520 B |

Constante relevante:

- `PROTO_MAX_PATH = 512`
- `PROTO_MAX_FILTER = 64`
- `PROTO_MAX_CLIPS = 16`
- `PROTO_MAX_MSG = 4096` (MERGE depaseste aceasta limita)

### 2.4 Flux request (exemplu trim, TCP)

1. client trimite `ProtoHeader(op=TRIM)` + `proto_trim_t`
2. server valideaza si construieste argv FFmpeg
3. server face `fork()` + `execv(ffmpeg, argv)`
4. server asteapta `waitpid()`
5. server raspunde cu `ProtoStatus`

### 2.5 Flux request (exemplu trim, REST)

1. client HTTP trimite `POST /trim?...`
2. `vps_rest` parseaza query params
3. construieste `proto_trim_t` intern
4. reuse `video_build_trim_argv` + `worker_spawn/worker_wait`
5. raspuns JSON: `{"status":"ok","output":"..."}`

### 2.6 Gestionarea proceselor si erorilor

- fiecare operatie video ruleaza in copil separat
- `waitpid` trateaza terminarea copiilor
- erorile FFmpeg => status eroare
- erori socket/parsing => inchidere conexiune/raspuns 400
- semnale: serverul evita crash la deconectari brutale (SIGPIPE)

### 2.7 Structura proiect

```text
skeleton/
  bin/                  (vps_server, vps_client, vps_rest)
  build/
  config/server.conf
  data/uploads/
  data/outputs/
  docs/                 (SRS_SDD.md, openapi.yaml, api.raml)
  include/              (proto.h, net_server.h, video_ops.h, worker.h)
  src/                  (server.c, net_server.c, proto.c, video_ops.c, worker.c, client.c, rest_server.c)
  makefile
```

### 2.8 Build si rulare

Dependinte:
```bash
sudo apt install ffmpeg libconfig-dev libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev
```

Build:
```bash
make clean && make all
make tidy
```

Rulare TCP:
```bash
bin/vps_server -v
bin/vps_client -o trim -i data/uploads/clip1.mp4 -O data/outputs/trimmed.mp4 -s 0 -e 3000
```

Rulare REST:
```bash
bin/vps_rest -p 18082
curl -sS http://127.0.0.1:18082/health
curl -sS -X POST "http://127.0.0.1:18082/trim?input=data/uploads/clip1.mp4&output=data/outputs/rest_trimmed.mp4&start_ms=0&end_ms=1000"
```

### 2.9 Biblioteci externe

| Biblioteca | Utilizare |
|---|---|
| libconfig | parsare configuratie |
| libavformat/libavcodec/libavutil/libavfilter | metadate si integrare ecosistem FFmpeg |
| ffmpeg (binar) | executia operatiilor video prin `execv` |

---

## Referinte

- `include/proto.h` - structuri protocol si constante
- `docs/openapi.yaml` - specificatie OpenAPI (binary + REST)
- `docs/api.raml` - specificatie RAML (binary + REST)
- `config/server.conf` - configuratie runtime
