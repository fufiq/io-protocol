// Level 1 load generator: fire-and-forget UDP blaster over io_uring.
// Keeps a fixed pool of N sendmsg slots always in flight: as soon as a send
// completes, the same slot is immediately re-armed and pushed back into the
// same submission batch. No waiting for replies here - this measures pure
// send-side syscall-amortized throughput; server-side pps (see
// level1_echo_server) is the number that matters for the 270k baseline.

#include "../net/uring_ring.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

struct slot {
    std::vector<uint8_t> buf;
    iovec iov{};
    msghdr msg{};
};

void arm_send(uring_ring& ring, slot& slot, unsigned idx, int fd, const sockaddr_in& dst) {
    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_sendmsg(sqe, fd, &slot.msg, 0);
    io_uring_sqe_set_data64(sqe, idx);
    (void)dst;
}

struct cli_args {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9999;
    unsigned slots = 1024;
    unsigned payload = 64;
    unsigned seconds = 5;
};

cli_args parse_args(int argc, char** argv) {
    cli_args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) a.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) a.port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--slots" && i + 1 < argc) a.slots = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--payload" && i + 1 < argc) a.payload = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) a.seconds = (unsigned)std::atoi(argv[++i]);
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    cli_args args = parse_args(argc, argv);
    std::signal(SIGINT, on_sigint);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int bufsz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(args.port);
    if (inet_pton(AF_INET, std::string(args.host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }

    uring_ring ring(std::max(64u, args.slots * 2));

    std::vector<slot> slots(args.slots);
    for (auto& slot : slots) {
        slot.buf.assign(args.payload, 0xAB);
        slot.iov.iov_base = slot.buf.data();
        slot.iov.iov_len = slot.buf.size();
        slot.msg.msg_name = &dst;
        slot.msg.msg_namelen = sizeof(dst);
        slot.msg.msg_iov = &slot.iov;
        slot.msg.msg_iovlen = 1;
    }
    for (unsigned i = 0; i < args.slots; ++i) arm_send(ring, slots[i], i, fd, dst);
    ring.submit();

    uint64_t sent = 0;
    uint64_t total_sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto deadline = start + std::chrono::seconds(args.seconds);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            unsigned idx = (unsigned)io_uring_cqe_get_data64(cqe);
            if (cqe->res > 0) ++sent;
            arm_send(ring, slots[idx], idx, fd, dst);
        });
        ring.submit();

        now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] pps=%.0f\n", (double)sent / secs);
            total_sent += sent;
            sent = 0;
            last_report = now;
        }
    }
    total_sent += sent;

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fprintf(stderr, "[client] total_sent=%llu elapsed=%.2fs avg_pps=%.0f\n",
            (unsigned long long)total_sent, elapsed, (double)total_sent / elapsed);
    return 0;
}
