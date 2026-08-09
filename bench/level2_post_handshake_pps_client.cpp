// Level 2 follow-up benchmark, client side. Two clearly separated phases:
//
//   Phase 1 (setup, not measured): NUM_CONN sequential, blocking Noise_IK
//   handshakes over a connect()'d UDP socket. Simple send()/recv() - this
//   isn't the hot path, so it doesn't need io_uring or batching.
//
//   Phase 2 (measured): pure data-plane blast, io_uring Send in a tight
//   batched loop exactly like level1_client_bench, round-robining packets
//   across the NUM_CONN already-established sessions. This is the number to
//   compare against the level-1 bare-io_uring baseline (270k pps) and
//   against level2_udp_bench's handshake-only pps: it isolates "what does
//   steady-state AEAD cost on top of the transport," with the (expensive,
//   one-time) handshake cost already paid and out of the measurement.
//
// Nonces are explicit per packet (see level2_post_handshake_pps_server.cpp
// for why), so round-robining across connections and any reordering in
// flight is safe as long as each connection's counter is only ever used
// once - enforced here by a strictly-incrementing per-connection counter
// taken before each encrypt.

#include "../net/noise_ik.hpp"

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
#include <string_view>
#include <vector>

#include "../net/uring_ring.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

constexpr size_t hdr_len = 1 + 8;
constexpr size_t data_hdr_len = hdr_len + 8;

uint64_t random_token() {
    uint64_t t;
    crypto::random_bytes(&t, sizeof(t));
    return t;
}

struct cli_args {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9997;
    unsigned connections = 10;
    unsigned payload = 64;
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
        else if (arg == "--payload" && i + 1 < argc) a.payload = (unsigned)std::atoi(argv[++i]);
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
    std::array<uint8_t, 256> buf;
};

} // namespace

int main(int argc, char** argv) {
    cli_args args = parse_args(argc, argv);
    std::signal(SIGINT, on_sigint);

    if (args.server_pubkey_hex.empty()) {
        fprintf(stderr,
                "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--connections N] "
                "[--payload B] [--in-flight K] [--seconds S]\n",
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
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) {
        perror("connect");
        return 1;
    }

    // --- Phase 1: NUM_CONN sequential, blocking handshakes -----------------

    std::vector<uint64_t> conn_ids(args.connections);
    std::vector<noise::cipher_state> send_keys;
    send_keys.reserve(args.connections);
    std::vector<uint64_t> counters(args.connections, 0);

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
        initiator.read_message2(msg2); // throws noise::handshake_error on failure, aborting the benchmark loudly

        conn_ids[i] = conn_id;
        send_keys.push_back(std::move(initiator.finish().send));
    }
    fprintf(stderr, "[client] phase 1 done: %u sessions established\n", args.connections);

    // --- Phase 2: batched io_uring data-plane blast -------------------------

    uring_ring ring(std::max(64u, args.in_flight * 2));
    std::vector<send_slot> slots(args.in_flight);
    std::vector<uint8_t> plaintext(args.payload, 0xAB);

    auto arm_data_send = [&](unsigned slot_idx, unsigned conn_idx) {
        send_slot& slot = slots[slot_idx];
        uint64_t counter = counters[conn_idx]++;
        auto ct = crypto::aead_encrypt(send_keys[conn_idx].key(), counter, nullptr, 0, plaintext.data(),
                                        plaintext.size());

        slot.buf[0] = 3;
        std::memcpy(slot.buf.data() + 1, &conn_ids[conn_idx], 8);
        std::memcpy(slot.buf.data() + hdr_len, &counter, 8);
        std::memcpy(slot.buf.data() + data_hdr_len, ct.data(), ct.size());
        unsigned total = (unsigned)data_hdr_len + ct.size();

        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_send(sqe, fd, slot.buf.data(), total, 0);
        // low 32 bits: slot index; next connection to use on this slot's next round is derived from a
        // separate round-robin counter, not stored per-slot, so user_data only needs to carry slot_idx.
        io_uring_sqe_set_data64(sqe, slot_idx);
    };

    unsigned rr = 0;
    for (unsigned i = 0; i < args.in_flight; ++i) {
        arm_data_send(i, rr);
        rr = (rr + 1) % args.connections;
    }
    ring.submit();

    uint64_t sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto deadline = start + std::chrono::seconds(args.seconds);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            unsigned slot_idx = (unsigned)io_uring_cqe_get_data64(cqe);
            if (cqe->res > 0) ++sent;
            arm_data_send(slot_idx, rr);
            rr = (rr + 1) % args.connections;
        });
        ring.submit();

        now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] data_pps=%.0f\n", (double)sent / secs);
            sent = 0;
            last_report = now;
        }
    }

    fprintf(stderr, "[client] done\n");
    return 0;
}
