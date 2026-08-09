// Level 6 pps benchmark: same io_uring batching model as
// level4_transport_client, but every send goes through
// reliability::reliable_sender::send_message instead of
// connection::encrypt_message directly, and incoming SACKs (type 5) are
// processed to release per-fragment tracking. Zero simulated loss here -
// this isolates "what does the bookkeeping (map insert/erase, timer-wheel
// schedule/cancel) cost at steady state" from "does retransmission recover
// loss," which level6_reliability_client's demo already answered
// separately. Point it at level6_reliability_server.

#include "../net/noise_ik.hpp"
#include "../net/reliability.hpp"
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
struct recv_slot {
    std::array<uint8_t, 2048> buf;
};

} // namespace

int main(int argc, char** argv) {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9991;
    unsigned message_size = 64;
    unsigned in_flight = 512;
    // The ack channel needs to be serviced at least as generously as the send
    // path: SACKs arrive in bursts capped at 96 entries/packet (see
    // level6_reliability_server.cpp), so at high message rates *many* SACK
    // packets can be in flight at once. Under-provisioning this (an earlier
    // version defaulted to 64) starved the ack path relative to the send
    // path in the same event loop and caused reliable_sender's pending_ map
    // to grow without bound - a real, worth-documenting finding, not just a
    // benchmark tuning knob.
    unsigned recv_slots_count = 512;
    unsigned seconds = 5;
    std::string_view server_pubkey_hex;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--message-size" && i + 1 < argc) message_size = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--in-flight" && i + 1 < argc) in_flight = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) seconds = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--server-pubkey" && i + 1 < argc) server_pubkey_hex = argv[++i];
    }
    if (server_pubkey_hex.empty()) {
        fprintf(stderr,
                "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--message-size B] "
                "[--in-flight K] [--seconds S]\n",
                argv[0]);
        return 1;
    }
    crypto::public_key server_pub = parse_hex_pubkey(server_pubkey_hex);
    auto client_static = crypto::x25519_generate();

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
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) { perror("connect"); return 1; }
    std::signal(SIGINT, on_sigint);

    // --- Blocking handshake --------------------------------------------------

    uint64_t conn_id;
    crypto::random_bytes(&conn_id, sizeof(conn_id));
    noise::noise_ik_initiator initiator(client_static, server_pub);
    auto msg1 = initiator.write_message1(nullptr, 0);
    {
        std::vector<uint8_t> wire(hdr_len + msg1.size());
        wire[0] = 1;
        std::memcpy(wire.data() + 1, &conn_id, 8);
        std::memcpy(wire.data() + hdr_len, msg1.data(), msg1.size());
        if (send(fd, wire.data(), wire.size(), 0) < 0) { perror("send handshake"); return 1; }
    }
    std::array<uint8_t, 2048> rbuf;
    ssize_t n = recv(fd, rbuf.data(), rbuf.size(), 0);
    if (n <= (ssize_t)hdr_len || rbuf[0] != 2) {
        fprintf(stderr, "handshake failed\n");
        return 1;
    }
    std::vector<uint8_t> msg2(rbuf.begin() + hdr_len, rbuf.begin() + n);
    initiator.read_message2(msg2);
    noise::transport_keys keys = initiator.finish();
    transport::connection<> conn(std::move(keys.send), std::move(keys.recv));
    reliability::reliable_sender sender(conn);
    fprintf(stderr, "[client] handshake complete\n");

    // --- io_uring data-plane loop, through reliable_sender -------------------

    uring_ring ring(std::max(64u, (in_flight + recv_slots_count) * 2));
    std::vector<send_slot> send_slots(in_flight);
    std::vector<recv_slot> recv_slots(recv_slots_count);
    constexpr uint64_t recv_tag_base = 1ull << 40; // disjoint from send slot indices

    std::vector<uint8_t> template_payload(message_size, 0xAB);
    std::deque<std::vector<uint8_t>> queue;
    constexpr size_t low_watermark = 256;
    auto refill = [&]() {
        while (queue.size() < low_watermark) {
            auto packets = sender.send_message(template_payload.data(), template_payload.size());
            for (auto& pkt : packets) {
                std::vector<uint8_t> wire(hdr_len + pkt.size());
                wire[0] = 3;
                std::memcpy(wire.data() + 1, &conn_id, 8);
                std::memcpy(wire.data() + hdr_len, pkt.data(), pkt.size());
                queue.push_back(std::move(wire));
            }
        }
    };
    refill();

    auto arm_next_send = [&](unsigned slot_idx) {
        refill();
        send_slot& slot = send_slots[slot_idx];
        auto wire = std::move(queue.front());
        queue.pop_front();
        std::memcpy(slot.buf.data(), wire.data(), wire.size());

        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_send(sqe, fd, slot.buf.data(), wire.size(), 0);
        io_uring_sqe_set_data64(sqe, slot_idx);
    };

    auto arm_recv = [&](unsigned idx) {
        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_recv(sqe, fd, recv_slots[idx].buf.data(), recv_slots[idx].buf.size(), 0);
        io_uring_sqe_set_data64(sqe, recv_tag_base + idx);
    };

    for (unsigned i = 0; i < in_flight; ++i) arm_next_send(i);
    for (unsigned i = 0; i < recv_slots_count; ++i) arm_recv(i);
    ring.submit();

    fprintf(stderr, "[client] message_size=%u in_flight=%u\n", message_size, in_flight);

    uint64_t sent = 0, sacks_processed = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto last_retransmit_check = start;
    auto deadline = start + std::chrono::seconds(seconds);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            uint64_t data = io_uring_cqe_get_data64(cqe);
            if (data >= recv_tag_base) {
                unsigned idx = (unsigned)(data - recv_tag_base);
                if (cqe->res > (int)hdr_len && recv_slots[idx].buf[0] == 5) {
                    uint8_t count = recv_slots[idx].buf[hdr_len];
                    for (uint8_t i = 0; i < count; ++i) {
                        size_t off = hdr_len + 1 + i * reliability::sack_wire_len;
                        if (off + reliability::sack_wire_len > (size_t)cqe->res) break;
                        sender.handle_ack(reliability::decode_sack(recv_slots[idx].buf.data() + off));
                        ++sacks_processed;
                    }
                }
                arm_recv(idx);
                return;
            }
            unsigned slot_idx = (unsigned)data;
            if (cqe->res > 0) ++sent;
            arm_next_send(slot_idx);
        });
        ring.submit();

        if (now - last_retransmit_check >= std::chrono::milliseconds(20)) {
            auto retransmits = sender.collect_retransmits(now);
            for (auto& pkt : retransmits) {
                std::vector<uint8_t> wire(hdr_len + pkt.size());
                wire[0] = 3;
                std::memcpy(wire.data() + 1, &conn_id, 8);
                std::memcpy(wire.data() + hdr_len, pkt.data(), pkt.size());
                queue.push_front(std::move(wire));
            }
            last_retransmit_check = now;
        }

        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] fragment_pps=%.0f pending=%zu gave_up=%llu sacks_processed=%llu\n",
                    (double)sent / secs, sender.pending_count(),
                    (unsigned long long)sender.gave_up_count(),
                    (unsigned long long)sacks_processed);
            sent = 0;
            last_report = now;
        }
    }

    fprintf(stderr, "[client] done\n");
    return 0;
}
