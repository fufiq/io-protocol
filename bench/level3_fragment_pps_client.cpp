// Level 3 benchmark, client side: fire-and-forget fragment blaster over a
// connect()'d UDP socket. A background work queue is kept topped up with
// freshly fragmented messages (incrementing msg_id each time); a fixed pool
// of io_uring send slots continuously pulls the next fragment off that
// queue as soon as its previous send completes - decoupling "how a message
// is split into fragments" from "how many sends are in flight" so
// multi-fragment messages don't need special-casing in the slot pool.

#include "../net/packet_assembly.hpp"
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

struct cli_args {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9996;
    unsigned in_flight = 1024;
    unsigned message_size = 64;
    unsigned seconds = 5;
};

cli_args parse_args(int argc, char** argv) {
    cli_args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) a.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) a.port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--in-flight" && i + 1 < argc) a.in_flight = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--message-size" && i + 1 < argc) a.message_size = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) a.seconds = (unsigned)std::atoi(argv[++i]);
    }
    return a;
}

struct send_slot {
    std::array<uint8_t, 1500> buf;
};

} // namespace

int main(int argc, char** argv) {
    cli_args args = parse_args(argc, argv);
    std::signal(SIGINT, on_sigint);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 21;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(args.port);
    if (inet_pton(AF_INET, std::string(args.host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) { perror("connect"); return 1; }

    std::vector<uint8_t> template_payload(args.message_size, 0xAB);
    uint32_t next_msg_id = 0;
    std::deque<std::vector<uint8_t>> queue;

    constexpr size_t low_watermark = 256;
    auto refill = [&]() {
        while (queue.size() < low_watermark) {
            for (auto& frag : assembly::fragment_message(next_msg_id, template_payload.data(), template_payload.size()))
                queue.push_back(std::move(frag));
            ++next_msg_id;
        }
    };
    refill();

    uring_ring ring(std::max(64u, args.in_flight * 2));
    std::vector<send_slot> slots(args.in_flight);

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

    for (unsigned i = 0; i < args.in_flight; ++i) arm_next_send(i);
    ring.submit();

    fprintf(stderr, "[client] message_size=%u fragments_per_message=%zu in_flight=%u\n", args.message_size,
            assembly::fragment_message(0, template_payload.data(), template_payload.size()).size(), args.in_flight);

    uint64_t sent = 0;
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
            arm_next_send(idx);
        });
        ring.submit();

        now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] fragment_pps=%.0f\n", (double)sent / secs);
            sent = 0;
            last_report = now;
        }
    }

    fprintf(stderr, "[client] done\n");
    return 0;
}
