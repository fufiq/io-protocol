// Level 7: the final server - every earlier level's concern (multishot
// transport, Noise_IK handshake, mac1/cookie anti-DoS, AEAD+reassembly,
// replay window, optional reliability) now lives inside one
// protocol::protocol_endpoint instead of five different ad hoc demo event loops.
// This file is what all of those were approximations of.
//
// Uses submit_and_wait_timeout, not a plain blocking wait - see
// uring_ring.hpp and level7_protocol_client.cpp for why: tick() (which
// checks retransmit timers, rotates the cookie secret, and is what this
// file's own per-second reporting depends on running) only executes after
// the wait call returns. An untimed wait blocks indefinitely if nothing is
// incoming, which starves tick() exactly when a reliable peer's congestion
// window is full and waiting on a retransmit timeout to make progress -
// this server's per-second log going silent was how that bug first showed
// up during development, not just the client hanging.

#include "../net/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9989;
    uint32_t load_threshold = argc > 2 ? (uint32_t)std::atoi(argv[2]) : 2000;
    // Only worth raising above protocol::endpoint_config's own conservative defaults if you already know
    // this deployment's real link capacity from prior measurement - see
    // reliability::reliable_sender's constructor comment for why guessing wrong on an unknown link
    // risks an early collapse instead of saving ramp time.
    size_t initial_cwnd = argc > 3 ? (size_t)std::atoll(argv[3]) : 4;
    size_t initial_ssthresh = argc > 4 ? (size_t)std::atoll(argv[4]) : 64;

    auto server_static = crypto::x25519_generate();
    fprintf(stderr, "[server] static pubkey (hex, share this with the client):\n");
    for (auto b : server_static.pub) fprintf(stderr, "%02x", b);
    fprintf(stderr, "\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 21;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    uring_ring ring(16384);
    protocol::endpoint_config config;
    config.load_threshold_per_sec = load_threshold;
    config.reliable_initial_cwnd = initial_cwnd;
    config.reliable_initial_ssthresh = initial_ssthresh;
    protocol::uring_io io(ring, fd, config.send_pool_size, config.recv_num_bufs, config.recv_buf_size);
    protocol::protocol_endpoint<> endpoint(io, std::move(server_static), config);
    endpoint.arm();
    ring.submit();

    fprintf(stderr, "[server] listening on :%u load_threshold=%u/s\n", port, load_threshold);

    uint64_t message_pps = 0, handshakes = 0;
    uint64_t prev_dh_attempts = 0, prev_cookie_replies = 0;
    auto last_report = std::chrono::steady_clock::now();

    constexpr auto tick_timeout = std::chrono::milliseconds(20);
    for (;;) {
        endpoint.poll_io(tick_timeout);
        for (auto& ev : endpoint.poll_incoming()) {
            switch (ev.type) {
                case protocol::event::event_type::handshake_established: ++handshakes; break;
                case protocol::event::event_type::message_received: ++message_pps; break;
                default: break;
            }
        }
        endpoint.tick(std::chrono::steady_clock::now());
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            uint64_t dh_attempts = endpoint.dh_attempts_count();
            uint64_t cookie_replies = endpoint.cookie_replies_sent_count();
            fprintf(stderr,
                    "[server] handshakes/s=%.0f message_pps=%.0f sessions=%zu dh_attempts/s=%.0f "
                    "cookie_replies/s=%.0f auth_failed=%llu replay_rejected=%llu reassembly_rejected=%llu "
                    "hs_rejected=%llu dropped_no_send_buf=%llu\n",
                    (double)handshakes / secs, (double)message_pps / secs,
                    endpoint.session_count(), (double)(dh_attempts - prev_dh_attempts) / secs,
                    (double)(cookie_replies - prev_cookie_replies) / secs,
                    (unsigned long long)endpoint.auth_failed_count(),
                    (unsigned long long)endpoint.replay_rejected_count(),
                    (unsigned long long)endpoint.reassembly_rejected_count(),
                    (unsigned long long)endpoint.handshake_rejected_count(),
                    (unsigned long long)endpoint.dropped_no_send_buf());
            handshakes = message_pps = 0;
            prev_dh_attempts = dh_attempts;
            prev_cookie_replies = cookie_replies;
            last_report = now;
        }
    }
}
