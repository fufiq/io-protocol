// Level 5 demonstration, attack simulator: blasts fresh message1 packets
// (real mac1 - computed the same way any legitimate client would, since
// the server's static public key is meant to be public knowledge for a
// reachable service) at maximum rate, fire-and-forget, never waiting for
// or processing a reply. This is exactly the traffic mac1 alone cannot
// filter (see anti_dos.hpp's file header): every packet legitimately knows
// the responder's pubkey, so mac1 passes every time. What should stop this
// from costing the responder a DH per packet is the load_gate + cookie
// mechanism - run this alongside level5_legit_client and watch
// level5_anti_dos_server's dh_attempts/s stay low while cookie_replies/s
// absorbs the flood.
//
// Deliberately does NOT attach a real mac2 (always zero) - an attacker
// flooding from a spoofed source could never complete the cookie round
// trip anyway (they'd never see the reply), so this simulates that
// end-state directly rather than needing actual IP spoofing (which would
// need raw sockets/CAP_NET_ADMIN and isn't the point being demonstrated).

#include "../net/noise_ik.hpp"
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
constexpr size_t mac2_len = 16;

uint64_t random_token() {
    uint64_t t;
    crypto::random_bytes(&t, sizeof(t));
    return t;
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
    std::string_view host = "127.0.0.1";
    uint16_t port = 9992;
    unsigned in_flight = 1024;
    unsigned seconds = 5;
    std::string_view server_pubkey_hex;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--in-flight" && i + 1 < argc) in_flight = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) seconds = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--server-pubkey" && i + 1 < argc) server_pubkey_hex = argv[++i];
    }
    if (server_pubkey_hex.empty()) {
        fprintf(stderr, "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--in-flight N] [--seconds S]\n",
                argv[0]);
        return 1;
    }
    crypto::public_key server_pub = parse_hex_pubkey(server_pubkey_hex);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 21;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, std::string(host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) { perror("connect"); return 1; }
    std::signal(SIGINT, on_sigint);

    uring_ring ring(std::max(64u, in_flight * 2));
    std::vector<send_slot> slots(in_flight);

    std::deque<std::vector<uint8_t>> queue;
    constexpr size_t low_watermark = 256;
    auto refill = [&]() {
        while (queue.size() < low_watermark) {
            // A fresh attacker-side keypair per packet: a real attacker has no
            // reason to reuse one, and it costs nothing extra to demonstrate
            // that reusing keys isn't what makes this traffic "attackable" or not.
            auto attacker_static = crypto::x25519_generate();
            noise::noise_ik_initiator initiator(attacker_static, server_pub);
            auto msg1 = initiator.write_message1(nullptr, 0);

            std::vector<uint8_t> wire(hdr_len + msg1.size() + mac2_len, 0);
            wire[0] = 1;
            uint64_t conn_id = random_token();
            std::memcpy(wire.data() + 1, &conn_id, 8);
            std::memcpy(wire.data() + hdr_len, msg1.data(), msg1.size());
            // trailing mac2 left as zero: never completing the cookie round trip is the point
            queue.push_back(std::move(wire));
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

    for (unsigned i = 0; i < in_flight; ++i) arm_next_send(i);
    ring.submit();

    fprintf(stderr, "[attack] flooding message1 packets, in_flight=%u\n", in_flight);

    uint64_t sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto deadline = start + std::chrono::seconds(seconds);

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
            fprintf(stderr, "[attack] flood_pps=%.0f\n", (double)sent / secs);
            sent = 0;
            last_report = now;
        }
    }
    fprintf(stderr, "[attack] done\n");
    return 0;
}
