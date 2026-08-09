#pragma once

// Sliding-window anti-replay bitmap (RFC 6479 / WireGuard style): tracks
// which of the last window_bits nonces have been seen for one connection,
// so a byte-identical replay of a previously-accepted packet is rejected
// instead of decrypting and reassembling again - closing the gap
// transport::connection documented and left open (see
// transport_connection.hpp's file header and the "KNOWN GAP" test this
// class makes obsolete).
//
// Security-critical ordering: check() must be called BEFORE AEAD
// decryption, and commit() must be called ONLY after that decryption
// succeeds. If commit() ran on unauthenticated nonces, an attacker could
// send a packet with a garbage ciphertext but a huge nonce to force the
// window forward arbitrarily, pushing every legitimate in-flight packet's
// nonce out of the window - a state-poisoning DoS that doesn't need to
// forge anything. Because commit only happens post-auth, the worst a
// forged packet can do is get rejected for free; it can never move the
// window.
//
// check()/commit() are split (rather than one "try to accept" call) for
// exactly that reason: the caller must decrypt in between.

#include <array>
#include <cstddef>
#include <cstdint>

namespace replay {

constexpr size_t window_bits = 2048;
constexpr size_t window_words = window_bits / 64;

namespace detail {
// Shifts a multi-word bitmap "left" by n bit-positions, where position 0 is
// the least-significant bit of bitmap[0] (word index and bit index both
// increase with "age" - see the class comment). This is a standard
// bignum shift-left, needed because commit() must move every previously
// recorded position by however far the window advanced when a new highest
// nonce arrives.
inline void shift_left(std::array<uint64_t, window_words>& words, size_t n) {
    if (n == 0) return;
    if (n >= window_words * 64) {
        words.fill(0);
        return;
    }

    size_t word_shift = n / 64;
    size_t bit_shift = n % 64;

    if (word_shift > 0) {
        for (size_t i = window_words; i-- > word_shift;) words[i] = words[i - word_shift];
        for (size_t i = 0; i < word_shift; ++i) words[i] = 0;
    }

    if (bit_shift > 0) {
        for (size_t i = window_words; i-- > 1;) words[i] = (words[i] << bit_shift) | (words[i - 1] >> (64 - bit_shift));
        words[0] <<= bit_shift;
    }
}
} // namespace detail

class window {
public:
    // Cheap pre-check, no state mutation: true means "not a definite
    // replay/too-old packet" - go ahead and attempt decryption. This does
    // NOT mean the packet is authentic, only that it's still worth spending
    // an AEAD verification on.
    bool check(uint64_t nonce) const {
        if (!initialized) return true;
        if (nonce > highest) return true;

        uint64_t diff = highest - nonce;
        if (diff >= window_bits) return false; // too old: fell out of the window

        size_t word = (size_t)(diff / 64);
        size_t bit = (size_t)(diff % 64);
        return (bitmap[word] & (1ull << bit)) == 0;
    }

    // Call only after the packet this nonce came from was successfully
    // authenticated. Advances the window if `nonce` is a new high-water
    // mark, otherwise just marks this position as seen.
    void commit(uint64_t nonce) {
        if (!initialized) {
            highest = nonce;
            bitmap.fill(0);
            bitmap[0] = 1;
            initialized = true;
            return;
        }

        if (nonce > highest) {
            uint64_t shift = nonce - highest;
            detail::shift_left(bitmap, (size_t)(shift > window_bits ? window_bits : shift));
            highest = nonce;
            bitmap[0] |= 1;
        } else {
            uint64_t diff = highest - nonce;
            size_t word = (size_t)(diff / 64);
            size_t bit = (size_t)(diff % 64);
            bitmap[word] |= (1ull << bit);
        }
    }

private:
    bool initialized = false;
    uint64_t highest = 0;
    std::array<uint64_t, window_words> bitmap{};
};

} // namespace replay
