// Level 6 demonstration client: sends `--count` messages through a
// reliable_sender, deliberately simulating `--loss-pct` percent packet loss
// on every send attempt (initial sends and retransmits alike) by just not
// actually writing the packet to the socket. This exercises the same
// recovery path a real lossy network would, without needing netem/tc or
// raw-socket packet manipulation.
//
// Uses plain blocking sockets with a short receive timeout rather than
// io_uring: this demo is about proving the retransmit/SACK logic actually
// recovers lost fragments, not re-measuring transport pps (already covered
// by level4's benchmarks) - a synchronous loop keeps that story readable.

#include "../net/noise_ik.hpp"
#include "../net/reliability.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t hdr_len = 1 + 8;

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
    uint16_t port = 9991;
    unsigned count = 50;
    unsigned message_size = 3000; // > max_fragment_payload, so most messages are multi-fragment
    double loss_pct = 15.0;
    std::string_view server_pubkey_hex;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--count" && i + 1 < argc) count = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--message-size" && i + 1 < argc) message_size = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--loss-pct" && i + 1 < argc) loss_pct = std::atof(argv[++i]);
        else if (arg == "--server-pubkey" && i + 1 < argc) server_pubkey_hex = argv[++i];
    }
    if (server_pubkey_hex.empty()) {
        fprintf(stderr,
                "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--count N] "
                "[--message-size B] [--loss-pct P]\n",
                argv[0]);
        return 1;
    }
    crypto::public_key server_pub = parse_hex_pubkey(server_pubkey_hex);
    auto client_static = crypto::x25519_generate();

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    timeval tv{0, 20000}; // 20ms recv timeout: short poll interval for the ack/retransmit loop below
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, std::string(host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) { perror("connect"); return 1; }

    // --- Handshake (blocking, not the thing being measured here) -----------

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
    fprintf(stderr, "[client] handshake complete, conn_id=%llu\n", (unsigned long long)conn_id);

    // --- Send `count` messages, simulating loss on every send attempt ------

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> unit(0.0, 100.0);
    auto maybe_send = [&](const std::vector<uint8_t>& wire) -> bool {
        if (unit(rng) < loss_pct) return false; // simulated drop: never actually written to the socket
        std::vector<uint8_t> framed(hdr_len + wire.size());
        framed[0] = 3;
        std::memcpy(framed.data() + 1, &conn_id, 8);
        std::memcpy(framed.data() + hdr_len, wire.data(), wire.size());
        return send(fd, framed.data(), framed.size(), 0) >= 0;
    };

    struct msg_info {
        uint32_t msg_id;
        uint16_t frag_count;
    };
    std::vector<msg_info> messages;
    uint64_t fragments_sent = 0, fragments_dropped_by_sim = 0;

    for (unsigned i = 0; i < count; ++i) {
        std::vector<uint8_t> payload(message_size);
        for (auto& b : payload) b = (uint8_t)rng();
        auto packets = sender.send_message(payload.data(), payload.size());

        auto hdr = assembly::decode_header(packets[0].data() + transport::nonce_len);
        messages.push_back({hdr.msg_id, hdr.frag_count});

        for (auto& pkt : packets) {
            if (maybe_send(pkt)) ++fragments_sent; else ++fragments_dropped_by_sim;
        }
    }
    fprintf(stderr, "[client] sent %u messages (%llu fragments, %llu dropped by loss simulation)\n", count,
            (unsigned long long)fragments_sent, (unsigned long long)fragments_dropped_by_sim);

    // --- Drive retransmits + SACK processing until everything settles ------

    std::unordered_map<uint32_t, bool> acked_complete;
    uint64_t retransmits_sent = 0, retransmits_dropped_by_sim = 0, sacks_received = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(10);

    auto all_done = [&] {
        for (auto& m : messages) {
            if (sender.has_pending(m.msg_id, m.frag_count)) return false;
        }
        return true;
    };

    sender.collect_retransmits(std::chrono::steady_clock::now()); // anchor the wheel's clock
    while (!all_done() && std::chrono::steady_clock::now() < deadline) {
        ssize_t rn = recv(fd, rbuf.data(), rbuf.size(), 0);
        if (rn > (ssize_t)hdr_len && rbuf[0] == 5) {
            uint8_t sack_count = rbuf[hdr_len];
            for (uint8_t i = 0; i < sack_count; ++i) {
                const uint8_t* entry = rbuf.data() + hdr_len + 1 + i * reliability::sack_wire_len;
                if (entry + reliability::sack_wire_len > rbuf.data() + rn) break;
                auto info = reliability::decode_sack(entry);
                sender.handle_ack(info);
                if (info.completed) acked_complete[info.msg_id] = true;
            }
            ++sacks_received;
        }

        auto now = std::chrono::steady_clock::now();
        auto retransmits = sender.collect_retransmits(now);
        for (auto& pkt : retransmits) {
            if (maybe_send(pkt)) ++retransmits_sent; else ++retransmits_dropped_by_sim;
        }
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    unsigned completed = 0, gave_up_msgs = 0, still_pending = 0;
    for (auto& m : messages) {
        if (acked_complete.count(m.msg_id)) ++completed;
        else if (!sender.has_pending(m.msg_id, m.frag_count)) ++gave_up_msgs; // no longer tracked, never fully acked
        else ++still_pending; // deadline hit before resolving either way
    }

    fprintf(stderr,
            "[client] done in %.2fs: completed=%u/%u gave_up=%u still_pending=%u sacks_received=%llu "
            "retransmits_sent=%llu retransmits_dropped_by_sim=%llu\n",
            elapsed, completed, count, gave_up_msgs, still_pending,
            (unsigned long long)sacks_received, (unsigned long long)retransmits_sent,
            (unsigned long long)retransmits_dropped_by_sim);
    return 0;
}
