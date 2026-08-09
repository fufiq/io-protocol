#pragma once

// Level 6: optional reliability on top of transport::connection. Deliberately
// NOT baked into connection itself - not all traffic needs delivery
// guarantees (a lot of it is fine fire-and-forget, as every earlier
// benchmark in this codebase already demonstrated), so this is an opt-in
// wrapper an application reaches for per-message, not a tax on everything.
//
// Two pieces, reusing what earlier levels already built rather than
// duplicating it:
//   - Retransmit timing goes through timer_wheel (timer_wheel.hpp) - one
//     coarse-grained wheel, not one timer per in-flight fragment.
//   - Acknowledgment reuses packet_assembly::reassembler's own bitmask
//     (via drain_dirty_acks()) instead of the receiver tracking a second,
//     separate "what have I gotten" bitmap: it already has one.
//
// Third piece: a congestion window, deliberately without RTT measurement.
// Classic TCP Reno-style AIMD doesn't actually need RTT for the window
// logic itself (RTT only feeds the retransmit *timeout*, which here is
// still just reliable_sender's fixed initial_timeout + backoff - a
// reasonable thing to make adaptive later, but a separate concern). The
// signals this uses are ones the class already has for free:
//   - an ACK arriving is "grow" (slow start: +1 per ack; congestion
//     avoidance past slow_start_threshold: +congestion_avoidance_step per
//     ~window's worth of acks - plain Reno's +1 per round trip was
//     measured too slow to reach a real link's actual capacity within any
//     reasonable test duration on a real WAN path: congestion_window was
//     still climbing steadily, with no real loss, when a 60-second run
//     ended. Bumping the per-round step preserves the same reactive
//     halve-on-loss behavior (so it still backs off the moment a genuine
//     retransmit fires) while climbing faster toward whatever ceiling the
//     link can actually sustain, instead of the abrupt "start way higher"
//     alternative (raising slow_start_threshold) that was tried first and
//     produced repeated early collapses from bursting into real loss
//     before the window had any evidence the link could sustain it.
//   - a retransmit actually firing is "loss" (halve: slow_start_threshold =
//     congestion_window/2, congestion_window = slow_start_threshold)
// can_send() is a soft/approximate gate, checked once per queued *message*
// before fragmenting it, not per fragment: a single large message can
// still push `pending` somewhat past `congestion_window` in one shot (its
// fragments are all-or-nothing, matching encrypt_message's own atomicity),
// and the next queued message just waits until pending drops back under
// the window.

#include "packet_assembly.hpp"
#include "timer_wheel.hpp"
#include "transport_connection.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace reliability {

constexpr size_t sack_wire_len = 4 + 2 + 8; // msg_id + frag_count + received_mask, all little-endian

// Wire encoding for one SACK entry. A real protocol would batch several of
// these per packet (one per dirty reassembler slot since the last SACK);
// that batching is the caller's job (see level6_reliability_server.cpp) -
// this is just the fixed-size per-entry codec.
constexpr std::vector<uint8_t> encode_sack(const assembly::ack_info& info) {
    std::vector<uint8_t> out(sack_wire_len);
    out[0] = (uint8_t)info.msg_id;
    out[1] = (uint8_t)(info.msg_id >> 8);
    out[2] = (uint8_t)(info.msg_id >> 16);
    out[3] = (uint8_t)(info.msg_id >> 24);
    out[4] = (uint8_t)info.frag_count;
    out[5] = (uint8_t)(info.frag_count >> 8);
    for (int i = 0; i < 8; ++i) out[6 + i] = (uint8_t)(info.received_mask >> (8 * i));
    return out;
}

constexpr assembly::ack_info decode_sack(const uint8_t* data) {
    assembly::ack_info info{};
    info.msg_id = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    info.frag_count = (uint16_t)(data[4] | (data[5] << 8));
    uint64_t mask = 0;
    for (int i = 0; i < 8; ++i) mask |= (uint64_t)data[6 + i] << (8 * i);
    info.received_mask = mask;

    // frag_count comes straight off the wire, and SACKs are the one packet
    // type that is NOT authenticated (see protocol.hpp's tick(): type 5 goes
    // out as plaintext), so this is attacker-chosen input from anyone who can
    // guess a live conn_id. A received_mask is 64 bits, so a count above
    // assembly::max_fragments cannot describe anything real - and shifting by
    // it is undefined behavior, which is exactly what a fuzzer found here
    // ("shift exponent 53456 is too large for 64-bit type"). Treat an
    // impossible count as a malformed SACK: zero it, which makes `completed`
    // false and makes handle_ack's loop below a no-op.
    if (info.frag_count > assembly::max_fragments) {
        info.frag_count = 0;
        info.completed = false;
        return info;
    }
    info.completed = (info.frag_count > 0) &&
                     (mask == ((info.frag_count == 64) ? ~0ull : ((1ull << info.frag_count) - 1)));
    return info;
}

// Templated on the same window_capacity as the transport::connection it
// wraps (must match, since it holds a reference to one) - see
// transport_connection.hpp and packet_assembly.hpp for why that's a
// compile-time parameter rather than a runtime one now.
template <size_t window_capacity = 4096>
class reliable_sender {
public:
    // initial_cwnd/initial_ssthresh default to the same conservative values
    // this class has always started at (see the class-level comment on why
    // that's the safe default for a link of unknown capacity). Raising them
    // is a deliberate escape hatch for a caller who already knows this
    // specific link's real capacity from prior measurement (e.g. a bare
    // io_uring baseline on the same path) and wants to skip most of the
    // slow-start ramp rather than re-discover that capacity from scratch
    // every connection - it is not safe to do blindly for a link whose
    // capacity is unknown, since guessing too high risks the same early
    // collapse raising ssthresh globally did in testing.
    // wheel_slots isn't a parameter here at all: every real caller uses the
    // same 512-slot wheel (see wheel_slots_default below), so that size is a
    // template argument on timer_wheel now, checked by static_assert instead
    // of a runtime throw.
    explicit reliable_sender(transport::connection<window_capacity>& conn, std::chrono::steady_clock::duration tick = std::chrono::milliseconds(20),
                             std::chrono::steady_clock::duration initial_timeout = std::chrono::milliseconds(200), uint32_t max_retries = 5,
                             size_t initial_cwnd = initial_cwnd_default, size_t initial_ssthresh = initial_ssthresh_default)
        : conn(conn), wheel(tick), initial_timeout(initial_timeout), max_retries(max_retries), congestion_window(initial_cwnd), slow_start_threshold(initial_ssthresh) {}

    // Encrypts+fragments via the wrapped connection and additionally tracks
    // every resulting wire packet for retransmit until it's acked. Returns
    // the same wire packets encrypt_message would have (send them exactly
    // as you would for an unreliable message).
    std::vector<std::vector<uint8_t>> send_message(const uint8_t* data, size_t len) {
        auto plaintext_fragments = conn.fragment_for_send(data, len);

        std::vector<std::vector<uint8_t>> out;
        out.reserve(plaintext_fragments.size());
        for (auto& frag : plaintext_fragments) {
            auto hdr = assembly::decode_header(frag.data());
            const uint8_t* payload = frag.data() + assembly::fragment_header_len;
            size_t payload_len = frag.size() - assembly::fragment_header_len;

            auto wire = conn.encrypt_fragment(hdr, payload, payload_len);

            auto token = make_token(hdr.msg_id, hdr.frag_index);
            pending[token] = pending_fragment{hdr, std::vector<uint8_t>(payload, payload + payload_len), 0};
            wheel.schedule(token, initial_timeout);

            out.push_back(std::move(wire));
        }
        return out;
    }

    // Feeds a SACK received from the peer: cancels retransmit tracking for
    // every fragment bit set in `received_mask`.
    void handle_ack(const assembly::ack_info& info) {
        bool any_newly_acked = false;
        // Bounded independently of decode_sack's own check: this is a public
        // entry point taking a caller-supplied ack_info, so it must not
        // depend on someone else having validated frag_count first. Past 64
        // the shift below is undefined behavior, and the loop would also
        // spin up to 65535 times doing map lookups on a packet nobody
        // authenticated.
        const uint16_t count = info.frag_count > assembly::max_fragments ? assembly::max_fragments : info.frag_count;
        for (uint16_t i = 0; i < count; ++i) {
            if (!(info.received_mask & (1ull << i))) continue;
            auto token = make_token(info.msg_id, i);
            wheel.cancel(token);
            if (pending.erase(token) > 0) any_newly_acked = true;
        }

        if (!any_newly_acked) return;
        // AIMD growth: slow start below slow_start_threshold (fast, +1 per
        // ack - roughly doubles per round trip), congestion avoidance above
        // it (+congestion_avoidance_step per ~window's worth of acks -
        // roughly +congestion_avoidance_step per round trip instead).
        if (congestion_window < slow_start_threshold) {
            ++congestion_window;
        } else if (++acks_since_growth >= congestion_window) {
            congestion_window += congestion_avoidance_step;
            acks_since_growth = 0;
        }
    }

    // Call once per event-loop iteration: advances the timer wheel and
    // returns wire-ready packets (already-encrypted, no new AEAD work) that
    // need resending. A fragment that has exhausted max_retries is dropped
    // from tracking (counted in gave_up_count()) rather than retried
    // forever - the application decides what "gave up" means for it (drop
    // the peer, surface an error, whatever), this class just stops paying
    // for a lost cause.
    std::vector<std::vector<uint8_t>> collect_retransmits(std::chrono::steady_clock::time_point now) {
        auto expired = wheel.advance(now);
        std::vector<std::vector<uint8_t>> out;

        // At most one multiplicative decrease per cooldown window, not one per
        // expired token and not merely one per call. A single burst that
        // overwhelms the link can time out dozens of fragments spread across
        // several consecutive ticks (they were sent close together but their
        // deadlines can straddle a tick boundary) - guarding only "this call"
        // isn't enough on its own: each of those ticks would independently
        // re-halve congestion_window, cascading a single congestion event
        // into a much deeper collapse than one halving (observed:
        // congestion_window falling to ~1/4 instead of ~1/2). Suppressing
        // further decreases until initial_timeout has passed since the last
        // one - see the member comment on decrease_cooldown_until - treats
        // the whole burst as one event, still without needing a measured RTT.
        bool decreased_this_round = false;

        for (auto token : expired) {
            auto it = pending.find(token);
            if (it == pending.end()) continue; // acked between being scheduled and firing

            if (it->second.retries >= max_retries) {
                pending.erase(it);
                ++gave_up;
                continue;
            }

            // A fragment actually timing out is this class's only loss signal
            // (no RTT/delay-based detection) - treat it as congestion the same
            // way TCP Reno treats a retransmit timeout: multiplicative decrease.
            // Only on the *first* retry for this fragment, so one message's
            // several lost fragments don't each independently re-halve the
            // window on the same round of loss.
            if (it->second.retries == 0 && !decreased_this_round && now >= decrease_cooldown_until) {
                slow_start_threshold = std::max<size_t>(2, congestion_window / 2);
                congestion_window = slow_start_threshold;
                acks_since_growth = 0;
                decreased_this_round = true;
                decrease_cooldown_until = now + initial_timeout;
            }

            ++it->second.retries;
            // Fresh nonce every retry - never resend the original ciphertext.
            // See transport_connection.hpp's encrypt_fragment comment: a
            // byte-identical retransmit is exactly what the replay window (level
            // 5) exists to reject, so a retry must look like a brand-new packet
            // on the wire even though header/msg_id/frag_index are unchanged.
            out.push_back(conn.encrypt_fragment(it->second.header, it->second.plaintext.data(), it->second.plaintext.size()));
            auto backoff = initial_timeout * (1u << it->second.retries);
            wheel.schedule(token, backoff);
        }
        return out;
    }

    uint64_t gave_up_count() const { return gave_up; }
    size_t pending_count() const { return pending.size(); }

    // Congestion window gate: true if there's room to send another message
    // without exceeding congestion_window. See the class-level comment for
    // the AIMD rules that grow/shrink it.
    bool can_send() const { return pending.size() < congestion_window; }
    size_t cwnd() const { return congestion_window; }
    size_t ssthresh() const { return slow_start_threshold; }

    // True if any of msg_id's frag_count fragments are still tracked
    // (neither acked nor given up on yet). Lets a caller ask "is this
    // message done" without this class needing to expose per-message
    // bookkeeping beyond the per-fragment tokens it already tracks.
    bool has_pending(uint32_t msg_id, uint16_t frag_count) const {
        for (uint16_t i = 0; i < frag_count; ++i) {
            if (pending.count(make_token(msg_id, i)) > 0) return true;
        }
        return false;
    }

private:
    // Stores the *plaintext* fragment, not a finished wire packet: a
    // retransmission must be re-encrypted through conn.encrypt_fragment()
    // with a fresh nonce each time (see transport_connection.hpp's own
    // comment on encrypt_fragment for why resending the original ciphertext
    // verbatim is a replay-window dead end), so the header+plaintext is
    // what actually needs to survive between send and any later retry.
    struct pending_fragment {
        assembly::fragment_header header;
        std::vector<uint8_t> plaintext;
        uint32_t retries = 0;
    };

    static constexpr timer_token make_token(uint32_t msg_id, uint16_t frag_index) { return ((uint64_t)msg_id << 16) | frag_index; }

    static constexpr size_t wheel_slots_default = 512; // every real caller uses this; see the constructor comment

    transport::connection<window_capacity>& conn;
    timer_wheel<wheel_slots_default> wheel;
    std::chrono::steady_clock::duration initial_timeout;
    uint32_t max_retries;
    std::unordered_map<timer_token, pending_fragment> pending;
    uint64_t gave_up = 0;

    static constexpr size_t initial_cwnd_default = 4;
    static constexpr size_t initial_ssthresh_default = 64;
    static constexpr size_t congestion_avoidance_step = 4;
    size_t congestion_window = initial_cwnd_default;
    size_t slow_start_threshold = initial_ssthresh_default;
    size_t acks_since_growth = 0;

    // A single burst of real loss doesn't time out all its fragments in the
    // same collect_retransmits() tick - they were sent microseconds apart
    // but their retransmit deadlines can straddle a tick boundary, so
    // "first retry" for different fragments from the *same* underlying
    // congestion event can land in several consecutive ticks. Limiting to
    // one decrease per tick (see collect_retransmits) isn't enough on its
    // own: without this cooldown, each of those ticks re-halves
    // congestion_window in turn, so what should be one proportionate cut
    // compounds into a much deeper collapse (observed: a single real event
    // cutting congestion_window to ~1/4 instead of ~1/2). Suppressing
    // further decreases until initial_timeout has passed since the last one
    // - the same duration this class already uses to judge "this fragment
    // is late" - treats everything that times out inside that window as
    // one event, without needing an RTT sample.
    std::chrono::steady_clock::time_point decrease_cooldown_until{};
};

} // namespace reliability
