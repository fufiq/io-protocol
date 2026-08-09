// Level 2 follow-up benchmark: does a Noise_IK session key tax the level-1
// baseline once the handshake is out of the way? A small, fixed number of
// connections complete their handshake first; every packet after that is
// pure AEAD-encrypted data traffic, counted the same way level 1 counted
// bare pps (receive + decrypt + drop, no echo) so the two numbers are
// directly comparable.
//
// Wire format, byte 0 is an explicit type tag (no ambiguity between
// handshake and data traffic, unlike relying on peer-address-only demux):
//   1 = handshake message1:  type(1) | conn_id(8) | noise_msg1
//   2 = handshake message2:  type(1) | conn_id(8) | noise_msg2
//   3 = data packet:         type(1) | conn_id(8) | counter(8 LE) | ciphertext
//
// The data-packet nonce is carried explicitly on the wire rather than
// reusing cipher_state's internal auto-incrementing counter: UDP doesn't
// guarantee order or delivery, so "the Nth packet received uses nonce N" is
// exactly the assumption that breaks the moment a single packet is lost or
// reordered. cipher_state::key() exists so this benchmark can drive
// crypto::aead_encrypt/decrypt directly with that explicit nonce. A real
// replay-safe receive window is level 4/5's job, not this benchmark's -
// here we just detect and count auth failures if the (rare, on loopback)
// case of loss/reordering desyncs a connection.
//
// Retrofitted onto multishot_udp_receiver: the hot path here (type=3 data
// packets) is pure receive, so this is close to the clean level-3 case;
// only the rare type=1 handshake replies still need a classic sendmsg SQE
// from a small dedicated send-buffer pool.

#include "../net/multishot_udp.hpp"
#include "../net/noise_ik.hpp"
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

constexpr size_t hdr_len = 1 + 8;           // type + conn_id
constexpr size_t data_hdr_len = hdr_len + 8; // + explicit counter
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
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9997;
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

    // Handshake replies are rare relative to data traffic, so a modest pool suffices.
    std::vector<send_slot> send_pool(std::max(64u, num_slots / 4));
    std::vector<uint32_t> free_send_bufs;
    for (uint32_t i = 0; i < send_pool.size(); ++i) free_send_bufs.push_back(i);
    uint64_t dropped_no_send_buf = 0;

    ring.submit();

    fprintf(stderr, "[server] listening on :%u slots=%u\n", port, num_slots);

    std::unordered_map<uint64_t, noise::cipher_state> sessions;

    uint64_t handshakes = 0, rejected = 0, data_packets = 0, decrypt_failures = 0, unknown_conn = 0;
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

            size_t total = frag->payload_len;
            uint8_t type = frag->payload[0];
            uint64_t conn_id;
            std::memcpy(&conn_id, frag->payload + 1, 8);

            if (type == 1) {
                std::vector<uint8_t> noise_msg(frag->payload + hdr_len, frag->payload + total);
                try {
                    if (!noise::noise_ik_responder::verify_mac1(server_static.pub, noise_msg.data(), noise_msg.size())) {
                        ++rejected;
                        return;
                    }
                    noise::noise_ik_responder responder(server_static);
                    responder.read_message1(noise_msg);
                    auto msg2 = responder.write_message2(nullptr, 0);
                    auto keys = responder.finish();
                    sessions.insert_or_assign(conn_id, std::move(keys.recv));

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
                    return;
                } catch (const noise::handshake_error&) {
                    ++rejected;
                    return;
                }
            }

            if (type == 3) {
                auto it = sessions.find(conn_id);
                if (it == sessions.end() || total <= data_hdr_len) {
                    ++unknown_conn;
                    return;
                }
                uint64_t counter;
                std::memcpy(&counter, frag->payload + hdr_len, 8);
                size_t ct_len = total - data_hdr_len;
                auto pt = crypto::aead_decrypt(it->second.key(), counter, nullptr, 0, frag->payload + data_hdr_len,
                                                ct_len);
                if (pt) ++data_packets; else ++decrypt_failures;
                return;
            }
        });
        receiver.flush();
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr,
                    "[server] handshakes/s=%.0f rejected/s=%.0f data_pps=%.0f decrypt_fail/s=%.0f "
                    "unknown_conn/s=%.0f sessions=%zu dropped_no_send_buf/s=%.0f\n",
                    (double)handshakes / secs, (double)rejected / secs,
                    (double)data_packets / secs, (double)decrypt_failures / secs,
                    (double)unknown_conn / secs, sessions.size(),
                    (double)dropped_no_send_buf / secs);
            handshakes = rejected = data_packets = decrypt_failures = unknown_conn = 0;
            dropped_no_send_buf = 0;
            last_report = now;
        }
    }
}
