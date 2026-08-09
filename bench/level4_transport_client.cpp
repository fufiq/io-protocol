// Level 4 network benchmark, client side. Phase 1 (setup, not measured):
// NUM_CONN sequential blocking Noise_IK handshakes, same as
// level2_post_handshake_pps_client. Phase 2 (measured): pushes messages of
// `--message-size` bytes through transport::connection::encrypt_message per
// connection, round-robin, via a work queue the same shape as
// level3_fragment_pps_client's - a message may expand to several wire
// packets, and the io_uring send-slot pool just keeps pulling whatever's
// next off the queue regardless of which message or connection it came
// from.
//
// message-size <= max_fragment_payload exercises the same single-packet path
// as level2_post_handshake_pps (directly comparable number); anything
// larger exercises the level-3 reassembly path end-to-end, now under real
// encryption with an authenticated header.

#include "../net/noise_ik.hpp"
#include "../net/transport_connection.hpp"
#include "../net/uring_ring.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string_view>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

constexpr size_t hdr_len = 1 + 8;

uint64_t random_token() {
    uint64_t t;
    crypto::random_bytes(&t, sizeof(t));
    return t;
}

struct cli_args {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9994;
    unsigned connections = 10;
    unsigned message_size = 64;
    unsigned in_flight = 1024;
    unsigned seconds = 5;
    std::string_view server_pubkey_hex;
};

cli_args parse_args(int argc, char** argv) {
    cli_args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) a.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) a.port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--connections" && i + 1 < argc) a.connections = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--message-size" && i + 1 < argc) a.message_size = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--in-flight" && i + 1 < argc) a.in_flight = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) a.seconds = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--server-pubkey" && i + 1 < argc) a.server_pubkey_hex = argv[++i];
    }
    return a;
}

crypto::public_key parse_hex_pubkey(std::string_view hex) {
    crypto::public_key pk{};
    if (hex.size() != crypto::key_len * 2) throw std::runtime_error("--server-pubkey must be 64 hex chars");
    for (size_t i = 0; i < crypto::key_len; ++i) {
        pk[i] = (uint8_t)std::stoul(std::string(hex.substr(i * 2, 2)), nullptr, 16);
    }
    return pk;
}

struct send_slot {
    std::array<uint8_t, 2048> buf;
};

} // namespace

int main(int argc, char** argv) {
    cli_args args = parse_args(argc, argv);
    std::signal(SIGINT, on_sigint);

    if (args.server_pubkey_hex.empty()) {
        fprintf(stderr,
                "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--connections N] "
                "[--message-size B] [--in-flight K] [--seconds S]\n",
                argv[0]);
        return 1;
    }
    crypto::public_key server_pub = parse_hex_pubkey(args.server_pubkey_hex);
    auto client_static = crypto::x25519_generate();

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 21;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(args.port);
    if (inet_pton(AF_INET, std::string(args.host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) { perror("connect"); return 1; }

    // --- Phase 1: sequential, blocking handshakes ---------------------------

    std::vector<uint64_t> conn_ids(args.connections);
    std::vector<transport::connection<>> connections;
    connections.reserve(args.connections);

    fprintf(stderr, "[client] phase 1: performing %u handshakes...\n", args.connections);
    for (unsigned i = 0; i < args.connections; ++i) {
        noise::noise_ik_initiator initiator(client_static, server_pub);
        auto msg1 = initiator.write_message1(nullptr, 0);
        uint64_t conn_id = random_token();

        std::vector<uint8_t> wire(hdr_len + msg1.size());
        wire[0] = 1;
        std::memcpy(wire.data() + 1, &conn_id, 8);
        std::memcpy(wire.data() + hdr_len, msg1.data(), msg1.size());
        if (send(fd, wire.data(), wire.size(), 0) < 0) { perror("send handshake"); return 1; }

        std::array<uint8_t, 2048> rbuf;
        ssize_t n = recv(fd, rbuf.data(), rbuf.size(), 0);
        if (n <= (ssize_t)hdr_len || rbuf[0] != 2) {
            fprintf(stderr, "[client] handshake %u failed: bad reply\n", i);
            return 1;
        }
        uint64_t reply_conn_id;
        std::memcpy(&reply_conn_id, rbuf.data() + 1, 8);
        if (reply_conn_id != conn_id) {
            fprintf(stderr, "[client] handshake %u failed: conn_id mismatch\n", i);
            return 1;
        }
        std::vector<uint8_t> msg2(rbuf.begin() + hdr_len, rbuf.begin() + n);
        initiator.read_message2(msg2);

        conn_ids[i] = conn_id;
        noise::transport_keys keys = initiator.finish();
        connections.emplace_back(std::move(keys.send), std::move(keys.recv));
    }
    fprintf(stderr, "[client] phase 1 done: %u sessions established\n", args.connections);

    // --- Phase 2: batched io_uring data-plane blast, work-queue fed --------

    uring_ring ring(std::max(64u, args.in_flight * 2));
    std::vector<send_slot> slots(args.in_flight);
    std::vector<uint8_t> template_payload(args.message_size, 0xAB);

    std::deque<std::vector<uint8_t>> queue;
    unsigned rr = 0;
    constexpr size_t low_watermark = 256;
    auto refill = [&]() {
        while (queue.size() < low_watermark) {
            auto packets = connections[rr].encrypt_message(template_payload.data(), template_payload.size());
            for (auto& pkt : packets) {
                std::vector<uint8_t> wire(hdr_len + pkt.size());
                wire[0] = 3;
                std::memcpy(wire.data() + 1, &conn_ids[rr], 8);
                std::memcpy(wire.data() + hdr_len, pkt.data(), pkt.size());
                queue.push_back(std::move(wire));
            }
            rr = (rr + 1) % args.connections;
        }
    };
    refill();

    auto arm_next_send = [&](unsigned slot_idx) {
        refill();
        send_slot& slot = slots[slot_idx];
        auto wire = std::move(queue.front());
        queue.pop_front();
        std::memcpy(slot.buf.data(), wire.data(), wire.size());

        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_send(sqe, fd, slot.buf.data(), wire.size(), 0);
        io_uring_sqe_set_data64(sqe, slot_idx);
    };

    for (unsigned i = 0; i < args.in_flight; ++i) arm_next_send(i);
    ring.submit();

    fprintf(stderr, "[client] message_size=%u in_flight=%u connections=%u\n", args.message_size, args.in_flight,
            args.connections);

    uint64_t sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto deadline = start + std::chrono::seconds(args.seconds);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            unsigned idx = (unsigned)io_uring_cqe_get_data64(cqe);
            if (cqe->res > 0) ++sent;
            arm_next_send(idx);
        });
        ring.submit();

        now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] fragment_pps=%.0f\n", (double)sent / secs);
            sent = 0;
            last_report = now;
        }
    }

    fprintf(stderr, "[client] done\n");
    return 0;
}
