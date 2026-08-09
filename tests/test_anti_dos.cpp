#include <doctest/doctest.h>

#include "../net/anti_dos.hpp"

using namespace anti_dos;

TEST_CASE("cookie is deterministic for the same secret and address") {
    cookie_secret secret;
    const uint8_t addr1[] = {127, 0, 0, 1, 0x1F, 0x90}; // 127.0.0.1:8080-ish, arbitrary bytes
    auto c1 = secret.compute(addr1, sizeof(addr1));
    auto c2 = secret.compute(addr1, sizeof(addr1));
    CHECK(c1 == c2);
    CHECK(secret.verify(addr1, sizeof(addr1), c1));
}

TEST_CASE("cookie differs for different source addresses under the same secret") {
    cookie_secret secret;
    const uint8_t addr_a[] = {10, 0, 0, 1, 0, 1};
    const uint8_t addr_b[] = {10, 0, 0, 2, 0, 1};
    auto ca = secret.compute(addr_a, sizeof(addr_a));
    auto cb = secret.compute(addr_b, sizeof(addr_b));
    CHECK(ca != cb);
    CHECK_FALSE(secret.verify(addr_b, sizeof(addr_b), ca));
}

TEST_CASE("a cookie from one cookie_secret instance does not verify under an independently-generated one") {
    cookie_secret secret_a;
    cookie_secret secret_b;
    const uint8_t addr[] = {1, 2, 3, 4};
    auto cookie = secret_a.compute(addr, sizeof(addr));
    CHECK_FALSE(secret_b.verify(addr, sizeof(addr), cookie));
}

TEST_CASE("maybe_rotate does nothing before the lifetime elapses") {
    cookie_secret secret(std::chrono::minutes(2));
    const uint8_t addr[] = {9, 9, 9, 9};
    auto before = secret.compute(addr, sizeof(addr));

    secret.maybe_rotate(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    auto after = secret.compute(addr, sizeof(addr));
    CHECK(before == after);
}

TEST_CASE("a cookie issued just before rotation still verifies once (grace period), but not after a second rotation") {
    cookie_secret secret(std::chrono::minutes(2));
    const uint8_t addr[] = {5, 5, 5, 5};
    auto cookie = secret.compute(addr, sizeof(addr));

    auto t0 = std::chrono::steady_clock::now();
    secret.maybe_rotate(t0 + std::chrono::minutes(3)); // first rotation: cookie now keyed under "previous"
    CHECK(secret.verify(addr, sizeof(addr), cookie));  // still accepted via the grace-period check

    secret.maybe_rotate(t0 + std::chrono::minutes(6)); // second rotation: original secret is gone entirely
    CHECK_FALSE(secret.verify(addr, sizeof(addr), cookie));
}

TEST_CASE("load_gate reports no load until the threshold is crossed within a ~1s bucket") {
    load_gate gate(100);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) gate.record_attempt(t0);
    CHECK_FALSE(gate.under_load(t0));

    for (int i = 0; i < 60; ++i) gate.record_attempt(t0);
    CHECK(gate.under_load(t0));
}

TEST_CASE("load_gate's elevated rate persists for one bucket after a burst stops (hysteresis), then clears") {
    load_gate gate(100);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) gate.record_attempt(t0);
    REQUIRE(gate.under_load(t0));

    // New bucket starts, but the *previous* bucket's rate was well above threshold.
    auto t1 = t0 + std::chrono::milliseconds(1100);
    gate.record_attempt(t1); // a single attempt in the new bucket
    CHECK(gate.under_load(t1));

    // Once enough real time passes with no further attempts, the signal must clear -
    // otherwise a one-time burst would permanently gate every future handshake.
    auto t2 = t1 + std::chrono::seconds(5);
    CHECK_FALSE(gate.under_load(t2));
}

TEST_CASE("load_gate never reports load before any attempt has been recorded") {
    load_gate gate(10);
    CHECK_FALSE(gate.under_load(std::chrono::steady_clock::now()));
}

TEST_CASE("mac2 verifies for the exact message it was computed over, and rejects any other message") {
    cookie_secret secret;
    const uint8_t addr[] = {192, 168, 1, 1, 0x1F, 0x90};
    auto cookie = secret.compute(addr, sizeof(addr));

    const uint8_t msg_a[] = "message one";
    const uint8_t msg_b[] = "message two, totally different";

    auto mac2_a = compute_mac2(cookie, msg_a, sizeof(msg_a));
    CHECK(secret.verify_mac2(addr, sizeof(addr), msg_a, sizeof(msg_a), mac2_a));
    CHECK_FALSE(secret.verify_mac2(addr, sizeof(addr), msg_b, sizeof(msg_b), mac2_a));
}

TEST_CASE("mac2 computed for one source address does not verify against a different one") {
    cookie_secret secret;
    const uint8_t addr_a[] = {1, 1, 1, 1};
    const uint8_t addr_b[] = {2, 2, 2, 2};
    const uint8_t msg[] = "retry message1";

    auto cookie_a = secret.compute(addr_a, sizeof(addr_a));
    auto mac2 = compute_mac2(cookie_a, msg, sizeof(msg));

    CHECK(secret.verify_mac2(addr_a, sizeof(addr_a), msg, sizeof(msg), mac2));
    CHECK_FALSE(secret.verify_mac2(addr_b, sizeof(addr_b), msg, sizeof(msg), mac2));
}

TEST_CASE("a cookie stolen from an observed packet cannot be stapled onto a different forged message1") {
    // This is the property that justifies mac2 = MAC(cookie, message) instead
    // of just re-sending the cookie verbatim: an observer who sees a
    // legitimate cookie value (it's not encrypted on the wire) still can't
    // reuse it to make an *arbitrary different* message1 pass the check.
    cookie_secret secret;
    const uint8_t addr[] = {8, 8, 8, 8};
    auto cookie = secret.compute(addr, sizeof(addr));

    const uint8_t legit_msg[] = "the real retry";
    auto legit_mac2 = compute_mac2(cookie, legit_msg, sizeof(legit_msg));

    const uint8_t forged_msg[] = "an attacker-crafted message1 from the same address";
    CHECK_FALSE(secret.verify_mac2(addr, sizeof(addr), forged_msg, sizeof(forged_msg), legit_mac2));
}

TEST_CASE("mac2 survives one secret rotation (grace period) like the raw cookie does") {
    cookie_secret secret(std::chrono::minutes(2));
    const uint8_t addr[] = {4, 4, 4, 4};
    const uint8_t msg[] = "retry";

    auto cookie = secret.compute(addr, sizeof(addr));
    auto mac2 = compute_mac2(cookie, msg, sizeof(msg));

    auto t0 = std::chrono::steady_clock::now();
    secret.maybe_rotate(t0 + std::chrono::minutes(3));
    CHECK(secret.verify_mac2(addr, sizeof(addr), msg, sizeof(msg), mac2));

    secret.maybe_rotate(t0 + std::chrono::minutes(6));
    CHECK_FALSE(secret.verify_mac2(addr, sizeof(addr), msg, sizeof(msg), mac2));
}
