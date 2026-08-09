// Level 4 network benchmark, server side: handshake (level 2) establishes a
// transport::connection per conn_id; every subsequent data packet goes
// through connection::decrypt_and_reassemble (level 4's AD-protected
// decrypt, then level 3's reassembly). Supersedes level2_post_handshake_pps_
// server's ad-hoc decrypt-only approach: that one never reassembled
// anything (single 64-byte payloads only) and didn't authenticate the
// fragment header at all. This is the real thing those benchmarks were
// approximating.
//
// Wire framing (outer, for demuxing before a connection exists):
//   1 = handshake message1:  type(1) | conn_id(8) | noise_msg1
//   2 = handshake message2:  type(1) | conn_id(8) | noise_msg2
//   3 = data packet:         type(1) | conn_id(8) | <connection wire packet>
//
// Retrofitted onto multishot_udp_receiver from the start (no separate classic
// version this time - that migration already happened for levels 1-3).

#include "../net/multishot_udp.hpp"
#include "../net/noise_ik.hpp"
#include "../net/transport_connection.hpp"
#include "../net/uring_ring.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t hdr_len = 1 + 8; // type + conn_id
constexpr uint64_t recv_tag = 1;
constexpr uint64_t send_tag = 1ull << 32;

struct send_slot {
    std::array<uint8_t, 256> buf;
    sockaddr_storage addr{};
    iovec iov{};
    msghdr msg{};
};

} // namespace

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9994;
    unsigned num_slots = argc > 2 ? (unsigned)std::atoi(argv[2]) : 1024;

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

    uring_ring ring(std::max(8192u, num_slots * 8));
    multishot_udp_receiver receiver(ring, fd, recv_tag, /*num_bufs=*/std::max(64u, num_slots), /*buf_size=*/2048);
    receiver.arm();

    std::vector<send_slot> send_pool(std::max(64u, num_slots / 4));
    std::vector<uint32_t> free_send_bufs;
    for (uint32_t i = 0; i < send_pool.size(); ++i) free_send_bufs.push_back(i);
    uint64_t dropped_no_send_buf = 0;

    ring.submit();
    fprintf(stderr, "[server] listening on :%u slots=%u\n", port, num_slots);

    std::unordered_map<uint64_t, transport::connection<>> connections;

    uint64_t handshakes = 0, handshake_rejected = 0;
    uint64_t fragment_pps = 0, message_pps = 0, auth_failed = 0, reassembly_rejected = 0, replay_rejected = 0,
             unknown_conn = 0;
    std::vector<uint8_t> out_message;
    auto last_report = std::chrono::steady_clock::now();

    for (;;) {
        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            uint64_t data = io_uring_cqe_get_data64(cqe);
            if (data & send_tag) {
                free_send_bufs.push_back((uint32_t)(data & ~send_tag));
                return;
            }

            auto frag = receiver.handle_cqe(cqe);
            if (!frag || frag->payload_len <= hdr_len) return;

            uint8_t type = frag->payload[0];
            uint64_t conn_id;
            std::memcpy(&conn_id, frag->payload + 1, 8);

            if (type == 1) {
                std::vector<uint8_t> noise_msg(frag->payload + hdr_len, frag->payload + frag->payload_len);
                try {
                    if (!noise::noise_ik_responder::verify_mac1(server_static.pub, noise_msg.data(), noise_msg.size())) {
                        ++handshake_rejected;
                        return;
                    }
                    noise::noise_ik_responder responder(server_static);
                    responder.read_message1(noise_msg);
                    auto msg2 = responder.write_message2(nullptr, 0);
                    auto keys = responder.finish();
                    connections.insert_or_assign(conn_id, transport::connection<>(std::move(keys.send), std::move(keys.recv)));

                    if (free_send_bufs.empty()) {
                        ++dropped_no_send_buf;
                        return;
                    }
                    uint32_t buf_idx = free_send_bufs.back();
                    free_send_bufs.pop_back();
                    send_slot& s = send_pool[buf_idx];

                    s.buf[0] = 2;
                    std::memcpy(s.buf.data() + 1, &conn_id, 8);
                    std::memcpy(s.buf.data() + hdr_len, msg2.data(), msg2.size());
                    std::memcpy(&s.addr, frag->peer_addr, frag->peer_addr_len);

                    s.iov.iov_base = s.buf.data();
                    s.iov.iov_len = hdr_len + msg2.size();
                    std::memset(&s.msg, 0, sizeof(s.msg));
                    s.msg.msg_name = &s.addr;
                    s.msg.msg_namelen = frag->peer_addr_len;
                    s.msg.msg_iov = &s.iov;
                    s.msg.msg_iovlen = 1;

                    io_uring_sqe* sqe = ring.get_sqe();
                    io_uring_prep_sendmsg(sqe, fd, &s.msg, 0);
                    io_uring_sqe_set_data64(sqe, send_tag | buf_idx);
                    ++handshakes;
                } catch (const noise::handshake_error&) {
                    ++handshake_rejected;
                }
                return;
            }

            if (type == 3) {
                auto it = connections.find(conn_id);
                if (it == connections.end()) {
                    ++unknown_conn;
                    return;
                }
                ++fragment_pps;
                auto res = it->second.decrypt_and_reassemble(frag->payload + hdr_len, frag->payload_len - hdr_len,
                                                               out_message);
                switch (res) {
                    case transport::result::completed: ++message_pps; break;
                    case transport::result::auth_failed: ++auth_failed; break;
                    case transport::result::reassembly_rejected: ++reassembly_rejected; break;
                    case transport::result::replay_rejected: ++replay_rejected; break;
                    case transport::result::incomplete: break;
                    case transport::result::duplicate_of_completed: break;
                }
            }
        });
        receiver.flush();
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr,
                    "[server] handshakes/s=%.0f hs_rejected/s=%.0f fragment_pps=%.0f message_pps=%.0f "
                    "auth_failed/s=%.0f reassembly_rejected/s=%.0f replay_rejected/s=%.0f unknown_conn/s=%.0f "
                    "connections=%zu\n",
                    (double)handshakes / secs, (double)handshake_rejected / secs,
                    (double)fragment_pps / secs, (double)message_pps / secs,
                    (double)auth_failed / secs, (double)reassembly_rejected / secs,
                    (double)replay_rejected / secs, (double)unknown_conn / secs,
                    connections.size());
            handshakes = handshake_rejected = fragment_pps = message_pps = auth_failed = reassembly_rejected =
                replay_rejected = unknown_conn = 0;
            last_report = now;
        }
    }
}
