# Captură & analiză Wireshark - Echipa 11 (Milestone 3, Nivel C)

Acest ghid e folosit pentru a demonstra în fața evaluatorilor că înțelegem trasa
de rețea generată de proiect. Acoperă atât protocolul binar custom
(`vps_server` ↔ `vps_admin`/`vps_remote`) cât și REST shim-ul (`vps_rest`).

## 1. Pregătire

Trei terminale:

```
T1: bin/vps_server -p 18081
T2: bin/vps_rest   -p 18082
T3: ./scripts/wireshark_capture.sh    # captură 30s pe loopback
```

Dependinţă: `sudo apt install tshark` (sau Wireshark cu GUI).

Capturile sunt salvate ca `capture/pcd_<timestamp>.pcapng`.

## 2. Generare trafic în timpul capturii

Într-un al patrulea terminal, rulează scenariile pe care vrem să le arătăm:

```bash
# trafic binar (port 18081)
bin/vps_admin ping
bin/vps_admin stats
bin/vps_admin list
python3 clients/python/vps_remote.py upload data/uploads/clip1.mp4
python3 clients/python/vps_remote.py trim --input data/uploads/clip1.mp4 \
    --output data/outputs/wireshark_demo.mp4 --start-ms 0 --end-ms 800

# trafic REST (port 18082)
python3 clients/python/vps_rest_test.py
```

## 3. Citirea pcap-ului

```bash
wireshark capture/pcd_<stamp>.pcapng
# sau: tshark -r capture/pcd_<stamp>.pcapng -V | less
```

### 3.1 Filtre Wireshark utile

| Filtru                                            | Ce arată                          |
| ------------------------------------------------- | --------------------------------- |
| `tcp.port == 18081`                               | protocol binar                    |
| `tcp.port == 18082`                               | trafic HTTP/REST                  |
| `tcp.flags.syn == 1 && tcp.flags.ack == 0`        | începuturi de conexiune (3-way)   |
| `tcp.flags.fin == 1`                              | închideri grațioase               |
| `http`                                            | doar HTTP-ul                      |
| `tcp.len > 0 and tcp.dstport == 18081`            | mesaje binare către server        |

### 3.2 Decodarea header-ului binar

Fiecare mesaj pe portul 18081 începe cu un header de 16 bytes, în
**network byte order** (big-endian):

```
offset  size  câmp
0       4     msg_size   (total mesaj, header inclus)
4       4     client_id  (atribuit de server la OPR_CONNECT)
8       4     op_id      (vezi tabelul de mai jos)
12      4     task_id
```

Op-uri (pentru explicații live):

| op_id | nume       | direcție            |
| ----- | ---------- | ------------------- |
| 8     | PING       | admin → server      |
| 9     | LIST_JOBS  | admin → server      |
| 11    | STATS      | admin → server      |
| 6     | STATUS     | remote → server     |
| 12    | UPLOAD     | remote → server (+stream raw) |
| 13    | DOWNLOAD   | remote → server     |
| 2-5   | TRIM/FILTER/MERGE/MIXAUDIO | remote → server |

Pe loopback poţi recunoaşte un PING imediat: payload 0 bytes, totalul
pachetului TCP cu `op_id=8` are exact 16 bytes după header-ul TCP.

### 3.3 Ce să arăţi evaluatorilor

1. **Three-way handshake** (SYN, SYN+ACK, ACK) la deschiderea conexiunii
   către 18081.
2. **Decodarea unui header binar**: dă click pe un pachet cu lungime ≥16
   bytes, "Follow TCP Stream", explică câmpurile (msg_size, op_id, task_id).
3. **Streaming UPLOAD**: după headerul `OPR_UPLOAD` și payload-ul de
   `proto_upload_t` (520 bytes), urmează un val de pachete TCP cu chunk-uri
   de 64 KiB până se atinge `size_bytes`.
4. **HTTP request/response** pe 18082: "Follow HTTP Stream" pe un POST
   `/trim`, evaluatorii văd request line + body JSON.
5. **FIN/ACK** la închidere - dovadă de tear-down corect (fără RST).

### 3.4 Mostre de export

Pentru raport, exportă două pachete cheie:

```bash
tshark -r capture/pcd_<stamp>.pcapng -Y "tcp.port == 18081" -V > docs/trace_binary.txt
tshark -r capture/pcd_<stamp>.pcapng -Y "http"             -V > docs/trace_http.txt
```

## 4. Întrebări frecvente la evaluare

- **De ce vezi pachete cu length 0?** ACK-uri pure TCP fără payload.
- **De ce header-ul e big-endian, dar payload-ul nu?** Convenția:
  `htonl`/`ntohl` pe câmpurile fixe ale header-ului; payload-urile sunt
  trimise raw (host order) pentru că ambele capete rulează x86_64.
  Limitare cunoscută - documentată în SDD.
- **De ce loopback?** Demo local. Pentru capturi cross-host, schimbă
  `IFACE=lo` în `IFACE=eth0`/`wlan0` în `wireshark_capture.sh`.
