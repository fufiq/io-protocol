#!/usr/bin/env bash
# Level 1 benchmark: launches the bare io_uring UDP server (no echo, so we
# measure the pure receive-side pps that the 270k baseline refers to),
# blasts it from the client for a fixed duration, and reports the server's
# average pps against the baseline target.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
PORT="${PORT:-9999}"
SLOTS="${SLOTS:-1024}"
PAYLOAD="${PAYLOAD:-64}"
SECONDS_RUN="${SECONDS_RUN:-5}"
BASELINE="${BASELINE:-270000}"

SERVER_LOG="$(mktemp)"
trap 'kill "${SERVER_PID:-0}" 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT

"$BUILD_DIR/level1_echo_server" --port "$PORT" --slots "$SLOTS" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 0.3

"$BUILD_DIR/level1_client_bench" --host 127.0.0.1 --port "$PORT" --slots "$SLOTS" \
    --payload "$PAYLOAD" --seconds "$SECONDS_RUN"

sleep 0.3
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

echo "--- server pps samples ---"
grep '\[server\] pps=' "$SERVER_LOG"

AVG=$(grep '\[server\] pps=' "$SERVER_LOG" | sed -E 's/.*pps=([0-9.]+).*/\1/' | \
    awk '{s+=$1; n++} END {if (n>0) printf "%.0f", s/n; else print 0}')

echo "--- summary ---"
echo "server_avg_pps=$AVG baseline=$BASELINE"
awk -v avg="$AVG" -v base="$BASELINE" 'BEGIN {
    ratio = (base > 0) ? avg/base*100 : 0
    printf "ratio_of_baseline=%.1f%%\n", ratio
}'
