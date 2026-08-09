#include <doctest/doctest.h>

#include "../net/packet_assembly.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

using namespace assembly;

namespace {

// Feeds every fragment of `wire_fragments` into `r` in the given order,
// returning the reassembled message once completed (fails the test if it
// never completes).
std::vector<uint8_t> feed_all(reassembler<>& r, std::vector<std::vector<uint8_t>> wire_fragments,
                               const std::vector<size_t>& order) {
    std::vector<uint8_t> out;
    bool completed = false;
    for (size_t i : order) {
        auto& wire = wire_fragments[i];
        fragment_header hdr = decode_header(wire.data());
        auto res = r.add_fragment(hdr, wire.data() + fragment_header_len, wire.size() - fragment_header_len, out);
        if (res == result::completed) completed = true;
    }
    REQUIRE(completed);
    return out;
}

} // namespace

TEST_CASE("header round-trips through encode/decode") {
    fragment_header hdr{0xdeadbeef, 1234, 5678};
    uint8_t buf[fragment_header_len];
    encode_header(hdr, buf);
    auto back = decode_header(buf);
    CHECK(back.msg_id == hdr.msg_id);
    CHECK(back.frag_index == hdr.frag_index);
    CHECK(back.frag_count == hdr.frag_count);
}

TEST_CASE("single-fragment message completes immediately") {
    reassembler<> r;
    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};
    auto frags = fragment_message(1, msg.data(), msg.size());
    REQUIRE(frags.size() == 1);

    std::vector<uint8_t> out;
    fragment_header hdr = decode_header(frags[0].data());
    auto res = r.add_fragment(hdr, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out);
    CHECK(res == result::completed);
    CHECK(out == msg);
}

TEST_CASE("multi-fragment message reassembles in-order delivery correctly") {
    reassembler<> r;
    std::vector<uint8_t> msg(max_fragment_payload * 3 + 123);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)i;

    auto frags = fragment_message(42, msg.data(), msg.size());
    CHECK(frags.size() == 4);

    std::vector<size_t> order(frags.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto out = feed_all(r, frags, order);
    CHECK(out == msg);
}

TEST_CASE("out-of-order and reversed delivery still reassembles byte-exact") {
    reassembler<> r;
    std::vector<uint8_t> msg(max_fragment_payload * 5 + 7);
    std::mt19937 rng(1234);
    for (auto& b : msg) b = (uint8_t)rng();

    auto frags = fragment_message(7, msg.data(), msg.size());
    std::vector<size_t> order(frags.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::reverse(order.begin(), order.end());

    auto out = feed_all(r, frags, order);
    CHECK(out == msg);
}

TEST_CASE("shuffled delivery across many messages reassembles all of them correctly") {
    reassembler<64> r;
    std::mt19937 rng(99);

    struct expected_message {
        uint32_t msg_id;
        std::vector<uint8_t> data;
    };
    std::vector<expected_message> expected;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> all_wire_with_id;

    for (uint32_t id = 0; id < 20; ++id) {
        std::vector<uint8_t> msg(1 + (rng() % (max_fragment_payload * 3)));
        for (auto& b : msg) b = (uint8_t)rng();
        expected.push_back({id, msg});
        for (auto& frag : fragment_message(id, msg.data(), msg.size())) {
            all_wire_with_id.emplace_back(id, std::move(frag));
        }
    }
    std::shuffle(all_wire_with_id.begin(), all_wire_with_id.end(), rng);

    std::vector<std::vector<uint8_t>> completed(20);
    std::vector<bool> done(20, false);
    for (auto& [id, wire] : all_wire_with_id) {
        std::vector<uint8_t> out;
        fragment_header hdr = decode_header(wire.data());
        auto res = r.add_fragment(hdr, wire.data() + fragment_header_len, wire.size() - fragment_header_len, out);
        if (res == result::completed) {
            completed[id] = out;
            done[id] = true;
        }
    }

    for (uint32_t id = 0; id < 20; ++id) {
        REQUIRE(done[id]);
        CHECK(completed[id] == expected[id].data);
    }
}

TEST_CASE("duplicate fragment within an in-progress message is a no-op, not corruption") {
    reassembler<> r;
    std::vector<uint8_t> msg(max_fragment_payload + 10);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)i;
    auto frags = fragment_message(5, msg.data(), msg.size());
    REQUIRE(frags.size() == 2);

    std::vector<uint8_t> out;
    fragment_header hdr0 = decode_header(frags[0].data());
    auto res1 = r.add_fragment(hdr0, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out);
    CHECK(res1 == result::incomplete);
    size_t bytes_after_first = r.bytes_buffered();

    // Re-deliver fragment 0 again before fragment 1 ever arrives.
    auto res_dup = r.add_fragment(hdr0, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out);
    CHECK(res_dup == result::incomplete);
    CHECK(r.bytes_buffered() == bytes_after_first); // no double-counting

    fragment_header hdr1 = decode_header(frags[1].data());
    auto res2 = r.add_fragment(hdr1, frags[1].data() + fragment_header_len, frags[1].size() - fragment_header_len, out);
    CHECK(res2 == result::completed);
    CHECK(out == msg);
}

TEST_CASE("duplicate fragment of an already-completed message is reported, not reopened, and re-dirties the slot") {
    reassembler<> r;
    std::vector<uint8_t> msg = {9, 9, 9};
    auto frags = fragment_message(3, msg.data(), msg.size());

    std::vector<uint8_t> out;
    fragment_header hdr = decode_header(frags[0].data());
    REQUIRE(r.add_fragment(hdr, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out) ==
            result::completed);

    // Drain the dirty flag set by the initial completion so the next check
    // is only about what the duplicate delivery itself does.
    r.drain_dirty_acks();

    // A retransmission of the same (now-completed) message must not be
    // treated as a validation failure - see result::duplicate_of_completed's
    // own comment: this is what lets a sender whose ack got lost learn its
    // data actually arrived, via a fresh SACK.
    auto res = r.add_fragment(hdr, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out);
    CHECK(res == result::duplicate_of_completed);

    auto acks = r.drain_dirty_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].msg_id == hdr.msg_id);
    CHECK(acks[0].completed);
}

TEST_CASE("a gap in the middle leaves the message permanently incomplete without crashing") {
    reassembler<> r;
    std::vector<uint8_t> msg(max_fragment_payload * 2 + 50);
    auto frags = fragment_message(11, msg.data(), msg.size());
    REQUIRE(frags.size() == 3);

    std::vector<uint8_t> out;
    // Deliver fragments 0 and 2, never fragment 1.
    fragment_header hdr0 = decode_header(frags[0].data());
    CHECK(r.add_fragment(hdr0, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out) ==
          result::incomplete);
    fragment_header hdr2 = decode_header(frags[2].data());
    CHECK(r.add_fragment(hdr2, frags[2].data() + fragment_header_len, frags[2].size() - fragment_header_len, out) ==
          result::incomplete);

    // Now deliver the missing one - should complete correctly, proving the gap didn't corrupt ordering.
    fragment_header hdr1 = decode_header(frags[1].data());
    CHECK(r.add_fragment(hdr1, frags[1].data() + fragment_header_len, frags[1].size() - fragment_header_len, out) ==
          result::completed);
    CHECK(out == msg);
}

TEST_CASE("a newer msg_id evicts an unfinished older message sharing the same window slot") {
    reassembler<4> r; // tiny window: msg_id 0 and msg_id 4 collide on slot 0
    std::vector<uint8_t> old_msg(max_fragment_payload + 1);
    std::vector<uint8_t> new_msg = {7, 7};

    auto old_frags = fragment_message(0, old_msg.data(), old_msg.size());
    REQUIRE(old_frags.size() == 2);
    std::vector<uint8_t> out;
    fragment_header old_hdr0 = decode_header(old_frags[0].data());
    CHECK(r.add_fragment(old_hdr0, old_frags[0].data() + fragment_header_len, old_frags[0].size() - fragment_header_len,
                          out) == result::incomplete);
    size_t bytes_with_old_partial = r.bytes_buffered();
    CHECK(bytes_with_old_partial > 0);

    auto new_frags = fragment_message(4, new_msg.data(), new_msg.size()); // 4 % 4 == 0, same slot as msg_id 0
    fragment_header new_hdr0 = decode_header(new_frags[0].data());
    auto res = r.add_fragment(new_hdr0, new_frags[0].data() + fragment_header_len,
                               new_frags[0].size() - fragment_header_len, out);
    CHECK(res == result::completed); // new_msg is a single fragment, completes immediately on eviction
    CHECK(out == new_msg);

    // The evicted old message's partial bytes must have been released, not leaked.
    CHECK(r.bytes_buffered() == 0);

    // The old message's remaining fragment, arriving late, must not be able to resurrect or corrupt the new one.
    fragment_header old_hdr1 = decode_header(old_frags[1].data());
    auto stale_res = r.add_fragment(old_hdr1, old_frags[1].data() + fragment_header_len,
                                     old_frags[1].size() - fragment_header_len, out);
    CHECK(stale_res == result::rejected);
}

TEST_CASE("an older msg_id cannot clobber a newer in-progress message in the same slot") {
    reassembler<4> r;
    std::vector<uint8_t> newer_msg(max_fragment_payload + 1); // 2 fragments, stays incomplete
    auto newer_frags = fragment_message(4, newer_msg.data(), newer_msg.size());
    std::vector<uint8_t> out;
    fragment_header newer_hdr0 = decode_header(newer_frags[0].data());
    CHECK(r.add_fragment(newer_hdr0, newer_frags[0].data() + fragment_header_len,
                          newer_frags[0].size() - fragment_header_len, out) == result::incomplete);

    std::vector<uint8_t> older_msg = {1};
    auto older_frags = fragment_message(0, older_msg.data(), older_msg.size()); // 0 % 4 == 0, same slot, but "older"
    fragment_header older_hdr0 = decode_header(older_frags[0].data());
    auto res = r.add_fragment(older_hdr0, older_frags[0].data() + fragment_header_len,
                               older_frags[0].size() - fragment_header_len, out);
    CHECK(res == result::rejected);

    // Finishing the newer message must still work - it was not clobbered.
    fragment_header newer_hdr1 = decode_header(newer_frags[1].data());
    CHECK(r.add_fragment(newer_hdr1, newer_frags[1].data() + fragment_header_len,
                          newer_frags[1].size() - fragment_header_len, out) == result::completed);
    CHECK(out == newer_msg);
}

TEST_CASE("malformed headers are rejected without touching any slot state") {
    reassembler<> r;
    std::vector<uint8_t> out;
    uint8_t payload[4] = {1, 2, 3, 4};

    CHECK(r.add_fragment({1, 0, 0}, payload, sizeof(payload), out) == result::rejected); // frag_count=0
    CHECK(r.add_fragment({1, 65, 65}, payload, sizeof(payload), out) ==
          result::rejected); // frag_count > max_fragments
    CHECK(r.add_fragment({1, 5, 5}, payload, sizeof(payload), out) ==
          result::rejected); // frag_index >= frag_count
    CHECK(r.bytes_buffered() == 0);
    CHECK(r.rejected_count() == 3);
}

TEST_CASE("oversized payload for a single fragment is rejected") {
    reassembler<> r;
    std::vector<uint8_t> out;
    std::vector<uint8_t> too_big(max_fragment_payload + 1, 0);
    CHECK(r.add_fragment({1, 0, 1}, too_big.data(), too_big.size(), out) == result::rejected);
}

// msg_id -> slot indexing uses `msg_id & (window_capacity - 1)` on the hot
// path instead of a modulo; that's only correct for a power-of-two
// window_capacity. This used to be a runtime constructor check
// (CHECK_THROWS_AS on 0, 100, 4097); now that window_capacity is a
// template parameter, a bad value is a
// static_assert failure - a compile error, not something a doctest
// CHECK_THROWS_AS can observe. reassembler<4>, reassembler<64>,
// reassembler<4096>, etc. compiling at all throughout this file is the
// positive half of this check; there's no way to express the negative half
// (reassembler<100> failing to compile) as a runtime test.

TEST_CASE("fragment_message throws for messages larger than max_message_size") {
    std::vector<uint8_t> huge(max_message_size + 1, 0);
    CHECK_THROWS_AS(fragment_message(1, huge.data(), huge.size()), std::invalid_argument);
}

TEST_CASE("global byte budget rejects new messages once exhausted, without evicting in-progress ones") {
    // Budget fits exactly one max_fragment_payload-sized partial fragment.
    reassembler<16> r(/*global_byte_budget=*/max_fragment_payload);

    std::vector<uint8_t> msg_a(max_fragment_payload * 2); // 2 fragments, will stay incomplete
    auto frags_a = fragment_message(1, msg_a.data(), msg_a.size());
    std::vector<uint8_t> out;
    fragment_header hdr_a0 = decode_header(frags_a[0].data());
    CHECK(r.add_fragment(hdr_a0, frags_a[0].data() + fragment_header_len, frags_a[0].size() - fragment_header_len,
                          out) == result::incomplete);
    CHECK(r.bytes_buffered() == max_fragment_payload);

    // A brand new message now has nowhere to fit under the budget.
    std::vector<uint8_t> msg_b = {1, 2, 3};
    auto frags_b = fragment_message(2, msg_b.data(), msg_b.size());
    fragment_header hdr_b0 = decode_header(frags_b[0].data());
    auto res_b = r.add_fragment(hdr_b0, frags_b[0].data() + fragment_header_len, frags_b[0].size() - fragment_header_len,
                                 out);
    CHECK(res_b == result::rejected);

    // msg_a is untouched and can still complete normally once budget frees up is irrelevant here -
    // finishing it directly (its own fragment 1) must still work, proving it wasn't evicted by the rejection.
    fragment_header hdr_a1 = decode_header(frags_a[1].data());
    // This alone would exceed the budget too (adds another full fragment), so it's also rejected -
    // demonstrating the budget applies uniformly, not just to "new" messages.
    auto res_a1 = r.add_fragment(hdr_a1, frags_a[1].data() + fragment_header_len, frags_a[1].size() - fragment_header_len,
                                  out);
    CHECK(res_a1 == result::rejected);
    CHECK(r.bytes_buffered() == max_fragment_payload); // unchanged: msg_a's first fragment is still safely buffered
}

TEST_CASE("64-fragment message (the bitmask edge case) completes correctly") {
    reassembler<> r;
    std::vector<uint8_t> msg(max_fragment_payload * max_fragments); // exactly 64 fragments, no remainder
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)i * 7;

    auto frags = fragment_message(1, msg.data(), msg.size());
    REQUIRE(frags.size() == max_fragments);

    std::vector<size_t> order(frags.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto out = feed_all(r, frags, order);
    CHECK(out == msg);
}

TEST_CASE("drain_dirty_acks reports a slot only once until it changes again (level 6's SACK hook)") {
    reassembler<> r;
    std::vector<uint8_t> out;

    CHECK(r.drain_dirty_acks().empty()); // nothing has happened yet

    std::vector<uint8_t> msg(max_fragment_payload + 10); // 2 fragments
    auto frags = fragment_message(5, msg.data(), msg.size());
    fragment_header hdr0 = decode_header(frags[0].data());
    r.add_fragment(hdr0, frags[0].data() + fragment_header_len, frags[0].size() - fragment_header_len, out);

    auto acks1 = r.drain_dirty_acks();
    REQUIRE(acks1.size() == 1);
    CHECK(acks1[0].msg_id == 5);
    CHECK(acks1[0].frag_count == 2);
    CHECK(acks1[0].received_mask == 0b01);
    CHECK_FALSE(acks1[0].completed);

    // Nothing changed since the last drain: must be empty now.
    CHECK(r.drain_dirty_acks().empty());

    fragment_header hdr1 = decode_header(frags[1].data());
    auto res = r.add_fragment(hdr1, frags[1].data() + fragment_header_len, frags[1].size() - fragment_header_len, out);
    REQUIRE(res == result::completed);

    auto acks2 = r.drain_dirty_acks();
    REQUIRE(acks2.size() == 1);
    CHECK(acks2[0].msg_id == 5);
    CHECK(acks2[0].received_mask == 0b11);
    CHECK(acks2[0].completed);
}

TEST_CASE("drain_dirty_acks tracks multiple in-progress messages independently") {
    reassembler<> r;
    std::vector<uint8_t> out;
    std::vector<uint8_t> msg_a = {1, 2, 3};
    std::vector<uint8_t> msg_b = {4, 5, 6};

    auto frags_a = fragment_message(1, msg_a.data(), msg_a.size());
    auto frags_b = fragment_message(2, msg_b.data(), msg_b.size());
    fragment_header hdr_a = decode_header(frags_a[0].data());
    fragment_header hdr_b = decode_header(frags_b[0].data());
    r.add_fragment(hdr_a, frags_a[0].data() + fragment_header_len, frags_a[0].size() - fragment_header_len, out);
    r.add_fragment(hdr_b, frags_b[0].data() + fragment_header_len, frags_b[0].size() - fragment_header_len, out);

    auto acks = r.drain_dirty_acks();
    REQUIRE(acks.size() == 2);
    bool saw_a = false, saw_b = false;
    for (auto& a : acks) {
        if (a.msg_id == 1) saw_a = true;
        if (a.msg_id == 2) saw_b = true;
    }
    CHECK(saw_a);
    CHECK(saw_b);
}
