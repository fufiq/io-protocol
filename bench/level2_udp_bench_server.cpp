// Level 2 network benchmark, server side: same batched io_uring UDP loop as
// level 1, but instead of echoing bytes back, each inbound packet is treated
// as a Noise_IK message1 and answered with message2. No handshake state is
// kept across event-loop iterations: read_message1+write_message2 both
// happen synchronously inside one CQE callback, so there is nothing to
// track between messages (the wire-level 8-byte token exists purely so the
// *client* can demux concurrent in-flight handshakes over its one socket;
// the server doesn't need it for anything but echoing it back unchanged).
//
// Retrofitted onto multishot_udp_receiver for the recv side (see
// multishot_udp.hpp); replies still go out one sendmsg SQE at a time from a
// small pool of persistent send buffers, since there's no multishot send.
// This benchmark is crypto-bound (~4-5k handshakes/sec, see
// level2_handshake_bench) rather than transport-bound, so the retrofit is
// mostly about not having two different recv models in the codebase, not
// about moving this particular number.

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
#include <vector>

namespace {

constexpr size_t token_len = 8;
constexpr uint64_t recv_tag = 1;
constexpr uint64_t send_tag = 1ull << 32;

struct send_slot {
    std::array<uint8_t, 256> buf; // message2 wire size is 80 bytes; generous headroom
    sockaddr_storage addr{};
    iovec iov{};
    msghdr msg{};
};

} // namespace

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9998;
    unsigned num_slots = argc > 2 ? (unsigned)std::atoi(argv[2]) : 512;

    auto server_static = crypto::x25519_generate();
    fprintf(stderr, "[server] static pubkey (hex, share this with the client):\n");
    for (auto b : server_static.pub) fprintf(stderr, "%02x", b);
    fprintf(stderr, "\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    uring_ring ring(std::max(8192u, num_slots * 8));
    multishot_udp_receiver receiver(ring, fd, recv_tag, /*num_bufs=*/std::max(64u, num_slots), /*buf_size=*/512);
    receiver.arm();

    std::vector<send_slot> send_pool(num_slots);
    std::vector<uint32_t> free_send_bufs;
    for (uint32_t i = 0; i < num_slots; ++i) free_send_bufs.push_back(i);
    uint64_t dropped_no_send_buf = 0;

    ring.submit();

    fprintf(stderr, "[server] listening on :%u slots=%u\n", port, num_slots);

    uint64_t handshakes = 0;
    uint64_t rejected = 0;
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
            if (!frag || frag->payload_len <= token_len) return;

            size_t total = frag->payload_len;
            std::vector<uint8_t> noise_msg(frag->payload + token_len, frag->payload + total);

            try {
                if (!noise::noise_ik_responder::verify_mac1(server_static.pub, noise_msg.data(), noise_msg.size())) {
                    ++rejected;
                    return;
                }
                noise::noise_ik_responder responder(server_static);
                responder.read_message1(noise_msg);
                auto msg2 = responder.write_message2(nullptr, 0);

                if (free_send_bufs.empty()) {
                    ++dropped_no_send_buf;
                    return;
                }
                uint32_t buf_idx = free_send_bufs.back();
                free_send_bufs.pop_back();
                send_slot& s = send_pool[buf_idx];

                // token is opaque to the server: copy the 8 bytes the client sent straight back.
                std::memcpy(s.buf.data(), frag->payload, token_len);
                std::memcpy(s.buf.data() + token_len, msg2.data(), msg2.size());
                std::memcpy(&s.addr, frag->peer_addr, frag->peer_addr_len);

                s.iov.iov_base = s.buf.data();
                s.iov.iov_len = token_len + msg2.size();
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
                ++rejected;
            }
        });
        receiver.flush();
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[server] handshakes/s=%.0f rejected/s=%.0f dropped_no_send_buf/s=%.0f\n",
                    (double)handshakes / secs, (double)rejected / secs,
                    (double)dropped_no_send_buf / secs);
            handshakes = 0;
            rejected = 0;
            dropped_no_send_buf = 0;
            last_report = now;
        }
    }
}
