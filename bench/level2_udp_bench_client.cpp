// Level 2 network benchmark, client side: keeps N Noise_IK handshakes
// concurrently in flight against level2_udp_bench_server over a single UDP
// socket / single io_uring ring.
//
// Two pools of persistent, stable-address buffers back every in-flight
// SQE, exactly like level1: io_uring only copies iovec/msghdr *pointers* at
// submit time, not the memory they point to, so any buffer referenced by an
// outstanding SQE must stay untouched until its CQE actually arrives - a
// local std::vector going out of scope, or being overwritten before that
// CQE is seen, is a real (if narrow, timing-dependent) use-after-free /
// data race, not just a style nit.
//   - recv_slots: one persistent buffer per concurrently-outstanding
//     recvmsg, always immediately re-armed after each completion.
//   - send_pool: a free-list of buffers for outgoing message1 packets,
//     sized larger than `concurrency` so a slot starting its next handshake
//     never has to reuse a buffer whose previous send hasn't been
//     confirmed complete by its own CQE yet.
//
// Demuxing concurrent handshakes on one socket needs *some* per-message
// identifier since raw Noise_IK carries none (a fresh ephemeral key isn't a
// usable lookup key before the packet is parsed) - an 8-byte client-chosen
// token is prefixed on the wire, plaintext, outside the Noise/mac1
// envelope. It carries no security weight (Noise never authenticates it):
// worst case a spoofed/duplicated token causes a reply to be matched to the
// wrong pending map entry or dropped, and read_message2's own AEAD tag
// would catch a mismatched message2 immediately. It exists purely so one
// socket can multiplex many concurrent handshake attempts.
//
// As soon as a pending handshake completes (or fails), it immediately
// starts a fresh one - closed-loop concurrency, same model as
// level1_client_bench.

#include "../net/noise_ik.hpp"
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
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

constexpr size_t token_len = 8;
constexpr size_t buf_len = 256;
constexpr uint64_t send_tag = 1ull << 32; // recv slot indices never reach this; keeps the two CQE kinds disjoint

uint64_t random_token() {
    uint64_t t;
    crypto::random_bytes(&t, sizeof(t));
    return t;
}

struct recv_slot {
    std::array<uint8_t, buf_len> buf;
    sockaddr_storage addr{};
    iovec iov{};
    msghdr msg{};
};

void arm_recv(uring_ring& ring, recv_slot& slot, unsigned idx, int fd) {
    std::memset(&slot.msg, 0, sizeof(slot.msg));
    slot.iov.iov_base = slot.buf.data();
    slot.iov.iov_len = slot.buf.size();
    slot.msg.msg_name = &slot.addr;
    slot.msg.msg_namelen = sizeof(slot.addr);
    slot.msg.msg_iov = &slot.iov;
    slot.msg.msg_iovlen = 1;

    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_recvmsg(sqe, fd, &slot.msg, 0);
    io_uring_sqe_set_data64(sqe, idx);
}

struct send_buf {
    std::array<uint8_t, buf_len> data;
    iovec iov{};
    msghdr msg{};
    sockaddr_in dst{};
};

struct cli_args {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9998;
    unsigned concurrency = 512;
    unsigned seconds = 5;
    std::string_view server_pubkey_hex;
};

cli_args parse_args(int argc, char** argv) {
    cli_args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) a.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) a.port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--concurrency" && i + 1 < argc) a.concurrency = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) a.seconds = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--server-pubkey" && i + 1 < argc) a.server_pubkey_hex = argv[++i];
    }
    return a;
}

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
    cli_args args = parse_args(argc, argv);
    std::signal(SIGINT, on_sigint);

    if (args.server_pubkey_hex.empty()) {
        fprintf(stderr, "usage: %s --server-pubkey <64 hex chars from server stderr> [--host H] [--port P] "
                        "[--concurrency N] [--seconds S]\n",
                argv[0]);
        return 1;
    }
    crypto::public_key server_pub = parse_hex_pubkey(args.server_pubkey_hex);
    auto client_static = crypto::x25519_generate();

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(args.port);
    if (inet_pton(AF_INET, std::string(args.host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }

    const unsigned recv_count = args.concurrency;
    const unsigned send_pool_size = args.concurrency * 4; // generous headroom, see file header

    uring_ring ring(std::max(64u, (recv_count + send_pool_size) * 2));

    std::vector<recv_slot> recv_slots(recv_count);
    for (unsigned i = 0; i < recv_count; ++i) arm_recv(ring, recv_slots[i], i, fd);

    std::vector<send_buf> send_pool(send_pool_size);
    std::vector<uint32_t> free_send_bufs;
    free_send_bufs.reserve(send_pool_size);
    for (uint32_t i = 0; i < send_pool_size; ++i) free_send_bufs.push_back(i);

    std::unordered_map<uint64_t, std::unique_ptr<noise::noise_ik_initiator>> pending;
    pending.reserve((size_t)args.concurrency * 2);

    uint64_t dropped_no_send_buf = 0;

    auto start_handshake = [&]() {
        if (free_send_bufs.empty()) {
            ++dropped_no_send_buf; // send pool exhausted (would indicate sends backing up); skip this round
            return;
        }
        uint32_t buf_idx = free_send_bufs.back();
        free_send_bufs.pop_back();
        send_buf& sb = send_pool[buf_idx];

        auto initiator = std::make_unique<noise::noise_ik_initiator>(client_static, server_pub);
        auto msg1 = initiator->write_message1(nullptr, 0);
        uint64_t token = random_token();

        std::memcpy(sb.data.data(), &token, token_len);
        std::memcpy(sb.data.data() + token_len, msg1.data(), msg1.size());
        size_t total = token_len + msg1.size();

        sb.dst = dst;
        sb.iov.iov_base = sb.data.data();
        sb.iov.iov_len = total;
        std::memset(&sb.msg, 0, sizeof(sb.msg));
        sb.msg.msg_name = &sb.dst;
        sb.msg.msg_namelen = sizeof(sb.dst);
        sb.msg.msg_iov = &sb.iov;
        sb.msg.msg_iovlen = 1;

        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_sendmsg(sqe, fd, &sb.msg, 0);
        io_uring_sqe_set_data64(sqe, send_tag | buf_idx);

        pending.emplace(token, std::move(initiator));
    };

    for (unsigned i = 0; i < args.concurrency; ++i) start_handshake();
    ring.submit();

    uint64_t completed = 0;
    uint64_t failed = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    auto deadline = start + std::chrono::seconds(args.seconds);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            uint64_t data = io_uring_cqe_get_data64(cqe);
            if (data & send_tag) {
                uint32_t buf_idx = (uint32_t)(data & ~send_tag);
                free_send_bufs.push_back(buf_idx); // send confirmed complete: buffer is safe to reuse
                return;
            }

            unsigned idx = (unsigned)data;
            recv_slot& slot = recv_slots[idx];
            if (cqe->res > (int)token_len) {
                uint64_t token;
                std::memcpy(&token, slot.buf.data(), token_len);
                auto it = pending.find(token);
                if (it != pending.end()) {
                    size_t total = (size_t)cqe->res;
                    std::vector<uint8_t> msg2(slot.buf.begin() + token_len, slot.buf.begin() + total);
                    try {
                        it->second->read_message2(msg2);
                        ++completed;
                    } catch (const noise::handshake_error&) {
                        ++failed;
                    }
                    pending.erase(it);
                    start_handshake();
                }
            }
            arm_recv(ring, slot, idx, fd);
        });
        ring.submit();

        now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_report).count();
            fprintf(stderr, "[client] handshakes/s=%.0f failed/s=%.0f pending=%zu free_send_bufs=%zu\n",
                    (double)completed / secs, (double)failed / secs, pending.size(),
                    free_send_bufs.size());
            completed = 0;
            failed = 0;
            last_report = now;
        }
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fprintf(stderr, "[client] elapsed=%.2fs dropped_no_send_buf=%llu\n", elapsed,
            (unsigned long long)dropped_no_send_buf);
    return 0;
}
