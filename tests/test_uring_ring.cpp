#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../net/uring_ring.hpp"

#include <liburing.h>

#include <chrono>

TEST_CASE("nop submit/wait round-trips user_data through a real ring") {
    uring_ring ring(8);

    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 42);

    unsigned submitted = ring.submit_and_wait(1);
    CHECK(submitted == 1);

    uint64_t seen_data = 0;
    int seen_res = -1;
    unsigned n = ring.drain_cqes([&](io_uring_cqe* cqe) {
        seen_data = io_uring_cqe_get_data64(cqe);
        seen_res = cqe->res;
    });

    CHECK(n == 1);
    CHECK(seen_data == 42);
    CHECK(seen_res == 0);
}

TEST_CASE("drain_cqes processes a whole batch and advances the CQ exactly once") {
    uring_ring ring(64);

    constexpr unsigned batch = 16;
    for (unsigned i = 0; i < batch; ++i) {
        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, i);
    }
    unsigned submitted = ring.submit_and_wait(batch);
    CHECK(submitted == batch);

    uint64_t sum = 0;
    unsigned n = ring.drain_cqes([&](io_uring_cqe* cqe) { sum += io_uring_cqe_get_data64(cqe); });

    CHECK(n == batch);
    CHECK(sum == (batch * (batch - 1)) / 2);

    // A second drain on an empty CQ must be a no-op, not a hang or crash.
    unsigned n2 = ring.drain_cqes([&](io_uring_cqe*) { FAIL("no completions should remain"); });
    CHECK(n2 == 0);
}

TEST_CASE("get_sqe throws once the submission queue is exhausted") {
    // A ring this small (entries rounded up to a power of two by the
    // kernel) has a bounded SQ; filling it without submitting must fail
    // loudly instead of returning nullptr for the caller to forget to check.
    uring_ring ring(4);

    unsigned filled = 0;
    CHECK_NOTHROW({
        for (int i = 0; i < 4; ++i) {
            io_uring_sqe* sqe = ring.get_sqe();
            io_uring_prep_nop(sqe);
            ++filled;
        }
    });
    CHECK(filled == 4);

    CHECK_THROWS_AS(ring.get_sqe(), std::runtime_error);
}

TEST_CASE("submit_and_wait_timeout returns real completions immediately, without waiting out the timeout") {
    uring_ring ring(8);
    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 99);

    auto start = std::chrono::steady_clock::now();
    unsigned submitted = ring.submit_and_wait_timeout(1, std::chrono::milliseconds(2000));
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(submitted == 1);
    CHECK(elapsed < std::chrono::milliseconds(500)); // nowhere near the 2s timeout: a real NOP completes instantly

    uint64_t seen = 0;
    unsigned n = ring.drain_cqes([&](io_uring_cqe* cqe) { seen = io_uring_cqe_get_data64(cqe); });
    CHECK(n == 1);
    CHECK(seen == 99);
}

TEST_CASE("submit_and_wait_timeout returns 0 (not an error) when nothing arrives before the deadline") {
    // This is the exact property a caller with software-timer-driven work (a
    // retransmit timer, a periodic flush) depends on: with no completions
    // outstanding at all, this must still return in roughly `timeout`, not
    // block forever - see the class-level comment in uring_ring.hpp for the
    // real deadlock this was written to prevent.
    uring_ring ring(8);

    auto start = std::chrono::steady_clock::now();
    unsigned n = ring.submit_and_wait_timeout(1, std::chrono::milliseconds(100));
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(n == 0);
    CHECK(elapsed >= std::chrono::milliseconds(90));  // actually waited, not a false-positive immediate return
    CHECK(elapsed < std::chrono::milliseconds(1000)); // and didn't hang well past the requested timeout either
}

TEST_CASE("move construction transfers ownership without double-free on exit") {
    uring_ring a(8);
    uring_ring b(std::move(a));

    io_uring_sqe* sqe = b.get_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 7);
    b.submit_and_wait(1);

    uint64_t seen = 0;
    unsigned n = b.drain_cqes([&](io_uring_cqe* cqe) { seen = io_uring_cqe_get_data64(cqe); });
    CHECK(n == 1);
    CHECK(seen == 7);
}
