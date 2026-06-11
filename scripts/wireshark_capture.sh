#!/usr/bin/env bash
# Echipa 11 · IR3 2026 · PCD · script captura Wireshark/tshark pentru demo M3
# Captureaza traficul de pe loopback pe porturile vps_server (18081) si vps_rest (18082).
# Output: capture/pcd_<timestamp>.pcapng -> deschis ulterior in Wireshark.

set -euo pipefail

PORT_BIN=${PORT_BIN:-18081}
PORT_REST=${PORT_REST:-18082}
IFACE=${IFACE:-lo}
OUT_DIR=${OUT_DIR:-capture}
DURATION=${DURATION:-30}

mkdir -p "$OUT_DIR"
STAMP=$(date +%Y%m%d_%H%M%S)
OUT="$OUT_DIR/pcd_${STAMP}.pcapng"

if ! command -v tshark >/dev/null 2>&1; then
    echo "tshark lipseste. Instaleaza: sudo apt install tshark" >&2
    exit 1
fi

FILTER="tcp port ${PORT_BIN} or tcp port ${PORT_REST}"
echo "[+] interfata=${IFACE} filter='${FILTER}' durata=${DURATION}s -> ${OUT}"
echo "[i] ruleaza in alt terminal: vps_server, vps_rest, si trafic (vps_remote / vps_rest_test.py)"

sudo tshark -i "${IFACE}" -f "${FILTER}" -a duration:"${DURATION}" -w "${OUT}"
echo "[+] gata: deschide cu 'wireshark ${OUT}' sau 'tshark -r ${OUT} -V | less'"
