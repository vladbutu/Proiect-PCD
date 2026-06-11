#!/usr/bin/env python3
# Echipa 11 · IR3 2026 · PCD · tester pentru vps_rest (HTTP/REST)
# Loveste endpoint-urile expuse de rest_server.c si valideaza raspunsurile JSON.
# Foloseste doar stdlib (urllib) ca sa nu cerem dependinte externe.

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request


def call(base, method, path, params=None, timeout=10.0, body=None):
    # Construieste URL cu query string si face request HTTP minim
    url = base.rstrip("/") + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method=method, data=body)
    if body is not None:
        req.add_header("Content-Length", str(len(body)))
        req.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
            try:
                return resp.status, data.decode("utf-8")
            except UnicodeDecodeError:
                return resp.status, data
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def parse_json(body):
    try:
        return json.loads(body)
    except json.JSONDecodeError:
        return None


def expect(label, got, want, ok_counter):
    # Comparatie simpla pt assertion + contoare pass/fail in suita de teste
    if got == want:
        print(f"  OK  {label}: {got}")
        ok_counter[0] += 1
    else:
        print(f"  FAIL {label}: got={got} want={want}")
        ok_counter[1] += 1


def run_suite(base):
    ok = [0, 0]
    print(f"[+] suite REST @ {base}")

    print("[*] GET /health")
    code, body = call(base, "GET", "/health")
    js = parse_json(body)
    expect("status code", code, 200, ok)
    expect("json.status", js and js.get("status"), "ok", ok)

    print("[*] POST /trim (cerere valida)")
    code, body = call(base, "POST", "/trim", {"input": "data/uploads/clip1.mp4", "output": "data/outputs/rest_trim.mp4", "start_ms": 0, "end_ms": 1000})
    js = parse_json(body)
    expect("status code", code, 200, ok)
    if js:
        print(f"      reply: {js}")

    print("[*] POST /filter (cerere valida)")
    # serverul paseaza filter direct la 'ffmpeg -vf' (vezi video_ops.c:197).
    # 'hue=s=0' = grayscale via dezsaturare; e o expresie ffmpeg valida nativ.
    code, body = call(base, "POST", "/filter", {"input": "data/uploads/clip1.mp4", "output": "data/outputs/rest_filter.mp4", "filter": "hue=s=0"})
    js = parse_json(body)
    expect("status code", code, 200, ok)
    if js:
        print(f"      reply: {js}")

    print("[*] POST /trim fara parametri (error path)")
    code, body = call(base, "POST", "/trim")
    expect("status code", code, 400, ok)

    print("[*] GET /trim (metoda gresita)")
    code, body = call(base, "GET", "/trim")
    expect("status code", code, 400, ok)

    print("[*] GET /endpoint-nonexistent")
    code, body = call(base, "GET", "/no_such_thing")
    expect("status code", code, 400, ok)

    # /upload + /download adaugate la M3 -- transfer fisiere bidirectional REST
    print("[*] POST /upload (transfer fisiere REST -> server)")
    payload = b"hello-rest-upload-" + os.urandom(64)
    code, body = call(base, "POST", "/upload", {"path": "rest_test_blob.bin"}, body=payload)
    js = parse_json(body) if isinstance(body, str) else None
    expect("status code", code, 200, ok)
    if js and js.get("stored"):
        print(f"      stored: {js['stored']}")

    print("[*] GET /download (transfer fisiere server -> REST)")
    # cerem inapoi un output cunoscut (creat de /trim de mai sus)
    code, body = call(base, "GET", "/download", {"path": "rest_trim.mp4"})
    expect("status code", code, 200, ok)
    if code == 200 and isinstance(body, (bytes, bytearray)):
        print(f"      bytes={len(body)}")

    passed, failed = ok
    print(f"\n[=] rezultat: {passed} ok, {failed} fail")
    return 0 if failed == 0 else 1


def main():
    p = argparse.ArgumentParser(description="vps_rest HTTP tester")
    p.add_argument("--base", default="http://127.0.0.1:18082")
    return run_suite(p.parse_args().base)


if __name__ == "__main__":
    sys.exit(main())
