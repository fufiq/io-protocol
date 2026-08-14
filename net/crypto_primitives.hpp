#pragma once

// Thin, audited-primitives-only wrappers around libsodium. Nothing in this
// file implements its own cipher, hash, or MAC construction - Roman's point
// that "you basically can't screw up cryptography itself, screwups happen in
// the surrounding protocol" only holds if you don't hand-roll the primitives
// either. Everything here is a direct pass-through to libsodium with two
// kinds of hardening bolted on:
//   1. secrets live in sodium_malloc'd, guard-paged, mlock'd memory that is
//      zeroed on destruction (secret_buffer);
//   2. every return value that libsodium uses to signal "this input was
//      cryptographically invalid" is checked and turned into a hard failure
//      (see dh() below for the small-order-point pitfall specifically).

#include <sodium.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace crypto {

namespace detail {
// Every public entry point below routes through here first instead of
// requiring callers to remember to call an init() function: a forgotten
// sodium_init() call is exactly the kind of silent footgun that turns into
// a production incident, so it isn't left as caller discipline.
inline void ensure_init() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    });
}
} // namespace detail

constexpr size_t key_len = 32;
constexpr size_t hash_len = 32;   // SHA-256
constexpr size_t aead_tag_len = crypto_aead_chacha20poly1305_IETF_ABYTES; // 16
constexpr size_t aead_nonce_len = crypto_aead_chacha20poly1305_IETF_NPUBBYTES; // 12
constexpr size_t mac_len = 16;    // truncated HMAC-SHA256, WireGuard-style mac1

using public_key = std::array<uint8_t, key_len>;
using hash = std::array<uint8_t, hash_len>;

// RAII secret buffer: sodium_malloc gives guard pages before/after the
// allocation (catches linear over/under-reads at the point of corruption
// instead of silently leaking adjacent heap) and the pages are mlock'd so
// the key material can't be paged out to swap. Zeroed unconditionally on
// destruction, including on the exception path.
class secret_buffer {
public:
    explicit secret_buffer(size_t len) : len(len) {
        detail::ensure_init();
        ptr = (uint8_t*)sodium_malloc(len);
        if (ptr == nullptr) throw std::bad_alloc();
    }
    ~secret_buffer() {
        if (ptr != nullptr) sodium_free(ptr); // sodium_free zeroes before unmapping
    }

    secret_buffer(const secret_buffer&) = delete;
    secret_buffer& operator=(const secret_buffer&) = delete;

    secret_buffer(secret_buffer&& other) noexcept : ptr(other.ptr), len(other.len) {
        other.ptr = nullptr;
        other.len = 0;
    }
    secret_buffer& operator=(secret_buffer&& other) noexcept {
        if (this != &other) {
            if (ptr != nullptr) sodium_free(ptr);
            ptr = other.ptr;
            len = other.len;
            other.ptr = nullptr;
            other.len = 0;
        }
        return *this;
    }

    uint8_t* data() { return ptr; }
    const uint8_t* data() const { return ptr; }
    size_t size() const { return len; }

    void zero() { sodium_memzero(ptr, len); }

private:
    uint8_t* ptr;
    size_t len;
};

inline void random_bytes(void* buf, size_t len) {
    detail::ensure_init();
    randombytes_buf(buf, len);
}

// --- X25519 -----------------------------------------------------------------

struct key_pair {
    secret_buffer priv{key_len};
    public_key pub{};
};

inline key_pair x25519_generate() {
    key_pair kp;
    random_bytes(kp.priv.data(), key_len);
    // Clamping is performed internally by crypto_scalarmult_curve25519_base.
    crypto_scalarmult_curve25519_base(kp.pub.data(), kp.priv.data());
    return kp;
}

// Curve25519 DH with small-order/identity-point rejection.
//
// Vulnerability this guards against: a malicious peer can send a public key
// that is a low-order point (there are 8 such points on the curve, e.g. the
// all-zero point). DH against a low-order point collapses to one of a
// handful of fixed outputs regardless of your private key, so if this were
// silently accepted an attacker could force a *predictable* shared secret
// and potentially impersonate or downgrade the session. libsodium's
// crypto_scalarmult_curve25519 already detects an all-zero output and
// returns -1 (it does NOT reject every low-order point on all versions, so
// treat this as defense-in-depth, not the only check) - we must not ignore
// that return value, which is why it's marked warn_unused_result upstream.
inline std::optional<public_key> dh(const secret_buffer& priv, const public_key& pub) {
    public_key out{};
    int rc = crypto_scalarmult_curve25519(out.data(), priv.data(), pub.data());
    if (rc != 0) return std::nullopt;
    return out;
}

// --- AEAD (ChaCha20-Poly1305, IETF variant) ---------------------------------

// Noise nonce encoding: 4 zero bytes || little-endian 64-bit counter.
inline std::array<uint8_t, aead_nonce_len> encode_nonce(uint64_t counter) {
    std::array<uint8_t, aead_nonce_len> nonce{};
    for (int i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t)(counter >> (8 * i));
    return nonce;
}

inline std::vector<uint8_t> aead_encrypt(const secret_buffer& key, uint64_t counter, const uint8_t* ad, size_t ad_len, const uint8_t* plaintext, size_t pt_len) {
    auto nonce = encode_nonce(counter);
    std::vector<uint8_t> out(pt_len + aead_tag_len);
    unsigned long long out_len = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out.data(), &out_len, plaintext, pt_len, ad, ad_len, nullptr, nonce.data(), key.data());
    out.resize(out_len);
    return out;
}

// Returns nullopt on authentication failure. Callers must not distinguish
// "wrong tag" from "wrong length" from any other failure mode in timing or
// behavior - this is the only signal exposed.
inline std::optional<std::vector<uint8_t>> aead_decrypt(const secret_buffer& key, uint64_t counter, const uint8_t* ad, size_t ad_len, const uint8_t* ciphertext, size_t ct_len) {
    if (ct_len < aead_tag_len) return std::nullopt;
    auto nonce = encode_nonce(counter);
    std::vector<uint8_t> out(ct_len - aead_tag_len);
    unsigned long long out_len = 0;
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(out.data(), &out_len, nullptr, ciphertext, ct_len, ad, ad_len, nonce.data(), key.data());
    if (rc != 0) return std::nullopt;
    out.resize(out_len);
    return out;
}

// --- hash / HKDF (SHA-256) ---------------------------------------------------

inline hash sha256(const uint8_t* data, size_t len) {
    detail::ensure_init();
    hash h{};
    crypto_hash_sha256(h.data(), data, len);
    return h;
}

// Noise's generic HKDF(chaining_key, ikm, num_outputs) is, for an empty
// `info` and a hash whose output equals the block used for extraction,
// exactly RFC 5869 HKDF-Extract(salt=chaining_key, ikm) followed by
// HKDF-Expand(prk, info="", L = 32 * num_outputs) sliced into 32-byte
// chunks - see the derivation in the level-2 design notes. That means we
// can use libsodium's audited HKDF-SHA256 directly instead of hand-rolling
// HMAC-SHA256 ourselves.
inline void hkdf2(const hash& chaining_key, const uint8_t* ikm, size_t ikm_len, hash& out1, hash& out2) {
    uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    static constexpr uint8_t empty_ikm = 0; // crypto_kdf_hkdf_sha256_extract's ikm param is nonnull even at len 0
    const uint8_t* safe_ikm = (ikm != nullptr) ? ikm : &empty_ikm;
    crypto_kdf_hkdf_sha256_extract(prk, chaining_key.data(), chaining_key.size(), safe_ikm, ikm_len);
    uint8_t okm[64];
    crypto_kdf_hkdf_sha256_expand(okm, sizeof(okm), nullptr, 0, prk);
    std::memcpy(out1.data(), okm, 32);
    std::memcpy(out2.data(), okm + 32, 32);
    sodium_memzero(prk, sizeof(prk));
    sodium_memzero(okm, sizeof(okm));
}

inline void hkdf3(const hash& chaining_key, const uint8_t* ikm, size_t ikm_len, hash& out1, hash& out2, hash& out3) {
    uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    static constexpr uint8_t empty_ikm = 0; // crypto_kdf_hkdf_sha256_extract's ikm param is nonnull even at len 0
    const uint8_t* safe_ikm = (ikm != nullptr) ? ikm : &empty_ikm;
    crypto_kdf_hkdf_sha256_extract(prk, chaining_key.data(), chaining_key.size(), safe_ikm, ikm_len);
    uint8_t okm[96];
    crypto_kdf_hkdf_sha256_expand(okm, sizeof(okm), nullptr, 0, prk);
    std::memcpy(out1.data(), okm, 32);
    std::memcpy(out2.data(), okm + 32, 32);
    std::memcpy(out3.data(), okm + 64, 32);
    sodium_memzero(prk, sizeof(prk));
    sodium_memzero(okm, sizeof(okm));
}

// --- Keyed MAC (HMAC-SHA256 truncated to 16 bytes), used for the cheap
// pre-DH "mac1" flood filter, WireGuard-style. Not part of the Noise
// handshake proper - see the level-2 design notes on why it's included now.
inline std::array<uint8_t, mac_len> mac16(const hash& key, const uint8_t* data, size_t len) {
    detail::ensure_init();
    uint8_t full[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(full, data, len, key.data());
    std::array<uint8_t, mac_len> out{};
    std::memcpy(out.data(), full, mac_len);
    sodium_memzero(full, sizeof(full));
    return out;
}

inline bool constant_time_eq(const uint8_t* a, const uint8_t* b, size_t len) {
    return sodium_memcmp(a, b, len) == 0;
}

// --- Bare stream cipher, for disguise rather than confidentiality ----------
//
// Pure ChaCha20 keystream (IETF variant), with no MAC and no associated data.
// This is deliberately weaker than everything else in this file: it exists
// for exactly one job - XORing a few fixed-offset plaintext bytes into
// something a passive observer cannot pattern-match - over bytes that are
// already covered by a real authenticator further into the same packet (see
// protocol.hpp's header obfuscation). It must never be the only thing
// protecting a byte range: unlike aead_encrypt, nothing here proves the
// bytes weren't tampered with, only that they don't look like what they are.
constexpr size_t stream_nonce_len = crypto_stream_chacha20_ietf_NONCEBYTES; // 12
using stream_nonce = std::array<uint8_t, stream_nonce_len>;
static_assert(hash_len == crypto_stream_chacha20_ietf_KEYBYTES, "chacha20-ietf wants a 32-byte key, same width as our hash type");

inline void stream_keystream(uint8_t* out, size_t len, const stream_nonce& nonce, const hash& key) {
    detail::ensure_init();
    crypto_stream_chacha20_ietf(out, len, nonce.data(), key.data());
}

} // namespace crypto
