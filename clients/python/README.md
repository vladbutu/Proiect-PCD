# Clienți Python — Echipa 11 / PCD / Milestone 3

Componente livrate pentru cerința **„client remote în alt limbaj"** (#4 din
checklist) și pentru testarea componentei REST (#5/#6).

## `vps_remote.py` — client remote OTH (Python)

Vorbește direct protocolul binar al `vps_server` (port 18081). Acoperă toate
operațiile definite în `include/proto.h`:

- admin/simple: `ping`, `list`, `stats`, `status`, `shutdown`
- transfer fișiere (unidirecțional + bidirecțional): `upload`, `download`
- video: `trim`, `filter`, `mixaudio`, `merge`

Endianness-ul payload-urilor variază (vezi comentariile la început de fișier):
header + replies pe `state`/`task_id` în big-endian (`htonl` pe server),
restul în host order pentru paritate cu `proto_upload_t` / `proto_trim_t`.

### Exemple

```bash
python3 vps_remote.py ping
python3 vps_remote.py stats
python3 vps_remote.py list

python3 vps_remote.py upload ../../data/uploads/clip1.mp4

python3 vps_remote.py trim --input data/uploads/clip1.mp4 \
    --output data/outputs/py_trim.mp4 --start-ms 0 --end-ms 1500

python3 vps_remote.py merge \
    --clips data/uploads/clip1.mp4,data/uploads/clip2.mp4 \
    --output data/outputs/py_merge.mp4 --transition fade

python3 vps_remote.py -H 127.0.0.1 -P 18081 shutdown
```

## `vps_rest_test.py` — tester HTTP pentru `vps_rest`

Lovește endpoint-urile expuse de `src/rest_server.c` (port 18082 default) și
afișează pass/fail pentru fiecare scenariu. Acoperă atât happy path
(`GET /health`, `POST /trim`, `POST /filter`) cât și ramurile de eroare
(metoda greșită, endpoint inexistent, parametri lipsă).

```bash
python3 vps_rest_test.py --base http://127.0.0.1:18082
```

## Dependențe

Doar stdlib (`socket`, `struct`, `urllib`, `argparse`, `json`). Python ≥ 3.8.
