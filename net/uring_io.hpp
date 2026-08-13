#pragma once
// uring_io.hpp - the io_uring half of the protocol's I/O, behind the contract
// protocol_endpoint now talks to instead of talking to a ring directly.
//
// WHY THIS FILE EXISTS. Everything above the socket in net/ - the handshake,
// reassembly, AEAD, the replay window, anti-DoS, reliability - never mentions
// io_uring, and never needed to: 2,000 of net/'s 2,856 lines are already
// portable. The endpoint was the exception, in four places. Putting those four
// behind a backend is what lets the same protocol run on epoll, which matters
// because Android does not merely lack io_uring - it blocks it with seccomp.
//
// The contract is three calls:
//
//   arm()                             start receiving
//   send(type, conn_id, body, addr)   one datagram out
//   poll(timeout, on_fragment)        wait, then hand over everything that came
//
// poll() replaced handle_cqe(cqe), and that is an improvement independent of
// the port: a completion queue is an io_uring concept, and it had no business
// being in the application's loop. Every caller used to write the same
// submit-then-drain dance; now it says poll().
//
// THE SEND POOL LIVES HERE, which is where it belongs. A buffer handed to
// io_uring must stay put until the kernel says it is done with it, so the pool
// and its free list are a lifetime rule of *this* transport - epoll has no such
// rule, since sendmsg has copied the bytes by the time it returns. The endpoint
// used to own that pool and count its exhaustion, which put an io_uring
// constraint in the middle of protocol code.

#include "io_backend.hpp"
#include "multishot_udp.hpp"
#include "uring_ring.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

namespace protocol {

    class uring_io {
    public:
        uring_io(uring_ring& ring, int fd, unsigned send_pool_size = 1024,
                 unsigned recv_num_bufs = 4096, unsigned recv_buf_size = 2048)
            : ring(ring), fd(fd),
              receiver(ring, fd, recv_tag, recv_num_bufs, recv_buf_size),
              send_pool(send_pool_size), send_tag_base(1ull << 40) {
            for (uint32_t i = 0; i < send_pool.size(); ++i) free_send_bufs.push_back(i);
        }

        uring_io(const uring_io&) = delete;
        uring_io& operator=(const uring_io&) = delete;

        void arm() { receiver.arm(); }

        int socket_fd() const noexcept { return fd; }

        // False when the pool is exhausted, which means the kernel has not
        // returned buffers as fast as this is filling them. The endpoint counts
        // it; the decision to count is protocol policy, the exhaustion itself is
        // this transport's business.
        bool send(uint8_t type, uint64_t conn_id, const uint8_t* body, std::size_t body_len,
                  const sockaddr* addr, socklen_t addr_len) {
            if (free_send_bufs.empty()) return false;
            const uint32_t buf_idx = free_send_bufs.back();
            free_send_bufs.pop_back();
            send_slot& s = send_pool[buf_idx];

            s.buf[0] = type;
            std::memcpy(s.buf.data() + 1, &conn_id, sizeof(conn_id));
            std::memcpy(s.buf.data() + hdr_len, body, body_len);
            std::memcpy(&s.addr, addr, addr_len);

            s.iov.iov_base = s.buf.data();
            s.iov.iov_len = hdr_len + body_len;
            std::memset(&s.msg, 0, sizeof(s.msg));
            s.msg.msg_name = &s.addr;
            s.msg.msg_namelen = addr_len;
            s.msg.msg_iov = &s.iov;
            s.msg.msg_iovlen = 1;

            io_uring_sqe* sqe = ring.get_sqe();
            io_uring_prep_sendmsg(sqe, fd, &s.msg, 0);
            io_uring_sqe_set_data64(sqe, send_tag_base | buf_idx);
            return true;
        }

        // Waits up to `timeout` for something to arrive, then delivers every
        // datagram that did. A zero timeout drains what is already there
        // without blocking, which is what a test loop wants.
        void poll(std::chrono::milliseconds timeout, auto&& on_datagram) {
            if (timeout.count() <= 0) ring.submit_and_wait(0);
            else ring.submit_and_wait_timeout(1, timeout);

            ring.drain_cqes([&](io_uring_cqe* cqe) {
                const uint64_t tag = io_uring_cqe_get_data64(cqe);
                if (tag & send_tag_base) {
                    // A send completed; its buffer is the kernel's no longer.
                    free_send_bufs.push_back((uint32_t)(tag & ~send_tag_base));
                    return;
                }
                if (auto frag = receiver.handle_cqe(cqe))
                    on_datagram(datagram{ frag->payload, frag->payload_len,
                                          frag->peer_addr, frag->peer_addr_len });
            });
            receiver.flush();
        }

        // Whether there is room for one more datagram. The protocol asks before
        // starting a new queued item: send() would drop what does not fit, and
        // beginning work known to have nowhere to go is pointless. A transport
        // with no pool of its own simply always says yes.
        bool can_send() const noexcept { return !free_send_bufs.empty(); }
        std::size_t send_buffers_free() const noexcept { return free_send_bufs.size(); }

    private:
        static constexpr uint64_t recv_tag = 1;

        struct send_slot {
            std::array<uint8_t, 2048> buf;
            sockaddr_storage addr{};
            iovec iov{};
            msghdr msg{};
        };

        uring_ring& ring;
        int fd;
        multishot_udp_receiver receiver;
        std::vector<send_slot> send_pool;
        std::vector<uint32_t> free_send_bufs;
        uint64_t send_tag_base;
    };

} // namespace protocol
