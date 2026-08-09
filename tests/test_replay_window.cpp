#include <doctest/doctest.h>

#include "../net/replay_window.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

using namespace replay;

TEST_CASE("first-ever nonce is accepted regardless of its value") {
    window w;
    CHECK(w.check(500));
    w.commit(500);
    CHECK_FALSE(w.check(500)); // now a replay
}

TEST_CASE("strictly increasing nonces are all accepted") {
    window w;
    for (uint64_t n = 0; n < 5000; ++n) {
        REQUIRE(w.check(n));
        w.commit(n);
    }
}

TEST_CASE("exact duplicate of the current highest is rejected") {
    window w;
    w.commit(10);
    CHECK_FALSE(w.check(10));
}

TEST_CASE("a nonce within the window but not yet seen is accepted, then rejected on replay") {
    window w;
    w.commit(1000);
    CHECK(w.check(998));
    w.commit(998);
    CHECK_FALSE(w.check(998));
    // A sibling within the window that's still fresh must remain accepted.
    CHECK(w.check(997));
}

TEST_CASE("a nonce exactly window_bits behind the highest falls outside the window") {
    window w;
    w.commit(window_bits + 100);
    CHECK_FALSE(w.check(100)); // diff == window_bits exactly: out of range
    CHECK(w.check(101));       // diff == window_bits - 1: still just inside
}

TEST_CASE("advancing the highest by more than the window size clears all prior history") {
    window w;
    w.commit(0);
    w.commit(1);
    w.commit(2);
    // Jump far beyond the window.
    w.commit(window_bits * 10);
    // Old nonces are now unconditionally too old, regardless of whether they
    // were ever actually seen.
    CHECK_FALSE(w.check(0));
    CHECK_FALSE(w.check(1));
    CHECK_FALSE(w.check(2));
}

TEST_CASE("shift across a word boundary preserves already-seen bits correctly") {
    // Regression test for the multi-word bignum shift in shift_left(): pick
    // a shift that isn't a clean multiple of 64 and isn't zero, and confirm
    // bits recorded before the shift land exactly where the window's
    // "diff" arithmetic expects them afterward.
    window w2;
    w2.commit(100);
    w2.commit(90);  // diff 10 from 100
    w2.commit(37);  // diff 63 from 100 (last bit of word 0)
    w2.commit(36);  // diff 64 from 100 (first bit of word 1)

    // Jump highest forward by 70 (crosses a word boundary: 70 = 1*64 + 6).
    w2.commit(170);

    // Relative to new highest 170: nonce 90 is now at diff 80, nonce 37 at
    // diff 133, nonce 36 at diff 134 - all still within the 2048-bit window
    // and all were marked seen, so replays of them must still be rejected.
    CHECK_FALSE(w2.check(90));
    CHECK_FALSE(w2.check(37));
    CHECK_FALSE(w2.check(36));
    // A nonce that was never sent, at a similarly-aged offset, must still be accepted.
    CHECK(w2.check(89));
}

TEST_CASE("out-of-order fragment-style delivery: every unique nonce accepted exactly once") {
    // Simulates how transport::connection actually uses this: many
    // fragments in flight concurrently, nonces assigned in send order but
    // arriving in essentially arbitrary order (see level4's own
    // out-of-order reassembly test for the non-replay half of this).
    window w;
    std::vector<uint64_t> nonces(500);
    std::iota(nonces.begin(), nonces.end(), 0);
    std::mt19937 rng(7);
    std::shuffle(nonces.begin(), nonces.end(), rng);

    for (uint64_t n : nonces) {
        REQUIRE(w.check(n));
        w.commit(n);
    }
    // Every one of them must now read as a replay.
    for (uint64_t n : nonces) {
        CHECK_FALSE(w.check(n));
    }
}

TEST_CASE("a flood of duplicate deliveries for the same nonce is rejected every time after the first") {
    window w;
    REQUIRE(w.check(42));
    w.commit(42);
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(w.check(42));
        // Deliberately not calling commit() again here: check() must not
        // mutate state, so repeated checks without a decrypt in between
        // must be idempotent.
    }
}
