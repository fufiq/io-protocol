#include <doctest/doctest.h>

#include "../net/timer_wheel.hpp"

#include <algorithm>

using namespace reliability;
using namespace std::chrono_literals;

TEST_CASE("a scheduled token fires once its tick comes due") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0); // first call just anchors the wheel's clock, per the class contract

    wheel.schedule(42, 30ms);
    CHECK(wheel.advance(t0 + 10ms).empty());
    CHECK(wheel.advance(t0 + 20ms).empty());
    auto fired = wheel.advance(t0 + 30ms);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == 42);
}

TEST_CASE("cancelling a token before it fires suppresses it") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(7, 20ms);
    wheel.cancel(7);
    auto fired = wheel.advance(t0 + 25ms);
    CHECK(fired.empty());
}

TEST_CASE("multiple tokens scheduled into the same tick all fire together") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(1, 20ms);
    wheel.schedule(2, 20ms);
    wheel.schedule(3, 20ms);
    auto fired = wheel.advance(t0 + 20ms);
    std::sort(fired.begin(), fired.end());
    CHECK(fired == std::vector<timer_token>{1, 2, 3});
}

TEST_CASE("a delay is rounded up to the next tick, never fires early") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(9, 15ms); // ceil(15/10) = 2 ticks
    CHECK(wheel.advance(t0 + 10ms).empty());
    auto fired = wheel.advance(t0 + 20ms);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == 9);
}

TEST_CASE("scheduling a delay that exceeds the wheel's span throws") {
    timer_wheel<16> wheel(10ms); // span = 160ms
    CHECK_THROWS_AS(wheel.schedule(1, 200ms), std::invalid_argument);
    CHECK_NOTHROW(wheel.schedule(1, 150ms));
}

// A non-power-of-two slot count (e.g. timer_wheel<100>) is now a compile
// error (static_assert), not a runtime exception - there's nothing left for
// a doctest CHECK_THROWS_AS to observe here. timer_wheel<128>(10ms) below
// (and every other instantiation in this file) is itself the positive half
// of that check: it wouldn't compile if the static_assert were broken.

TEST_CASE("advancing across many ticks at once (a long pause) still fires everything, doesn't hang") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(5, 20ms);
    // Simulate a huge gap - far more than the wheel's total span - between advance() calls.
    auto fired = wheel.advance(t0 + std::chrono::seconds(100));
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == 5);
}

TEST_CASE("sub-tick advance() calls accumulate without losing time (no drift)") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(1, 10ms);
    // Two half-tick advances should together cover the one full tick needed.
    CHECK(wheel.advance(t0 + 5ms).empty());
    auto fired = wheel.advance(t0 + 10ms);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == 1);
}

TEST_CASE("wraparound: scheduling near the far edge of the wheel still fires at the right tick") {
    timer_wheel<8> wheel(10ms); // span = 80ms, only 7 usable slots ahead of current
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(99, 70ms); // ticks_ahead = 7, the maximum valid value for an 8-slot wheel
    for (int i = 1; i < 7; ++i) {
        CHECK(wheel.advance(t0 + std::chrono::milliseconds(i * 10)).empty());
    }
    auto fired = wheel.advance(t0 + 70ms);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == 99);
}

TEST_CASE("cancelling a token that was never scheduled is a harmless no-op") {
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);
    CHECK_NOTHROW(wheel.cancel(12345));

    wheel.schedule(1, 10ms);
    auto fired = wheel.advance(t0 + 10ms);
    REQUIRE(fired.size() == 1); // the earlier stray cancel() must not have suppressed an unrelated token
    CHECK(fired[0] == 1);
}

TEST_CASE("re-scheduling the same token after it fires works normally (retry pattern)") {
    // This is exactly how reliable_sender re-arms a retransmit after one
    // attempt: same token, fresh delay, each round independent.
    timer_wheel<16> wheel(10ms);
    auto t0 = std::chrono::steady_clock::now();
    wheel.advance(t0);

    wheel.schedule(3, 10ms);
    auto first = wheel.advance(t0 + 10ms);
    REQUIRE(first.size() == 1);

    wheel.schedule(3, 10ms); // re-arm after the "retransmit" was sent
    auto second = wheel.advance(t0 + 20ms);
    REQUIRE(second.size() == 1);
    CHECK(second[0] == 3);
}
