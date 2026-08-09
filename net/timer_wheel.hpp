#pragma once

// Batched retransmit timing: one timer wheel instead of one timer per
// in-flight packet. A per-packet timerfd/IORING_OP_TIMEOUT would mean N
// kernel timer registrations for N outstanding fragments - exactly the
// per-item overhead this whole project has been avoiding at every other
// layer (one io_uring SQE per packet was already the thing multishot recv
// eliminated on the receive side; per-packet timers would reintroduce the
// same problem on the retransmit side). Instead: one coarse-grained wheel,
// advanced by a single periodic tick from the event loop, and every
// fragment that happens to expire in the same tick is returned together -
// batched, the same way SQ submission and CQ draining are batched
// everywhere else in this codebase.
//
// Cancellation is lazy: cancel() doesn't search the wheel for the token
// (that would mean scanning a slot's whole entry list), it just records the
// token as cancelled; advance() checks that set when a slot's entries come
// due and silently drops any that were cancelled. This is O(1) for both
// schedule and cancel, at the cost of a cancelled-but-never-fired token
// sitting in a set until its original slot is visited (bounded by the
// wheel's own tick rate, not by how long the token was cancelled).
//
// slot_count is a template parameter (not a runtime constructor argument): every
// real caller in this codebase uses the same fixed slot count, so the
// power-of-two requirement (slot index is `elapsed_ticks & (slot_count-1)`, not
// a modulo) is enforced by static_assert at compile time instead of a
// runtime throw - an invalid slot count is now a build failure, not a
// startup-time exception. `slots` itself becomes a fixed-size std::array
// instead of a heap-allocated std::vector as a direct consequence (no
// allocation at all for the wheel's own storage).

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace reliability {

using timer_token = uint64_t;

// The `requires` clause (not a static_assert in the body) means a bad
// slot_count is a substitution failure at the point of use - the compiler
// error points straight at the offending `timer_wheel<100>`, rather than
// at an assert buried inside the class template's own definition.
template <size_t slot_count>
    requires (std::has_single_bit(slot_count))
class timer_wheel {
public:

    // `tick` is the wheel's time granularity; scheduling a delay longer
    // than `slot_count * tick` is not supported (see schedule()'s contract).
    explicit timer_wheel(std::chrono::steady_clock::duration tick) : tick(tick) {
        if (tick.count() <= 0) throw std::invalid_argument("timer_wheel: tick must be positive");
    }

    // Schedules `token` to be returned by a future advance() call
    // approximately `delay` from now (rounded up to the nearest tick).
    // Throws std::invalid_argument if delay exceeds the wheel's total span
    // (slot_count * tick) - that's a caller sizing bug, not a runtime condition
    // to silently clamp.
    void schedule(timer_token token, std::chrono::steady_clock::duration delay) {
        int64_t tick_count = tick.count();
        int64_t delay_count = delay.count();
        int64_t ticks_ahead = (delay_count + tick_count - 1) / tick_count; // ceiling division
        if (ticks_ahead < 1) ticks_ahead = 1; // always land in a strictly future slot, never the current one

        if ((size_t)ticks_ahead >= slot_count) throw std::invalid_argument("timer_wheel: delay exceeds the wheel's span (slot_count * tick)");

        size_t target = (current_index + (size_t)ticks_ahead) & (slot_count - 1);
        slots[target].entries.push_back(token);
    }

    // No-op if `token` isn't currently pending (already fired, already
    // cancelled, or never scheduled).
    void cancel(timer_token token) { cancelled.insert(token); }

    // Advances the wheel by however many whole ticks have elapsed since the
    // last call (or since construction, for the first call), and returns
    // every non-cancelled token whose slot came due during that advance -
    // possibly from several ticks at once if advance() is called less often
    // than `tick`, possibly empty if called more often.
    std::vector<timer_token> advance(std::chrono::steady_clock::time_point now) {
        if (!started) {
            last_tick_time = now;
            started = true;
            return {};
        }

        auto elapsed = now - last_tick_time;
        uint64_t ticks = elapsed.count() > 0 ? (uint64_t)(elapsed / tick) : 0;
        if (ticks == 0) return {};
        last_tick_time += ticks * tick; // advance by exact tick multiples; leftover remainder carries to next call

        size_t steps = ticks < slot_count ? (size_t)ticks : slot_count; // more than a full lap is redundant work

        std::vector<timer_token> fired;
        for (size_t i = 0; i < steps; ++i) {
            current_index = (current_index + 1) & (slot_count - 1);
            for (timer_token t : slots[current_index].entries) {
                auto it = cancelled.find(t);
                if (it != cancelled.end()) {
                    cancelled.erase(it);
                } else {
                    fired.push_back(t);
                }
            }
            slots[current_index].entries.clear();
        }

        if (ticks >= slot_count) {
            // A full lap (steps == slot_count) returns current_index to where it started; the true final
            // position after `ticks` (which may be far larger than slot_count, e.g. after a long pause) is
            // this starting point plus the remainder - direct modulo arithmetic instead of looping
            // `ticks` times, since every slot beyond the first full lap was already cleared above.
            current_index = (current_index + (size_t)(ticks % slot_count)) & (slot_count - 1);
        }

        return fired;
    }

private:
    struct slot {
        std::vector<timer_token> entries;
    };

    std::array<slot, slot_count> slots{};
    std::unordered_set<timer_token> cancelled;
    size_t current_index = 0;
    std::chrono::steady_clock::time_point last_tick_time;
    std::chrono::steady_clock::duration tick;
    bool started = false;
};

} // namespace reliability
