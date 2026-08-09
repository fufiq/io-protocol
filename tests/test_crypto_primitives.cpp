#include <doctest/doctest.h>

#include "../net/crypto_primitives.hpp"

using namespace crypto;

TEST_CASE("x25519 DH agreement matches from both sides") {
    auto a = x25519_generate();
    auto b = x25519_generate();

    auto shared_a = dh(a.priv, b.pub);
    auto shared_b = dh(b.priv, a.pub);

    REQUIRE(shared_a.has_value());
    REQUIRE(shared_b.has_value());
    CHECK(std::memcmp(shared_a->data(), shared_b->data(), key_len) == 0);
}

TEST_CASE("dh rejects the all-zero (identity/low-order) public key") {
    // This is the small-order-point pitfall: a malicious peer sending an
    // all-zero "public key" must not silently produce a predictable shared
    // secret. See the comment on crypto::dh in crypto_primitives.hpp.
    auto a = x25519_generate();
    public_key zero{};
    zero.fill(0);

    auto shared = dh(a.priv, zero);
    CHECK_FALSE(shared.has_value());
}

TEST_CASE("aead encrypt/decrypt round-trips and authenticates associated data") {
    secret_buffer key(key_len);
    random_bytes(key.data(), key_len);

    const uint8_t ad[] = {1, 2, 3, 4};
    const uint8_t pt[] = "hello noise";

    auto ct = aead_encrypt(key, /*counter=*/0, ad, sizeof(ad), pt, sizeof(pt));
    CHECK(ct.size() == sizeof(pt) + aead_tag_len);

    auto decrypted = aead_decrypt(key, 0, ad, sizeof(ad), ct.data(), ct.size());
    REQUIRE(decrypted.has_value());
    CHECK(std::memcmp(decrypted->data(), pt, sizeof(pt)) == 0);
}

TEST_CASE("aead rejects a flipped ciphertext byte") {
    secret_buffer key(key_len);
    random_bytes(key.data(), key_len);
    const uint8_t pt[] = "authenticate me";

    auto ct = aead_encrypt(key, 0, nullptr, 0, pt, sizeof(pt));
    ct[0] ^= 0x01;

    CHECK_FALSE(aead_decrypt(key, 0, nullptr, 0, ct.data(), ct.size()).has_value());
}

TEST_CASE("aead rejects tampered associated data even if ciphertext is untouched") {
    secret_buffer key(key_len);
    random_bytes(key.data(), key_len);
    const uint8_t pt[] = "payload";
    const uint8_t ad[] = "context-a";

    auto ct = aead_encrypt(key, 0, ad, sizeof(ad), pt, sizeof(pt));

    const uint8_t wrong_ad[] = "context-b";
    CHECK_FALSE(aead_decrypt(key, 0, wrong_ad, sizeof(wrong_ad), ct.data(), ct.size()).has_value());
}

TEST_CASE("aead rejects reuse of a nonce with different plaintext under a different key check") {
    // Not a nonce-reuse-safety test (that's a protocol-level invariant owned
    // by cipher_state, tested in test_noise_ik.cpp) - this just pins down
    // that counter is actually mixed into the nonce and not ignored.
    secret_buffer key(key_len);
    random_bytes(key.data(), key_len);
    const uint8_t pt[] = "same plaintext";

    auto ct0 = aead_encrypt(key, 0, nullptr, 0, pt, sizeof(pt));
    auto ct1 = aead_encrypt(key, 1, nullptr, 0, pt, sizeof(pt));
    CHECK(std::memcmp(ct0.data(), ct1.data(), ct0.size()) != 0);

    CHECK_FALSE(aead_decrypt(key, 1, nullptr, 0, ct0.data(), ct0.size()).has_value());
}

TEST_CASE("hkdf2 is deterministic and produces two distinct outputs") {
    hash chaining_key{};
    random_bytes(chaining_key.data(), chaining_key.size());
    const uint8_t ikm[] = "input key material";

    hash out1a{}, out2a{}, out1b{}, out2b{};
    hkdf2(chaining_key, ikm, sizeof(ikm), out1a, out2a);
    hkdf2(chaining_key, ikm, sizeof(ikm), out1b, out2b);

    CHECK(out1a == out1b);
    CHECK(out2a == out2b);
    CHECK(out1a != out2a);
}

TEST_CASE("hkdf3 third output differs from a truncated hkdf2 call") {
    hash chaining_key{};
    random_bytes(chaining_key.data(), chaining_key.size());
    const uint8_t ikm[] = "ikm";

    hash a1{}, a2{};
    hkdf2(chaining_key, ikm, sizeof(ikm), a1, a2);

    hash b1{}, b2{}, b3{};
    hkdf3(chaining_key, ikm, sizeof(ikm), b1, b2, b3);

    CHECK(a1 == b1);
    CHECK(a2 == b2);
    // b3 is chained off b2, not independently derived - just confirm it's
    // present and distinct, not that it equals some other formula.
    CHECK(b3 != b2);
    CHECK(b3 != b1);
}

TEST_CASE("mac16 is deterministic, key-dependent, and constant_time_eq matches sodium_memcmp semantics") {
    hash key1{}, key2{};
    random_bytes(key1.data(), key1.size());
    random_bytes(key2.data(), key2.size());
    const uint8_t msg[] = "mac me";

    auto mac1 = mac16(key1, msg, sizeof(msg));
    auto mac1_again = mac16(key1, msg, sizeof(msg));
    auto mac2 = mac16(key2, msg, sizeof(msg));

    CHECK(constant_time_eq(mac1.data(), mac1_again.data(), mac_len));
    CHECK_FALSE(constant_time_eq(mac1.data(), mac2.data(), mac_len));
}
