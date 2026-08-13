#pragma once
// epoll_udp.hpp - the same protocol, on epoll, for platforms where io_uring is
// not an option.
//
// On Android that is not a matter of an old kernel: io_uring is blocked for
// apps by seccomp, deliberately, after a run of kernel exploits. So the choice
// is not "epoll until Android catches up" - it is epoll, permanently, for
// anything that ships in an app.
//
// Implements the contract in net/io_backend.hpp and nothing else: arm(), send(),
// can_send(), poll(). protocol_endpoint does not know which one it has.
//
// WHAT IS DIFFERENT FROM THE io_uring BACKEND, and each difference is a
// simplification rather than a compromise:
//
//   No provided buffer ring. The kernel fills buffers this file owns, so there
//   is no rule about handing them back and no way to run out of them mid-flight.
//
//   No send pool and no completion for a send. sendmsg has copied the bytes by
//   the time it returns, so a send buffer is a local, and can_send() is always
//   true. The endpoint's dropped_missing_send_buffer_count therefore stays zero
//   here, which is correct rather than a gap: there is no such failure to count.
//
//   recvmmsg, not recvmsg. The protocol is built on batching and it would be a
//   poor trade to give that up at the last layer. One syscall returns up to
//   `batch` datagrams with their source addresses. bionic has had it since
//   API 21, as it has sendmmsg.
//
// LEVEL-TRIGGERED, deliberately. Edge-triggered epoll obliges a reader to drain
// until EAGAIN or risk a wakeup that never comes again, which is a correctness
// burden with no payoff here: poll() already drains in a loop, and one UDP
// socket does not benefit from the reduced wakeup count edge-triggering buys a
// server with thousands. Level-triggered fails safe - a missed drain costs
// another wakeup, not a stalled connection.

#include "../net/io_backend.hpp"

#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <cstring>
#include <system_error>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace protocol {

    class epoll_udp {
    public:
        // `batch` is how many datagrams one recvmmsg may return, and `mtu` how
        // large each may be. Both are memory: batch * mtu is allocated once and
        // reused, so 64 * 2048 is 128 KB and the default costs less than the
        // io_uring backend's buffer ring.
        explicit epoll_udp(int fd, unsigned batch = 64, unsigned mtu = 2048)
            : fd(fd), storage_(std::size_t(batch) * mtu), msgs(batch), iovs(batch), addrs(batch) {
            // NON-BLOCKING, and this is not optional. poll() drains in a loop
            // while the kernel keeps filling the batch, so the call that finds
            // nothing left must return EAGAIN rather than wait. On a blocking
            // socket that second recvmmsg blocks forever and the whole event
            // loop stops - which is what happened the first time this ran
            // against the protocol tests, whose sockets are plain blocking ones
            // because io_uring never cared.
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
                throw std::system_error(errno, std::system_category(), "fcntl(O_NONBLOCK)");

            epfd = ::epoll_create1(EPOLL_CLOEXEC);
            if (epfd < 0) throw std::system_error(errno, std::system_category(), "epoll_create1");

            epoll_event ev{};
            ev.events = EPOLLIN;          // level-triggered on purpose - see the header
            ev.data.fd = fd;
            if (::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
                const int err = errno;
                ::close(epfd);
                throw std::system_error(err, std::system_category(), "epoll_ctl");
            }

            for (unsigned i = 0; i < batch; ++i) {
                iovs[i].iov_base = storage_.data() + std::size_t(i) * mtu;
                iovs[i].iov_len = mtu;
                std::memset(&msgs[i], 0, sizeof(msgs[i]));
                msgs[i].msg_hdr.msg_iov = &iovs[i];
                msgs[i].msg_hdr.msg_iovlen = 1;
                msgs[i].msg_hdr.msg_name = &addrs[i];
                msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
            }
        }

        ~epoll_udp() { if (epfd >= 0) ::close(epfd); }

        epoll_udp(const epoll_udp&) = delete;
        epoll_udp& operator=(const epoll_udp&) = delete;

        // Nothing to arm: epoll is readiness, not a standing request for
        // completions. Present because the contract has it, and because a
        // caller should not have to know which backend it holds.
        void arm() noexcept {}

        int socket_fd() const noexcept { return fd; }

        // Always. sendmsg copies, so there is no pool to exhaust.
        bool can_send() const noexcept { return true; }

        bool send(uint8_t type, uint64_t conn_id, const uint8_t* body, std::size_t body_len,
                  const sockaddr* addr, socklen_t addr_len) {
            if (hdr_len + body_len > out.size()) return false;
            out[0] = type;
            std::memcpy(out.data() + 1, &conn_id, sizeof(conn_id));
            std::memcpy(out.data() + hdr_len, body, body_len);

            iovec iov{ out.data(), hdr_len + body_len };
            msghdr msg{};
            msg.msg_name = const_cast<sockaddr*>(addr);
            msg.msg_namelen = addr_len;
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;

            // EINTR is worth one retry; everything else is the caller's
            // problem to observe as a lost datagram, which UDP permits and the
            // reliability layer above already handles.
            ssize_t n;
            do { n = ::sendmsg(fd, &msg, 0); } while (n < 0 && errno == EINTR);
            return n >= 0;
        }

        // Wait up to `timeout`, then deliver everything that arrived. A zero
        // timeout polls without blocking, which is what a test loop wants; a
        // negative one would block forever and is clamped to zero, because a
        // protocol that also has timers must never sleep unboundedly.
        void poll(std::chrono::milliseconds timeout, auto&& on_datagram) {
            const int ms = timeout.count() > 0 ? (int)timeout.count() : 0;

            epoll_event ev{};
            int ready;
            do { ready = ::epoll_wait(epfd, &ev, 1, ms); } while (ready < 0 && errno == EINTR);
            if (ready <= 0) return;

            // Keep draining while the kernel keeps filling the batch: a full
            // return means there is very likely more behind it, and stopping
            // there would leave it for another wakeup under exactly the load
            // where wakeups are scarcest.
            for (;;) {
                int n;
                do { n = ::recvmmsg(fd, msgs.data(), (unsigned)msgs.size(), 0, nullptr); }
                while (n < 0 && errno == EINTR);
                if (n <= 0) return;

                for (int i = 0; i < n; ++i) {
                    on_datagram(datagram{
                        (const uint8_t*)iovs[i].iov_base,
                        (std::size_t)msgs[i].msg_len,
                        (const sockaddr*)&addrs[i],
                        msgs[i].msg_hdr.msg_namelen,
                    });
                    // msg_namelen is an in/out field: the kernel overwrites it
                    // with the address size it actually wrote, so it has to be
                    // reset or the next call is told the buffer is smaller than
                    // it is, and source addresses come back truncated.
                    msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
                }
                if ((unsigned)n < msgs.size()) return;
            }
        }

    private:
        int fd;
        int epfd = -1;
        std::vector<uint8_t> storage_;      // batch * mtu, filled by the kernel
        std::vector<mmsghdr> msgs;
        std::vector<iovec> iovs;
        std::vector<sockaddr_storage> addrs;
        std::vector<uint8_t> out = std::vector<uint8_t>(2048);
    };

} // namespace protocol
