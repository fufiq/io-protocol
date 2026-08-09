// Level 7 final benchmark client: connects via protocol::protocol_endpoint, then
// keeps its outgoing queue topped up to `in_flight` messages and lets
// tick() drain it every iteration - the number this whole project has been
// measured against since level 1's 270k pps baseline. No credit-counting
// dance needed on the caller's side any more: outgoing_queue_depth() tells
// us exactly how much backlog is still waiting, so topping it up to a
// fixed target self-regulates against whatever rate tick()'s
// flush_outgoing_for can actually sustain (bounded by send_pool_size).
//
// Uses submit_and_wait_timeout, not a plain blocking submit_and_wait - see
// uring_ring.hpp's comment on why: with --reliable traffic, tick() is what
// checks the retransmit timer, and tick() only runs after the wait call
// returns. If the congestion window is full (no new sends possible) and the
// network briefly has nothing incoming, an untimed wait has nothing to
// return for - meaning the retransmit timer can never be reached and
// fired, and the connection can permanently wedge. This is not a
// theoretical concern: it reproduced reliably during development (see the
// git history / conversation this file's design came out of) and is fixed
// by this timeout, which guarantees tick() runs at least every ~20ms
// regardless of I/O activity.

#include "../net/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

constexpr auto tick_timeout = std::chrono::milliseconds(20);

crypto::public_key parse_hex_pubkey(std::string_view hex) {
    crypto::public_key pk{};
    if (hex.size() != crypto::key_len * 2) throw std::runtime_error("--server-pubkey must be 64 hex chars");
    for (size_t i = 0; i < crypto::key_len; ++i) {
        pk[i] = (uint8_t)std::stoul(std::string(hex.substr(i * 2, 2)), nullptr, 16);
    }
    return pk;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9989;
    unsigned message_size = 64;
    unsigned in_flight = 512;
    unsigned seconds = 5;
    bool reliable = false;
    std::string_view server_pubkey_hex;
    uint64_t conn_id = 0; // 0 means "not set by the user" - randomized below if still 0 after parsing
    size_t initial_cwnd = 0;     // 0 means "use protocol::endpoint_config's own default"
    size_t initial_ssthresh = 0; // 0 means "use protocol::endpoint_config's own default"

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--message-size" && i + 1 < argc) message_size = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--in-flight" && i + 1 < argc) in_flight = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) seconds = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--reliable") reliable = true;
        else if (arg == "--server-pubkey" && i + 1 < argc) server_pubkey_hex = argv[++i];
        else if (arg == "--conn-id" && i + 1 < argc) conn_id = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--initial-cwnd" && i + 1 < argc) initial_cwnd = (size_t)std::atoll(argv[++i]);
        else if (arg == "--initial-ssthresh" && i + 1 < argc) {
            initial_ssthresh = (size_t)std::atoll(argv[++i]);
        }
    }
    if (server_pubkey_hex.empty()) {
        fprintf(stderr,
                "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--message-size B] "
                "[--in-flight K] [--seconds S] [--reliable] [--conn-id N] [--initial-cwnd N] "
                "[--initial-ssthresh N]\n"
                "  --conn-id defaults to a random 64-bit value - running two clients against the same\n"
                "  server with the same conn_id makes the second one silently evict the first's session\n"
                "  (they'd otherwise collide, since conn_id is a caller-assigned identifier, not derived\n"
                "  from the socket address).\n"
                "  --initial-cwnd/--initial-ssthresh: only worth raising above the library defaults if\n"
                "  you already know this specific link's real capacity from prior measurement - see\n"
                "  reliability::reliable_sender's constructor comment for why guessing wrong on an unknown\n"
                "  link risks an early collapse instead of saving ramp time.\n",
                argv[0]);
        return 1;
    }
    if (conn_id == 0) {
        std::random_device rd;
        conn_id = ((uint64_t)rd() << 32) | rd();
    }
    crypto::public_key server_pub = parse_hex_pubkey(server_pubkey_hex);
    std::signal(SIGINT, on_sigint);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 21;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, std::string(host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }

    uring_ring ring(std::max(8192u, in_flight * 8));
    protocol::endpoint_config config;
    config.send_pool_size = std::max(1024u, in_flight * 4);
    if (initial_cwnd != 0) config.reliable_initial_cwnd = initial_cwnd;
    if (initial_ssthresh != 0) config.reliable_initial_ssthresh = initial_ssthresh;
    auto client_static = crypto::x25519_generate();
    protocol::protocol_endpoint<> endpoint(ring, fd, std::move(client_static), config);
    endpoint.arm();

    endpoint.connect(conn_id, server_pub, (sockaddr*)&dst, sizeof(dst));
    ring.submit();

    fprintf(stderr, "[client] connecting...\n");
    bool established = false;
    while (!established) {
        ring.submit_and_wait_timeout(1, tick_timeout);
        ring.drain_cqes([&](io_uring_cqe* cqe) { endpoint.handle_cqe(cqe); });
        for (auto& ev : endpoint.poll_incoming()) {
            if (ev.type == protocol::event::event_type::handshake_established) established = true;
            if (ev.type == protocol::event::event_type::handshake_failed) {
                fprintf(stderr, "[client] handshake failed\n");
                return 1;
            }
        }
        endpoint.tick(std::chrono::steady_clock::now());
        ring.submit();
    }
    fprintf(stderr, "[client] connected, message_size=%u in_flight=%u reliable=%d\n", message_size, in_flight,
            reliable);

    std::vector<uint8_t> payload(message_size, 0xAB);

    auto top_up_queue = [&] {
        while (endpoint.outgoing_queue_depth(conn_id) < in_flight) {
            endpoint.send_message(conn_id, payload, reliable);
        }
    };
    top_up_queue();
    endpoint.tick(std::chrono::steady_clock::now());
    ring.submit();

    uint64_t sent_estimate = 0; // measured from the queue-depth drop tick() causes, right after topping up
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto deadline = start + std::chrono::seconds(seconds);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        ring.submit_and_wait_timeout(1, tick_timeout);
        ring.drain_cqes([&](io_uring_cqe* cqe) { endpoint.handle_cqe(cqe); });
        for (auto& ev : endpoint.poll_incoming()) (void)ev; // client doesn't expect inbound messages in this benchmark

        // Top up *before* tick(), not after: whatever tick() doesn't drain into SQEs this
        // iteration won't get submitted until next iteration's tick() call - with the timed wait
        // above that no longer risks a permanent block, but it would still add a needless extra
        // round of latency for no reason, so keep the ordering that avoids it.
        top_up_queue();
        size_t depth_before_tick = endpoint.outgoing_queue_depth(conn_id);
        endpoint.tick(now);
        size_t depth_after_tick = endpoint.outgoing_queue_depth(conn_id);
        if (depth_before_tick > depth_after_tick) sent_estimate += depth_before_tick - depth_after_tick;

        ring.submit();

        now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] message_pps=%.0f dropped_no_send_buf=%llu pending=%zu cwnd=%zu\n",
                    (double)sent_estimate / secs,
                    (unsigned long long)endpoint.dropped_no_send_buf(),
                    endpoint.reliable_pending_count(conn_id), endpoint.reliable_cwnd(conn_id));
            sent_estimate = 0;
            last_report = now;
        }
    }

    fprintf(stderr, "[client] done\n");
    return 0;
}
