#!/usr/bin/env bash
# Full cross-level benchmark comparison: runs every level's server/client
# pair under consistent conditions (same payload sizes, concurrency,
# duration) in one pass, so the numbers are directly comparable rather than
# copy-pasted from different sessions with different parameters.
set -uo pipefail
cd "$(dirname "$0")/.."

PAYLOAD=64
BIG_PAYLOAD=10000
IN_FLIGHT=1024
SECONDS_RUN=5
CONNECTIONS=10

SERVER_PID=

run_server() {
    "$@" > /tmp/bench_server.log 2>&1 &
    SERVER_PID=$!
    sleep 0.4
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
}

# Averages a "field=value" metric out of /tmp/bench_server.log, skipping the
# first reporting line (partial/warmup interval) and requiring the field
# name be preceded by line-start or whitespace so e.g. "pps" doesn't also
# match inside "fragment_pps" or "message_pps".
avg_field() {
    local field=$1
    grep -oE "(^| )${field}=[0-9.]+" /tmp/bench_server.log | tail -n +2 | sed -E "s/^[^=]*=//" |
        awk '{s+=$1;n++} END {if(n>0) printf "%.0f", s/n; else print "N/A"}'
}

pubkey_from_server_log() {
    grep -A1 "static pubkey" /tmp/bench_server.log | tail -1
}

echo "=== Level 1: bare io_uring UDP (noecho) ==="
run_server ./build/level1_echo_server --port 19001 --slots "$IN_FLIGHT"
./build/level1_client_bench --host 127.0.0.1 --port 19001 --slots "$IN_FLIGHT" --payload "$PAYLOAD" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level1_baseline_pps=$(avg_field pps)"
stop_server

echo "=== Level 1: echo mode ==="
run_server ./build/level1_echo_server --port 19002 --slots "$IN_FLIGHT" --echo
./build/level1_client_bench --host 127.0.0.1 --port 19002 --slots "$IN_FLIGHT" --payload "$PAYLOAD" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level1_echo_pps=$(avg_field pps)"
stop_server

echo "=== Level 2: handshake, pure CPU (no network) ==="
./build/level2_handshake_bench 3 >/tmp/bench_l2cpu.log 2>&1
echo "RESULT level2_handshake_cpu_per_sec=$(grep -oE 'full_handshakes_per_sec=[0-9.]+' /tmp/bench_l2cpu.log | sed -E 's/.*=//')"

echo "=== Level 2: handshake over UDP ==="
run_server ./build/level2_udp_bench_server 19003 512
PUBKEY=$(pubkey_from_server_log)
timeout 8 ./build/level2_udp_bench_client --host 127.0.0.1 --port 19003 --server-pubkey "$PUBKEY" --concurrency 512 --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level2_handshake_udp_per_sec=$(avg_field 'handshakes/s')"
stop_server

echo "=== Level 2: post-handshake data plane, ad-hoc (10 conns) ==="
run_server ./build/level2_post_handshake_pps_server 19004 1024
PUBKEY=$(pubkey_from_server_log)
timeout 8 ./build/level2_post_handshake_pps_client --host 127.0.0.1 --port 19004 --server-pubkey "$PUBKEY" --connections "$CONNECTIONS" --payload "$PAYLOAD" --in-flight "$IN_FLIGHT" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level2_post_handshake_data_pps=$(avg_field data_pps)"
stop_server

echo "=== Level 3: reassembly only, single fragment (${PAYLOAD}B) ==="
run_server ./build/level3_fragment_pps_server 19005 4096 4096
./build/level3_fragment_pps_client --host 127.0.0.1 --port 19005 --message-size "$PAYLOAD" --in-flight "$IN_FLIGHT" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level3_single_fragment_pps=$(avg_field fragment_pps)"
stop_server

echo "=== Level 3: reassembly only, ${BIG_PAYLOAD}B message ==="
run_server ./build/level3_fragment_pps_server 19006 4096 4096
./build/level3_fragment_pps_client --host 127.0.0.1 --port 19006 --message-size "$BIG_PAYLOAD" --in-flight "$IN_FLIGHT" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level3_10kb_fragment_pps=$(avg_field fragment_pps)"
echo "RESULT level3_10kb_message_pps=$(avg_field message_pps)"
stop_server

echo "=== Level 4/5: full transport (handshake+AD-encryption+reassembly+replay-window), single fragment ==="
run_server ./build/level4_transport_server 19007 1024 4096
PUBKEY=$(pubkey_from_server_log)
timeout 8 ./build/level4_transport_client --host 127.0.0.1 --port 19007 --server-pubkey "$PUBKEY" --connections "$CONNECTIONS" --message-size "$PAYLOAD" --in-flight "$IN_FLIGHT" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level4_single_fragment_pps=$(avg_field fragment_pps)"
stop_server

echo "=== Level 4/5: full transport, ${BIG_PAYLOAD}B message ==="
run_server ./build/level4_transport_server 19008 1024 4096
PUBKEY=$(pubkey_from_server_log)
timeout 8 ./build/level4_transport_client --host 127.0.0.1 --port 19008 --server-pubkey "$PUBKEY" --connections "$CONNECTIONS" --message-size "$BIG_PAYLOAD" --in-flight "$IN_FLIGHT" --seconds "$SECONDS_RUN" >/tmp/bench_client.log 2>&1
echo "RESULT level4_10kb_fragment_pps=$(avg_field fragment_pps)"
echo "RESULT level4_10kb_message_pps=$(avg_field message_pps)"
stop_server

echo "=== Level 5: anti-DoS demo (handshakes, no attack) ==="
run_server ./build/level5_anti_dos_server 19009 1024 50
PUBKEY=$(pubkey_from_server_log)
./build/level5_legit_client --host 127.0.0.1 --port 19009 --server-pubkey "$PUBKEY" --count 20 >/tmp/bench_l5_baseline.log 2>&1
echo "RESULT level5_baseline: $(cat /tmp/bench_l5_baseline.log)"
stop_server

echo "=== Level 5: anti-DoS demo (handshakes WHILE under attack) ==="
run_server ./build/level5_anti_dos_server 19010 1024 50
PUBKEY=$(pubkey_from_server_log)
./build/level5_attack_client --host 127.0.0.1 --port 19010 --server-pubkey "$PUBKEY" --in-flight 512 --seconds 6 >/tmp/bench_l5_attack.log 2>&1 &
ATTACK_PID=$!
sleep 1.5
./build/level5_legit_client --host 127.0.0.1 --port 19010 --server-pubkey "$PUBKEY" --count 20 >/tmp/bench_l5_underload.log 2>&1
wait "$ATTACK_PID" 2>/dev/null
echo "RESULT level5_under_attack: $(cat /tmp/bench_l5_underload.log)"
echo "RESULT level5_dh_attempts_per_sec_under_attack=$(avg_field 'dh_attempts/s')"
echo "RESULT level5_cookie_replies_per_sec_under_attack=$(avg_field 'cookie_replies/s')"
echo "RESULT level5_attack_flood_pps=$(grep -oE 'flood_pps=[0-9.]+' /tmp/bench_l5_attack.log | tail -n +2 | sed -E 's/.*=//' | awk '{s+=$1;n++} END{if(n>0) printf "%.0f", s/n; else print "N/A"}')"
stop_server

echo "=== ALL BENCHMARKS DONE ==="
