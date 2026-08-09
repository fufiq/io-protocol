// Level 3 benchmark, server side: receive-only, like level1_echo_server's
// --noecho mode (the number we're comparing against is the level-1 bare
// baseline, so this has to count the same way: receive + minimal processing
// + drop, no reply). Every inbound packet is one fragment; each is fed
// through assembly::reassembler. Two numbers matter here:
//   - fragment_pps: raw packets/sec, directly comparable to level 1's
//     ~334-342k multishot baseline (see multishot_udp.hpp).
//   - message_pps: completed (fully reassembled) messages/sec - equal to
//     fragment_pps for single-fragment (small) messages, and a fraction of
//     it (1/frag_count) for larger multi-fragment ones.
//
// Retrofitted onto multishot_udp_receiver: pure-receive servers are the
// cleanest case for that class, since there's no reply path to also keep
// classic per-op sendmsg SQEs around for.

#include "../net/multishot_udp.hpp"
#include "../net/packet_assembly.hpp"
#include "../net/uring_ring.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr uint64_t recv_tag = 1;
}

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 9996;
    unsigned num_bufs = argc > 2 ? (unsigned)std::atoi(argv[2]) : 4096;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 21;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    uring_ring ring(8192);
    multishot_udp_receiver receiver(ring, fd, recv_tag, num_bufs, /*buf_size=*/2048);
    receiver.arm();
    ring.submit();

    assembly::reassembler<> reassembler; // window size is a compile-time template parameter now (see packet_assembly.hpp)
    fprintf(stderr, "[server] listening on :%u num_bufs=%u\n", port, num_bufs);

    uint64_t fragments = 0, messages = 0, rejected = 0;
    std::vector<uint8_t> out_message;
    auto last_report = std::chrono::steady_clock::now();

    for (;;) {
        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            if (io_uring_cqe_get_data64(cqe) != recv_tag) return;
            auto frag = receiver.handle_cqe(cqe);
            if (!frag || frag->payload_len <= assembly::fragment_header_len) return;

            ++fragments;
            auto hdr = assembly::decode_header(frag->payload);
            size_t payload_len = frag->payload_len - assembly::fragment_header_len;
            auto res = reassembler.add_fragment(hdr, frag->payload + assembly::fragment_header_len, payload_len,
                                                 out_message);
            if (res == assembly::result::completed) ++messages;
            else if (res == assembly::result::rejected) ++rejected;
        });
        receiver.flush();
        ring.submit();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr,
                    "[server] fragment_pps=%.0f message_pps=%.0f rejected/s=%.0f bytes_buffered=%zu "
                    "malformed=%llu\n",
                    (double)fragments / secs, (double)messages / secs,
                    (double)rejected / secs, reassembler.bytes_buffered(),
                    (unsigned long long)receiver.malformed_count());
            fragments = messages = rejected = 0;
            last_report = now;
        }
    }
}
