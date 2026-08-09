# io_protocol

An ultra-fast, secure UDP transport protocol built on Linux `io_uring`, single-threaded per instance. Every layer is built and benchmarked in isolation first (levels 1-6), then unified into one reusable class (level 7).

Guiding principle throughout: batch everything, avoid kernel/syscall overhead per packet, and never trade away a security property for throughput without saying so explicitly.

## Architecture

The project is organized as seven levels, each adding one concern on top of the last. Every level has its own doctest coverage and its own standalone benchmark binary (`level<N>_*`), so each concern's cost and correctness can be measured independently before it's combined with anything else.

| Level | Concern | Key file |
|---|---|---|
| 1 | Raw `io_uring` UDP baseline: multishot `recvmsg` + provided buffer rings | `multishot_udp.hpp`, `uring_ring.hpp` |
| 2 | Noise_IK_25519_ChaChaPoly_SHA256 handshake (hardcoded state machine, not a generic pattern interpreter) | `noise_ik.hpp` |
| 3 | Message fragmentation/reassembly via a sliding-window bitmask | `packet_assembly.hpp` |
| 4 | Transport encryption tying the handshake and reassembly together (AEAD with the fragment header as associated data) | `transport_connection.hpp` |
| 5 | Anti-DoS: sliding-window replay protection + stateless cookie/load-gate under flood | `replay_window.hpp`, `anti_dos.hpp` |
| 6 | Optional per-message reliability: SACK + batched timer-wheel retransmit + RTT-free AIMD congestion window | `reliability.hpp`, `timer_wheel.hpp` |
| 7 | Unified `protocol::protocol_endpoint` - one class wiring all six together, queue-based (not callback-based) API | `protocol.hpp` |

Every module under `src/` is **header-only** - there are no compiled intermediate libraries. Each `.hpp` is self-contained; `#include` it and it just works, no separate `.cpp` to compile-and-link.

### Wire format (level 7)

One byte of type, then an 8-byte caller-assigned `conn_id`, then a type-specific body:

```
1 = handshake message1: type(1) | conn_id(8) | noise_msg1(with mac1) | mac2(16, zero if none yet)
2 = handshake message2: type(1) | conn_id(8) | noise_msg2
3 = data packet:        type(1) | conn_id(8) | <connection wire packet>
4 = cookie reply:       type(1) | conn_id(8) | cookie(16)
5 = SACK batch:         type(1) | conn_id(8) | count(1) | count * 14-byte entries
6 = keepalive:          type(1) | conn_id(8) | nonce(8) | AEAD tag(16), no payload
```

`conn_id` is caller-assigned and must be unique among an endpoint's live sessions - two peers using the same `conn_id` against the same endpoint will collide (one silently evicts the other's session). `level7_protocol_client` randomizes it by default; pass `--conn-id N` to pin one.

### API shape: two queues, not callbacks

`protocol_endpoint::handle_cqe()` only ever files results away. `poll_incoming()` drains everything that arrived since the last call in one batch - a `std::vector<event>` move, not a callback whose call stack is dictated by the CQE-draining loop. `send_message()` queues data and returns immediately; the actual encrypt+send happens in `tick()`, which is also where a reliable session's congestion window is enforced.

```cpp
protocol::protocol_endpoint<> endpoint(ring, fd, std::move(local_static), config);
endpoint.arm();
endpoint.connect(conn_id, remote_static_pubkey, addr, addr_len);

for (;;) {
    ring.submit_and_wait_timeout(1, 20ms); // not a plain wait - see uring_ring.hpp
    ring.drain_cqes([&](io_uring_cqe* cqe) { endpoint.handle_cqe(cqe); });
    for (auto& event : endpoint.poll_incoming()) {
        // handle handshake_established / handshake_failed / message_received / peer_unreachable however you like
    }
    endpoint.send_message(conn_id, some_payload, /*reliable=*/true);
    endpoint.tick(std::chrono::steady_clock::now());
}
```

Authorization/sessions/JWTs are deliberately **not** this protocol's job - that's application business logic. What the protocol does hand the application is a cryptographically-proven peer identity (`event::peer_static`, only ever populated once Noise_IK has verified the peer holds the matching private key) - what to do with that (allow, whitelist, rate-limit per key) is the caller's decision.

## Security design

- **Noise_IK** (level 2): the initiator's static key is AEAD-encrypted in message 1 (hidden from passive eavesdroppers); the responder's identity is never sent at all. A WireGuard-style `mac1` (HMAC over a hash of the responder's static key) filters out senders who don't even know the responder's public key, for the cost of one HMAC - before any DH is attempted.
- **AEAD-as-associated-data** (level 4): the fragment header (`msg_id`, `frag_index`, `frag_count`) is passed to the AEAD call as associated data, not encrypted. It needs integrity, not confidentiality - binding it as AD means any tamper to the header *or* the ciphertext fails the same authentication check, before `packet_assembly::Reassembler` (which trusts every field it's handed) ever sees it.
- **Replay window** (level 5): a sliding bitmap (RFC 6479/WireGuard style) rejects a byte-identical replay of an already-accepted nonce. `check()` (pre-decryption) never mutates state; `commit()` only runs after AEAD verification succeeds - otherwise a forged packet with a huge nonce could desync the window for free.
- **Stateless cookie / load gate** (level 5): under measured load, a responder replies to a handshake attempt with a cheap "here's your cookie" instead of spending a DH - a source-spoofing attacker never completes the round trip needed to make the responder do real work, without the responder keeping any per-source state.
- **Nonce/msg_id separation** (levels 4+6, QUIC-style): a naive design ties packet-identity (the AEAD nonce, replay-checked) to data-identity (the fragment's `msg_id`), which breaks the moment reliability needs to retransmit - a byte-identical retry reuses an already-committed nonce and gets replay-rejected forever, so a sender whose ack was lost can never learn its data arrived. The fix: every transmission attempt (including retries) gets a **fresh nonce** (`connection::encrypt_fragment`), while `msg_id` alone drives dedup/reassembly. A retransmit of an already-completed message is recognized by `reassembler::result::duplicate_of_completed`, which re-marks the slot dirty so a fresh SACK goes out - the receiver-side half of the fix that makes retrying and replay protection compatible at all.
- **AIMD congestion control without RTT measurement** (level 6): classic Reno-style AIMD doesn't need an RTT sample for the window logic itself. The two signals it uses are already free: an ACK arriving grows the window (slow start below `slow_start_threshold`, `+kCongestionAvoidanceStep` per round trip in congestion avoidance); a retransmit actually firing (the only loss signal available) halves it. A decrease is capped at one per cooldown window (`initial_timeout`) - a burst of loss spanning several consecutive retransmit ticks is one congestion event, not one per tick, which would otherwise cascade a single event into a much deeper collapse than one halving.
- **NAT keepalive** (level 7): UDP has no connection-reset signal, so an idle session's NAT/firewall mapping can silently expire (carrier-grade NAT in particular can time out well under a minute), breaking the session with no error surfaced to either side. `tick()` tracks how long it's been since a session last had anything to send; once that exceeds `protocol_endpoint<keepalive_interval_ms>`'s own compile-time interval (default 20000ms - a template parameter, like `connection`/`reassembler`'s window size, since every real deployment wants the same value), it sends a type=6 keepalive instead. It's a dedicated wire type, not a reuse of the data channel with an empty payload, specifically so it never touches `packet_assembly::reassembler` and can never surface as a `message_received` event - `handle_keepalive()` only calls `connection::decrypt_keepalive()` (AEAD-authenticated, replay-checked exactly like a data packet's nonce) and drops the result. A session that already sent real traffic (data, a SACK, a retransmit) this tick has its idle clock reset for free and never gets a redundant keepalive stacked on top.
- **Hanging-connection cleanup** (level 7): two distinct timeouts, both template parameters alongside `keepalive_interval_ms` for the same "every real deployment wants the same value" reason. `liveness_timeout_ms` (default 60000ms, ~3 missed keepalive cycles) tears down an established session that's gone quiet - `tick()` tracks `last_received_time` per session (updated only on *authenticated* data/keepalive, or on a SACK, so an unauthenticated packet can't fake liveness) and fires a `peer_unreachable` event carrying the peer's proven static key (not just the now-dead `conn_id`) once it's exceeded. `handshake_timeout_ms` (default 5000ms) is the separate, much shorter timeout for a `connect()` whose responder never answers at all - previously such an attempt sat in `pending_initiators` forever with no signal; now it fires `handshake_failed` like any other rejected handshake. A cookie-reply retry resets this clock (the responder proved it's there, just under load), so a slow-but-progressing retry isn't punished the same as true silence.

## Building

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/unit_tests
```

For ASan/UBSan (much slower, tests only - don't benchmark through this build):

```
cmake -B build-asan -DENABLE_SANITIZERS=ON
cmake --build build-asan -j$(nproc)
./build-asan/unit_tests
```

Dependencies: `liburing` (2.4+ - needs `io_uring_setup_buf_ring`), `libsodium` (1.0.19+ - needs the HKDF-SHA256 functions), CMake 3.20+, a C++20 compiler.

## Running the level 7 demo

```
./build/level7_protocol_server <port> [load_threshold] [initial_cwnd] [initial_ssthresh]
./build/level7_protocol_client --server-pubkey <64 hex printed by the server> --host H --port P \
    [--message-size B] [--in-flight K] [--seconds S] [--reliable] [--conn-id N] \
    [--initial-cwnd N] [--initial-ssthresh N]
```

`--initial-cwnd`/`--initial-ssthresh` (client flags, or positional args 3/4 on the server) are an escape hatch for a link whose real capacity you already know from prior measurement - raising them skips most of the slow-start ramp. Guessing too high on a link of *unknown* capacity risks the same early congestion collapse this project hit and fixed during tuning; the defaults are the safe, conservative starting point.

## Benchmarks

**Local (16-core desktop, loopback):**

| Level | Metric | Result |
|---|---|---|
| 1 - raw io_uring baseline | pps | ~383,000 |
| 2 - Noise_IK handshake | handshakes/s | ~2,300 (full), ~4,600 (one-sided) |
| 3 - fragment reassembly | pps | ~322,000 |
| 4 - full transport (handshake+AEAD+reassembly) | pps | ~310,000 |
| 5 - anti-DoS under flood | legit handshakes still complete 20/20 while dh_attempts/s collapses under a ~6,400 pps flood | |
| 6 - reliability under 0/15/30/50% simulated loss | 199-200/200 delivered, 0 given up | |
| 7 - unified protocol_endpoint, unreliable | pps | ~310,000 |
| 7 - unified protocol_endpoint, reliable | pps | ~290,000, `replay_rejected` stays 0 for the whole run |

**VPS (1 vCPU, 960MB RAM, real network path):** roughly a third of the local numbers (one core shared between client and server processes, plus real network latency instead of loopback) - same qualitative behavior, `replay_rejected=0` throughout, no crashes or collapses. CPU utilization stayed under 20% even at the VPS's peak throughput, meaning the congestion window's ramp-up rate (not CPU or the network) was the binding constraint at the tested durations.

## Testing

101 doctest cases across every layer, covering the happy path, tamper detection on every AEAD'd field, replay rejection, DoS resilience under simulated flood, loss recovery, and the full level-7 integration (real loopback sockets, not mocked I/O). Run with `./build/unit_tests`.
