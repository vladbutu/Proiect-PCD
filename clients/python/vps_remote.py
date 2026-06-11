#!/usr/bin/env python3
# Echipa 11 · IR3 2026 · PCD · client remote (OTH) in Python
# Vorbeste protocolul binar al vps_server (vezi include/proto.h).
# Endianness (confirmat citind net_server.c si remote.c):
#   - header: big-endian (htonl)
#   - STATUS/LIST/STATS/upload+download replies pe state/task_id: big-endian
#   - TRIM/FILTER/MERGE/MIXAUDIO uint32 payload (start_ms etc.): host order (LE pe x86)
#   - proto_upload_t.size_bytes si bytes_received in reply: host order (LE pe x86)

import argparse
import os
import socket
import struct
import sys

OPR_CONNECT = 0
OPR_BYE = 1
OPR_TRIM = 2
OPR_FILTER = 3
OPR_MERGE = 4
OPR_MIXAUDIO = 5
OPR_STATUS = 6
OPR_RESULT = 7
OPR_PING = 8
OPR_LIST_JOBS = 9
OPR_SHUTDOWN = 10
OPR_STATS = 11
OPR_UPLOAD = 12
OPR_DOWNLOAD = 13
OPR_INFO = 14

TASK_STATE = {0: "queued", 1: "running", 2: "done", 0xFFFFFFFF: "error", -1: "error"}
OPR_NAME = {2: "trim", 3: "filter", 4: "merge", 5: "mixaudio", 12: "upload", 13: "download"}

PROTO_MAX_PATH = 512
PROTO_MAX_FILTER = 64
PROTO_MAX_CLIPS = 16
PROTO_UPLOAD_CHUNK = 64 * 1024
HEADER_SIZE = 16


def recv_full(sock, n):
    # Citeste exact n bytes (loop pe EINTR/partial)
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed early")
        buf += chunk
    return bytes(buf)


def send_header(sock, op_id, payload_len, task_id=0, client_id=0):
    # Header: big-endian, identic cu htonl-urile din proto.c
    msg_size = HEADER_SIZE + payload_len
    sock.sendall(struct.pack(">IIII", msg_size, client_id, op_id, task_id))


def recv_header(sock):
    raw = recv_full(sock, HEADER_SIZE)
    return struct.unpack(">IIII", raw)


def send_simple(sock, op_id, payload=b"", task_id=0):
    send_header(sock, op_id, len(payload), task_id=task_id)
    if payload:
        sock.sendall(payload)


def dial(host, port, unix_path=None):
    # daca s-a dat unix_path, conecteaza AF_UNIX in loc de TCP
    if unix_path:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(unix_path)
        return s
    return socket.create_connection((host, port))


def pad_str(s, length):
    # Stringuri C: NUL-terminated, padded la length cu zero
    raw = s.encode("utf-8")[: length - 1]
    return raw + b"\x00" * (length - len(raw))


def state_str(s):
    if s == 2:
        return "done"
    if s == 1:
        return "running"
    if s == 0:
        return "queued"
    return "error"


def cmd_ping(sock, _args):
    send_simple(sock, OPR_PING)
    _msg, _cid, op, _tid = recv_header(sock)
    print(f"PING ok (op_id={op})")
    return 0


def cmd_status(sock, args):
    # proto_query_t.task_id: server citeste cu ntohl -> trimit big-endian
    payload = struct.pack(">I", args.task_id)
    send_simple(sock, OPR_STATUS, payload)
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # proto_status_t: server scrie task_id/state cu htonl -> citesc big-endian
    task_id, state = struct.unpack_from(">Ii", body, 0)
    path = body[8 : 8 + PROTO_MAX_PATH].split(b"\x00", 1)[0].decode("utf-8", "replace")
    print(f"STATUS task_id={task_id} state={state_str(state)} path={path}")
    return 0


def cmd_list(sock, _args):
    send_simple(sock, OPR_LIST_JOBS)
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # proto_list_jobs_reply_t: count si toate campurile job-urilor cu htonl
    (count,) = struct.unpack_from(">I", body, 0)
    print(f"LIST: {count} job(s)")
    entry_size = 4 + 4 + 4 + PROTO_MAX_PATH
    off = 4
    for i in range(count):
        tid, opid, st = struct.unpack_from(">IIi", body, off)
        out = body[off + 12 : off + 12 + PROTO_MAX_PATH].split(b"\x00", 1)[0].decode("utf-8", "replace")
        print(f"  [{i}] task_id={tid} op={OPR_NAME.get(opid, opid)} state={state_str(st)} out={out}")
        off += entry_size
    return 0


def cmd_stats(sock, _args):
    send_simple(sock, OPR_STATS)
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # proto_stats_reply_t: toate cele 5 uint32 cu htonl
    q, r, d, f, up = struct.unpack_from(">IIIII", body, 0)
    print(f"STATS queued={q} running={r} done={d} failed={f} uptime={up}s")
    return 0


def cmd_info(sock, _args):
    send_simple(sock, OPR_INFO)
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # proto_info_reply_t: char[32] version, char[32] build_date, uint32 pid, uint32 uptime (BE)
    version = body[0:32].split(b"\x00", 1)[0].decode("utf-8", "replace")
    build = body[32:64].split(b"\x00", 1)[0].decode("utf-8", "replace")
    pid, uptime = struct.unpack_from(">II", body, 64)
    print(f"INFO version={version} build={build} pid={pid} uptime={uptime}s")
    return 0


def cmd_shutdown(sock, _args):
    send_simple(sock, OPR_SHUTDOWN)
    try:
        recv_header(sock)
    except ConnectionError:
        pass
    print("SHUTDOWN trimis")
    return 0


def cmd_upload(sock, args):
    # OPR_UPLOAD: uint64 size + char[512] filename, apoi stream raw
    size = os.path.getsize(args.path)
    payload = struct.pack("<Q", size) + pad_str(os.path.basename(args.path), PROTO_MAX_PATH)
    send_simple(sock, OPR_UPLOAD, payload)
    with open(args.path, "rb") as f:
        while True:
            chunk = f.read(PROTO_UPLOAD_CHUNK)
            if not chunk:
                break
            sock.sendall(chunk)
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # state e htonl-uit pe server; bytes_received ramane host order (LE pe x86)
    (state,) = struct.unpack_from(">i", body, 0)
    (recvd,) = struct.unpack_from("<Q", body, 8)
    stored = body[16 : 16 + PROTO_MAX_PATH].split(b"\x00", 1)[0].decode("utf-8", "replace")
    print(f"UPLOAD state={state_str(state)} bytes={recvd} stored={stored}")
    return 0


def cmd_download(sock, args):
    # OPR_DOWNLOAD cere char[512] path. Raspuns: int32+pad+uint64, apoi raw bytes
    payload = pad_str(args.path, PROTO_MAX_PATH)
    send_simple(sock, OPR_DOWNLOAD, payload)
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # state big-endian, size_bytes host order (LE pe x86)
    (state,) = struct.unpack_from(">i", body, 0)
    (size,) = struct.unpack_from("<Q", body, 8)
    if state != 2 or size == 0:
        print(f"DOWNLOAD esuat state={state_str(state)}")
        return 1
    out_path = args.out or os.path.basename(args.path)
    with open(out_path, "wb") as f:
        left = size
        while left > 0:
            chunk = sock.recv(min(left, PROTO_UPLOAD_CHUNK))
            if not chunk:
                raise ConnectionError("download intrerupt")
            f.write(chunk)
            left -= len(chunk)
    print(f"DOWNLOAD ok size={size} -> {out_path}")
    return 0


def cmd_trim(sock, args):
    payload = struct.pack("<II", args.start_ms, args.end_ms) + pad_str(args.input, PROTO_MAX_PATH) + pad_str(args.output, PROTO_MAX_PATH)
    send_simple(sock, OPR_TRIM, payload)
    return _print_status_reply(sock, "TRIM")


def cmd_filter(sock, args):
    payload = pad_str(args.filter, PROTO_MAX_FILTER) + pad_str(args.input, PROTO_MAX_PATH) + pad_str(args.output, PROTO_MAX_PATH)
    send_simple(sock, OPR_FILTER, payload)
    return _print_status_reply(sock, "FILTER")


def cmd_mixaudio(sock, args):
    payload = pad_str(args.video, PROTO_MAX_PATH) + pad_str(args.audio, PROTO_MAX_PATH) + pad_str(args.output, PROTO_MAX_PATH)
    send_simple(sock, OPR_MIXAUDIO, payload)
    return _print_status_reply(sock, "MIXAUDIO")


def cmd_merge(sock, args):
    clips = args.clips.split(",")
    if not clips or len(clips) > PROTO_MAX_CLIPS:
        print(f"merge: nr clipuri invalid ({len(clips)})", file=sys.stderr)
        return 1
    payload = struct.pack("<I", len(clips))
    payload += pad_str(args.transition, PROTO_MAX_FILTER)
    payload += pad_str(args.output, PROTO_MAX_PATH)
    for i in range(PROTO_MAX_CLIPS):
        payload += pad_str(clips[i] if i < len(clips) else "", PROTO_MAX_PATH)
    send_simple(sock, OPR_MERGE, payload)
    return _print_status_reply(sock, "MERGE")


def _print_status_reply(sock, label):
    msg_size, _cid, _op, _tid = recv_header(sock)
    body = recv_full(sock, msg_size - HEADER_SIZE)
    # proto_status_t reply: big-endian (htonl pe server)
    task_id, state = struct.unpack_from(">Ii", body, 0)
    path = body[8 : 8 + PROTO_MAX_PATH].split(b"\x00", 1)[0].decode("utf-8", "replace")
    print(f"{label} task_id={task_id} state={state_str(state)} path={path}")
    return 0 if state in (0, 1, 2) else 1


COMMANDS = {
    "ping": cmd_ping,
    "status": cmd_status,
    "list": cmd_list,
    "stats": cmd_stats,
    "info": cmd_info,
    "shutdown": cmd_shutdown,
    "upload": cmd_upload,
    "download": cmd_download,
    "trim": cmd_trim,
    "filter": cmd_filter,
    "mixaudio": cmd_mixaudio,
    "merge": cmd_merge,
}


def build_parser():
    p = argparse.ArgumentParser(description="vps_remote client Python (alt limbaj)")
    p.add_argument("-H", "--host", default="127.0.0.1")
    p.add_argument("-P", "--port", type=int, default=18081)
    p.add_argument("-U", "--unix", help="conecteaza pe AF_UNIX (ex: /tmp/vps_admin.sock)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    sub.add_parser("list")
    sub.add_parser("stats")
    sub.add_parser("info")
    sub.add_parser("shutdown")
    s = sub.add_parser("status"); s.add_argument("task_id", type=int)
    s = sub.add_parser("upload"); s.add_argument("path")
    s = sub.add_parser("download"); s.add_argument("path"); s.add_argument("--out")
    s = sub.add_parser("trim"); s.add_argument("--input", required=True); s.add_argument("--output", required=True); s.add_argument("--start-ms", dest="start_ms", type=int, default=0); s.add_argument("--end-ms", dest="end_ms", type=int, required=True)
    s = sub.add_parser("filter"); s.add_argument("--input", required=True); s.add_argument("--output", required=True); s.add_argument("--filter", required=True)
    s = sub.add_parser("mixaudio"); s.add_argument("--video", required=True); s.add_argument("--audio", required=True); s.add_argument("--output", required=True)
    s = sub.add_parser("merge"); s.add_argument("--clips", required=True, help="lista CSV cu max 16 cai"); s.add_argument("--output", required=True); s.add_argument("--transition", default="none")
    return p


def main():
    args = build_parser().parse_args()
    handler = COMMANDS[args.cmd]
    with dial(args.host, args.port, unix_path=args.unix) as sock:
        return handler(sock, args)


if __name__ == "__main__":
    sys.exit(main())
