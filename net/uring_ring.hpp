#pragma once

#include <liburing.h>

#include <chrono>
#include <concepts>
#include <stdexcept>
#include <system_error>

// Thin RAII wrapper around raw liburing. We never touch io_uring_queue_init
// / io_uring_get_sqe / io_uring_submit directly outside this file: rare
// failures (ENOMEM on setup, SQ-full on get_sqe) throw, so the hot per-packet
// path stays branch-free on the happy path instead of threading a result
// through every call site.
//
// Doctest (tests/test_uring_ring.cpp):
// \code{.cpp}
// uring_ring ring(8);
// io_uring_sqe* sqe = ring.get_sqe();
// io_uring_prep_nop(sqe);
// io_uring_sqe_set_data64(sqe, 42);
// ring.submit_and_wait(1);
// unsigned seen = 0;
// unsigned n = ring.drain_cqes([&](io_uring_cqe* cqe) {
//     seen = (unsigned)io_uring_cqe_get_data64(cqe);
// });
// DOCTEST_CHECK(n == 1);
// DOCTEST_CHECK(seen == 42);
// \endcode
class uring_ring {
public:
    explicit uring_ring(unsigned entries, unsigned flags = 0) {
        int ret = io_uring_queue_init(entries, &ring, flags);
        if (ret < 0) throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
        initialized = true;
    }

    ~uring_ring() {
        if (initialized) io_uring_queue_exit(&ring);
    }

    uring_ring(const uring_ring&) = delete;
    uring_ring& operator=(const uring_ring&) = delete;

    uring_ring(uring_ring&& other) noexcept : ring(other.ring), initialized(other.initialized) { other.initialized = false; }
    uring_ring& operator=(uring_ring&& other) noexcept {
        if (this != &other) {
            if (initialized) io_uring_queue_exit(&ring);
            ring = other.ring;
            initialized = other.initialized;
            other.initialized = false;
        }
        return *this;
    }

    // Throws if the submission queue is full instead of returning null:
    // callers never re-check a pointer on the hot path.
    io_uring_sqe* get_sqe() {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (sqe == nullptr) throw std::runtime_error("io_uring_get_sqe: submission queue full");
        return sqe;
    }

    unsigned submit() {
        int ret = io_uring_submit(&ring);
        if (ret < 0) throw std::system_error(-ret, std::system_category(), "io_uring_submit");
        return (unsigned)ret;
    }

    unsigned submit_and_wait(unsigned wait_nr) {
        int ret = io_uring_submit_and_wait(&ring, wait_nr);
        if (ret < 0) throw std::system_error(-ret, std::system_category(), "io_uring_submit_and_wait");
        return (unsigned)ret;
    }

    // Like submit_and_wait, but gives up and returns 0 after `timeout` if no
    // completion arrives - not a knob to tune away, a correctness
    // requirement for anything with software-timer-driven work (a
    // retransmit timer, a periodic flush, anything checked in a caller's
    // "tick" function) sharing the loop with network I/O. A plain,
    // untimed submit_and_wait(1) blocks until a completion exists; if the
    // *only* thing that would ever produce new network activity is
    // gated behind that same timer actually firing (e.g. a reliability
    // layer's congestion window is full, waiting on either an ACK or a
    // retransmit timeout to make room), the loop can never reach the tick
    // call that checks the timer - a real deadlock this project hit and
    // confirmed by instrumentation, not a hypothetical. Every caller
    // whose loop also drives time-based logic must use this, not the
    // untimed version, however unlikely the network seems to go quiet.
    unsigned submit_and_wait_timeout(unsigned wait_nr, std::chrono::milliseconds timeout) {
        __kernel_timespec ts;
        ts.tv_sec = timeout.count() / 1000;
        ts.tv_nsec = (timeout.count() % 1000) * 1000000;
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_submit_and_wait_timeout(&ring, &cqe, wait_nr, &ts, nullptr);
        if (ret == -ETIME) return 0; // timed out with nothing ready - expected, not an error
        if (ret < 0) throw std::system_error(-ret, std::system_category(), "io_uring_submit_and_wait_timeout");
        return (unsigned)ret;
    }

    // Drains every currently-available CQE in one batch, invoking fn(cqe) for each, then advances
    // the CQ tail once for the whole batch (single atomic-store style update instead of one per
    // completion). Takes `fn` as `auto&&` rather than a named template parameter - same effect,
    // shorter to read. Constrained with `requires` instead of left unconstrained: passing a
    // callable with the wrong signature now fails right here with "constraint not satisfied",
    // not several template-instantiation frames deep inside the loop body below.
    unsigned drain_cqes(auto&& fn)
        requires std::invocable<decltype(fn), io_uring_cqe*>
    {
        unsigned count = 0;
        unsigned head;
        io_uring_cqe* cqe;
        io_uring_for_each_cqe(&ring, head, cqe) {
            fn(cqe);
            ++count;
        }
        if (count > 0) io_uring_cq_advance(&ring, count);
        return count;
    }

    io_uring* raw() { return &ring; }

private:
    io_uring ring{};
    bool initialized = false;
};
