// Level 1: bare io_uring UDP receiver/echo, single-threaded, no crypto, no
// framing. Purpose: establish the "speed of light" baseline that every later
// layer (handshake, packet assembly, encryption) is measured against.
//
// Retrofitted onto multishot_udp_receiver (multishot recvmsg + provided buffer
// ring): one recvmsg SQE stays posted indefinitely instead of being
// re-armed after every single packet, benchmarked at ~15-20% higher pps than
// the classic per-slot recv/re-arm model this file used to have.
//
// There's no multishot sendmsg, so --echo mode still needs one sendmsg SQE
// per outgoing reply - those use their own small pool of persistent send
// buffers (freed back to the pool on their own send-completion CQE, tagged
// disjointly from the receiver's tag) rather than reusing the receiver's
// provided-buffer memory, which must be returned to the kernel pool
// immediately after being read to keep the multishot recv fed.

#include "../net/multishot_udp.hpp"
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
#include <string_view>
#include <vector>

namespace {

constexpr size_t max_packet = 1472; // typical Ethernet MTU minus IPv4+UDP headers
constexpr uint64_t recv_tag = 1;
constexpr uint64_t send_tag = 1ull << 32; // disjoint from recv tag and from any realistic buffer index

struct send_slot {
    std::array<uint8_t, max_packet> buf;
    sockaddr_storage addr{};
    iovec iov{};
    msghdr msg{};
};

struct cli_args {
    uint16_t port = 9999;
    unsigned capacity = 1024; // recv provided-buffer count and echo send-pool size
    bool echo = false;
};

cli_args parse_args(int argc, char** argv) {
    cli_args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--port" && i + 1 < argc) a.port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--slots" && i + 1 < argc) a.capacity = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--echo") a.echo = true;
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    cli_args args = parse_args(argc, argv);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int bufsz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(args.port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    uring_ring ring(std::max(8192u, args.capacity * 8));
    multishot_udp_receiver receiver(ring, fd, recv_tag, /*num_bufs=*/std::max(64u, args.capacity), /*buf_size=*/2048);
    receiver.arm();

    std::vector<send_slot> send_pool(args.echo ? args.capacity : 0);
    std::vector<uint32_t> free_send_bufs;
    for (uint32_t i = 0; i < send_pool.size(); ++i) free_send_bufs.push_back(i);
    uint64_t dropped_no_send_buf = 0;

    ring.submit();

    fprintf(stderr, "[server] listening on :%u capacity=%u echo=%d\n", args.port, args.capacity, args.echo);

    uint64_t packets = 0;
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
            if (!frag) return;
            ++packets;

            if (args.echo) {
                if (free_send_bufs.empty()) {
                    ++dropped_no_send_buf;
                    return;
                }
                uint32_t buf_idx = free_send_bufs.back();
                free_send_bufs.pop_back();
                send_slot& s = send_pool[buf_idx];

                size_t len = std::min(frag->payload_len, s.buf.size());
                std::memcpy(s.buf.data(), frag->payload, len);
                std::memcpy(&s.addr, frag->peer_addr, frag->peer_addr_len);
                s.iov.iov_base = s.buf.data();
                s.iov.iov_len = len;
                std::memset(&s.msg, 0, sizeof(s.msg));
                s.msg.msg_name = &s.addr;
                s.msg.msg_namelen = frag->peer_addr_len;
                s.msg.msg_iov = &s.iov;
                s.msg.msg_iovlen = 1;

                io_uring_sqe* sqe = ring.get_sqe();
                io_uring_prep_sendmsg(sqe, fd, &s.msg, 0);
                io_uring_sqe_set_data64(sqe, send_tag | buf_idx);
            }
        });
        receiver.flush();
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[server] pps=%.0f malformed=%llu dropped_no_send_buf=%llu\n",
                    (double)packets / secs, (unsigned long long)receiver.malformed_count(),
                    (unsigned long long)dropped_no_send_buf);
            packets = 0;
            last_report = now;
        }
    }
}
