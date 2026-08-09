// Level 5 demonstration server: level 4's transport server plus the cookie
// (mac2) mechanism from anti_dos.hpp, gated by a global load_gate. Below the
// load threshold, handshakes proceed exactly like level 4 (mac2 isn't even
// checked - cookies are an escalation, not a permanent tax). Above it, a
// message1 without a valid mac2 gets a cheap cookie-reply instead of a DH;
// the counters below make the cost difference visible directly (see
// bench/level5_attack_demo.sh): dh_attempts should stay low and
// cookie_replies_sent should absorb the flood once under load.
//
// Wire framing:
//   1 = handshake message1: type(1) | conn_id(8) | noise_msg1(with mac1) | mac2(16, zero if none yet)
//   2 = handshake message2: type(1) | conn_id(8) | noise_msg2
//   3 = data packet:        type(1) | conn_id(8) | <connection wire packet>
//   4 = cookie reply:       type(1) | conn_id(8) | cookie(16)

#include "../net/anti_dos.hpp"
#include "../net/multishot_udp.hpp"
#include "../net/noise_ik.hpp"
#include "../net/transport_connection.hpp"
#include "../net/uring_ring.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9992;
    unsigned num_slots = argc > 2 ? (unsigned)std::atoi(argv[2]) : 1024;
    uint32_t load_threshold = argc > 3 ? (uint32_t)std::atoi(argv[3]) : 2000;

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
    fprintf(stderr, "[server] listening on :%u slots=%u load_threshold=%u/s\n", port, num_slots, load_threshold);

    anti_dos::cookie_secret cookie_secret;
    anti_dos::load_gate load_gate(load_threshold);
    std::unordered_map<uint64_t, transport::connection<>> connections;

    uint64_t handshakes = 0, handshake_rejected = 0, cookie_replies_sent = 0, dh_attempts = 0;
    uint64_t fragment_pps = 0, message_pps = 0, auth_failed = 0, reassembly_rejected = 0, replay_rejected = 0,
              unknown_conn = 0;
    std::vector<uint8_t> out_message;
    auto last_report = std::chrono::steady_clock::now();

    auto send_reply = [&](uint8_t type, const uint8_t* body, size_t body_len, uint64_t conn_id,
                            const sockaddr* peer_addr, socklen_t peer_addr_len) {
        if (free_send_bufs.empty()) {
            ++dropped_no_send_buf;
            return;
        }
        uint32_t buf_idx = free_send_bufs.back();
        free_send_bufs.pop_back();
        send_slot& s = send_pool[buf_idx];

        s.buf[0] = type;
        std::memcpy(s.buf.data() + 1, &conn_id, 8);
        std::memcpy(s.buf.data() + hdr_len, body, body_len);
        std::memcpy(&s.addr, peer_addr, peer_addr_len);

        s.iov.iov_base = s.buf.data();
        s.iov.iov_len = hdr_len + body_len;
        std::memset(&s.msg, 0, sizeof(s.msg));
        s.msg.msg_name = &s.addr;
        s.msg.msg_namelen = peer_addr_len;
        s.msg.msg_iov = &s.iov;
        s.msg.msg_iovlen = 1;

        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_sendmsg(sqe, fd, &s.msg, 0);
        io_uring_sqe_set_data64(sqe, send_tag | buf_idx);
    };

    for (;;) {
        auto now = std::chrono::steady_clock::now();
        cookie_secret.maybe_rotate(now);

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
            auto now_inner = std::chrono::steady_clock::now();

            if (type == 1) {
                constexpr size_t mac2_len = anti_dos::cookie_len;
                if (frag->payload_len <= hdr_len + mac2_len) {
                    ++handshake_rejected;
                    return;
                }
                size_t noise_len = frag->payload_len - hdr_len - mac2_len;
                const uint8_t* noise_msg_ptr = frag->payload + hdr_len;
                const uint8_t* mac2_ptr = frag->payload + hdr_len + noise_len;

                if (!noise::noise_ik_responder::verify_mac1(server_static.pub, noise_msg_ptr, noise_len)) {
                    ++handshake_rejected;
                    return;
                }
                load_gate.record_attempt(now_inner);

                if (load_gate.under_load(now_inner)) {
                    anti_dos::cookie received_mac2;
                    std::memcpy(received_mac2.data(), mac2_ptr, mac2_len);
                    bool zero_mac2 =
                        std::all_of(received_mac2.begin(), received_mac2.end(), [](uint8_t b) { return b == 0; });

                    const auto* addr_bytes = (const uint8_t*)frag->peer_addr;
                    bool mac2_ok = !zero_mac2 && cookie_secret.verify_mac2(addr_bytes, frag->peer_addr_len,
                                                                             noise_msg_ptr, noise_len, received_mac2);
                    if (!mac2_ok) {
                        auto cookie = cookie_secret.compute(addr_bytes, frag->peer_addr_len);
                        send_reply(4, cookie.data(), cookie.size(), conn_id, frag->peer_addr, frag->peer_addr_len);
                        ++cookie_replies_sent;
                        return;
                    }
                }

                std::vector<uint8_t> noise_msg(noise_msg_ptr, noise_msg_ptr + noise_len);
                try {
                    ++dh_attempts;
                    noise::noise_ik_responder responder(server_static);
                    responder.read_message1(noise_msg);
                    auto msg2 = responder.write_message2(nullptr, 0);
                    auto keys = responder.finish();
                    connections.insert_or_assign(
                        conn_id, transport::connection<>(std::move(keys.send), std::move(keys.recv)));

                    send_reply(2, msg2.data(), msg2.size(), conn_id, frag->peer_addr, frag->peer_addr_len);
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

        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr,
                    "[server] handshakes/s=%.0f hs_rejected/s=%.0f dh_attempts/s=%.0f cookie_replies/s=%.0f "
                    "under_load=%d fragment_pps=%.0f message_pps=%.0f connections=%zu\n",
                    (double)handshakes / secs, (double)handshake_rejected / secs,
                    (double)dh_attempts / secs, (double)cookie_replies_sent / secs,
                    load_gate.under_load(now) ? 1 : 0, (double)fragment_pps / secs,
                    (double)message_pps / secs, connections.size());
            handshakes = handshake_rejected = dh_attempts = cookie_replies_sent = fragment_pps = message_pps = 0;
            auth_failed = reassembly_rejected = replay_rejected = unknown_conn = 0;
            last_report = now;
        }
    }
}
