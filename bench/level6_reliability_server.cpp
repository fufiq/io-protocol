// Level 6 demonstration server: level 4's transport server, plus periodic
// SACK generation from each connection's reassembler bitmask (via
// connection::drain_dirty_acks(), see transport_connection.hpp/
// packet_assembly.hpp) so a client-side reliable_sender can retransmit
// exactly the fragments that never arrived, instead of guessing or
// resending whole messages blind.
//
// Wire framing:
//   1 = handshake message1, 2 = handshake message2, 3 = data packet (as in level 4)
//   5 = SACK batch: type(1) | conn_id(8) | count(1) | count * 14-byte entries
//       (msg_id:4 | frag_count:2 | received_mask:8, see reliability.hpp's encode_sack)

#include "../net/multishot_udp.hpp"
#include "../net/noise_ik.hpp"
#include "../net/reliability.hpp"
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

constexpr size_t hdr_len = 1 + 8;
constexpr uint64_t recv_tag = 1;
constexpr uint64_t send_tag = 1ull << 32;
// 96 * 14B entries + 1B count + 9B outer header = 1354B, comfortably under the MTU. Kept as large
// as fits in one packet rather than a smaller round number: at high pps, thousands of fragments
// can go dirty in a single server-loop tick, and batching fewer acks per packet just shifts the
// bottleneck onto "how many SACK packets can we send," not "how much does acking cost."
constexpr size_t max_sack_entries_per_packet = 96;

struct send_slot {
    std::array<uint8_t, 2048> buf; // must fit the largest SACK batch: hdr_len + 1 + max_sack_entries_per_packet*14
    sockaddr_storage addr{};
    iovec iov{};
    msghdr msg{};
};

struct peer_info {
    transport::connection<> conn;
    sockaddr_storage addr;
    socklen_t addr_len;
};

} // namespace

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9991;
    unsigned num_slots = argc > 2 ? (unsigned)std::atoi(argv[2]) : 256;

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

    std::vector<send_slot> send_pool(std::max(64u, num_slots / 2));
    std::vector<uint32_t> free_send_bufs;
    for (uint32_t i = 0; i < send_pool.size(); ++i) free_send_bufs.push_back(i);
    uint64_t dropped_no_send_buf = 0;

    ring.submit();
    fprintf(stderr, "[server] listening on :%u slots=%u\n", port, num_slots);

    std::unordered_map<uint64_t, peer_info> peers;

    auto send_wire = [&](const uint8_t* body, size_t body_len, uint8_t type, uint64_t conn_id,
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

    uint64_t handshakes = 0, message_pps = 0, fragment_pps = 0, sacks_sent = 0;
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
                        return;
                    }
                    noise::noise_ik_responder responder(server_static);
                    responder.read_message1(noise_msg);
                    auto msg2 = responder.write_message2(nullptr, 0);
                    auto keys = responder.finish();

                    peer_info info{transport::connection<>(std::move(keys.send), std::move(keys.recv)), {},
                                  frag->peer_addr_len};
                    std::memcpy(&info.addr, frag->peer_addr, frag->peer_addr_len);
                    peers.insert_or_assign(conn_id, std::move(info));

                    send_wire(msg2.data(), msg2.size(), 2, conn_id, frag->peer_addr, frag->peer_addr_len);
                    ++handshakes;
                } catch (const noise::handshake_error&) {
                }
                return;
            }

            if (type == 3) {
                auto it = peers.find(conn_id);
                if (it == peers.end()) return;
                ++fragment_pps;
                auto res = it->second.conn.decrypt_and_reassemble(frag->payload + hdr_len,
                                                                     frag->payload_len - hdr_len, out_message);
                if (res == transport::result::completed) ++message_pps;
            }
        });

        // Batch SACKs for every connection that has dirty (changed) reassembly state since last time.
        for (auto& [conn_id, info] : peers) {
            auto acks = info.conn.drain_dirty_acks();
            for (size_t i = 0; i < acks.size(); i += max_sack_entries_per_packet) {
                size_t n = std::min(max_sack_entries_per_packet, acks.size() - i);
                std::vector<uint8_t> body(1 + n * reliability::sack_wire_len);
                body[0] = (uint8_t)n;
                for (size_t j = 0; j < n; ++j) {
                    auto entry = reliability::encode_sack(acks[i + j]);
                    std::memcpy(body.data() + 1 + j * reliability::sack_wire_len, entry.data(), entry.size());
                }
                send_wire(body.data(), body.size(), 5, conn_id, (sockaddr*)&info.addr,
                          info.addr_len);
                ++sacks_sent;
            }
        }

        receiver.flush();
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr,
                    "[server] handshakes/s=%.0f fragment_pps=%.0f message_pps=%.0f sacks_sent/s=%.0f "
                    "dropped_no_send_buf=%llu peers=%zu\n",
                    (double)handshakes / secs, (double)fragment_pps / secs,
                    (double)message_pps / secs, (double)sacks_sent / secs,
                    (unsigned long long)dropped_no_send_buf, peers.size());
            handshakes = fragment_pps = message_pps = sacks_sent = 0;
            last_report = now;
        }
    }
}
