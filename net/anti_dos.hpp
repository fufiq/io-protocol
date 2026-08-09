#pragma once

// Level 5, second piece: a stateless cookie (WireGuard's "mac2") to stop a
// handshake flood from costing the responder a DH per garbage packet, plus
// a crude load detector that decides when to start requiring it.
//
// What mac1 (level 2, noise_ik.hpp) already buys you: rejecting packets
// from a sender who doesn't know the responder's static public key, for the
// cost of one HMAC. It does NOT stop a flood from someone who *does* know
// that key (it's not secret - assume it leaks or is simply public
// knowledge for a public-facing service) from forcing a full DH computation
// on every packet they send, and it does NOT stop a source-spoofed flood:
// mac1 is deterministic over content the attacker already controls, so
// spoofing costs them nothing either.
//
// The cookie closes both gaps without the responder keeping any per-source
// state (which would itself be a memory-exhaustion vector under a
// distributed flood): a cookie is HMAC(rotating_secret, source_address),
// verified by recomputing it, never stored. Under load, the responder
// replies to a message1 that lacks a valid cookie with a cheap "here is
// your cookie" reply instead of doing the DH; a legitimate initiator (one
// that can actually receive traffic at its claimed source address) gets
// the cookie and retries message1 with it attached. An attacker spoofing
// someone else's address never sees the cookie reply, so can never
// complete the round trip needed to make the responder do real work - this
// is the actual amplification defense, not a separate mechanism from the
// cookie itself.
//
// The secret rotates periodically (default 2 minutes) so a cookie's
// validity window is bounded; `verify` accepts the current OR immediately
// previous secret so a cookie issued just before a rotation isn't
// invalidated mid-handshake-retry.

#include "crypto_primitives.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace anti_dos {

constexpr size_t cookie_len = 16;
using cookie = std::array<uint8_t, cookie_len>;

// Client-side counterpart of cookie_secret::verify_mac2: once an initiator
// has received a cookie (via a cookie-reply packet), it computes mac2 the
// same way the responder will check it - keyed by (a hash of) the cookie,
// over this specific retry's message1 bytes.
inline cookie compute_mac2(const cookie& cookie_value, const uint8_t* msg, size_t msg_len) {
    auto key = crypto::sha256(cookie_value.data(), cookie_value.size());
    auto mac = crypto::mac16(key, msg, msg_len);
    cookie out;
    std::copy(mac.begin(), mac.end(), out.begin());
    return out;
}

class cookie_secret {
public:
    explicit cookie_secret(std::chrono::steady_clock::duration lifetime = std::chrono::minutes(2)) : last_rotation(std::chrono::steady_clock::now()), lifetime(lifetime) {
        crypto::random_bytes(current.data(), current.size());
    }

    // Call periodically (e.g. once per event-loop iteration) with the
    // current time; rotates the secret if `lifetime` has elapsed since the
    // last rotation. Cheap to call when no rotation is due (one comparison).
    void maybe_rotate(std::chrono::steady_clock::time_point now) {
        if (now - last_rotation < lifetime) return;
        previous = current;
        has_previous = true;
        crypto::random_bytes(current.data(), current.size());
        last_rotation = now;
    }

    cookie compute(const uint8_t* source_addr, size_t addr_len) const { return compute_with(current, source_addr, addr_len); }

    bool verify(const uint8_t* source_addr, size_t addr_len, const cookie& cookie_value) const {
        auto expected = compute_with(current, source_addr, addr_len);
        if (crypto::constant_time_eq(expected.data(), cookie_value.data(), cookie_len)) return true;
        if (has_previous) {
            auto expected_prev = compute_with(previous, source_addr, addr_len);
            if (crypto::constant_time_eq(expected_prev.data(), cookie_value.data(), cookie_len)) return true;
        }
        return false;
    }

    // "mac2": a MAC keyed by (a hash of) the cookie, computed over the
    // message bytes it accompanies (everything up to and including mac1).
    // Deliberately not just "resend the cookie verbatim" - binding it to
    // the specific message content means an observer who sees one
    // cookie-bearing packet can't lift that cookie and staple it onto a
    // *different* forged message1 for the same source address. Verifies
    // against both the current and previous secret's derived cookie, same
    // grace-period reasoning as verify().
    bool verify_mac2(const uint8_t* source_addr, size_t addr_len, const uint8_t* msg, size_t msg_len, const cookie& received_mac2) const {
        auto candidate = compute_with(current, source_addr, addr_len);
        if (crypto::constant_time_eq(compute_mac2(candidate, msg, msg_len).data(), received_mac2.data(), cookie_len)) return true;
        if (has_previous) {
            auto candidate_prev = compute_with(previous, source_addr, addr_len);
            if (crypto::constant_time_eq(compute_mac2(candidate_prev, msg, msg_len).data(), received_mac2.data(), cookie_len)) return true;
        }
        return false;
    }

private:
    cookie compute_with(const crypto::hash& secret, const uint8_t* source_addr, size_t addr_len) const {
        auto mac = crypto::mac16(secret, source_addr, addr_len);
        cookie out;
        std::copy(mac.begin(), mac.end(), out.begin());
        return out;
    }

    crypto::hash current{};
    crypto::hash previous{};
    bool has_previous = false;
    std::chrono::steady_clock::time_point last_rotation;
    std::chrono::steady_clock::duration lifetime;
};

// Crude, deliberately global (not per-source) rate detector: how many
// handshake *attempts* (mac1-valid packets, i.e. already past the cheapest
// filter) have arrived in roughly the last second. Global rather than
// per-source on purpose - per-source counters are themselves a
// memory-exhaustion vector when the flood comes from many distinct spoofed
// or real sources, which defeats the point.
class load_gate {
public:
    explicit load_gate(uint32_t threshold_per_sec) : threshold(threshold_per_sec) {}

    void record_attempt(std::chrono::steady_clock::time_point now) {
        if (!initialized || now - bucket_start >= std::chrono::seconds(1)) {
            last_rate = bucket_count;
            bucket_start = now;
            bucket_count = 1;
            initialized = true;
        } else {
            ++bucket_count;
        }
    }

    bool under_load(std::chrono::steady_clock::time_point now) const {
        if (!initialized) return false;
        if (now - bucket_start >= std::chrono::seconds(2)) return false; // no recent attempts at all
        return bucket_count >= threshold || last_rate >= threshold;
    }

private:
    uint32_t threshold;
    bool initialized = false;
    std::chrono::steady_clock::time_point bucket_start{};
    uint32_t bucket_count = 0;
    uint32_t last_rate = 0;
};

} // namespace anti_dos
