#pragma once

// Message fragmentation/reassembly for messages that don't fit in one UDP
// datagram, tracked with a sliding window of in-progress messages and a
// per-message bitmask of which fragments have arrived.
//
// Layering: this code trusts every fragment handed to it completely. It
// does NOT authenticate anything - that is level 4's job (per-packet AEAD,
// decrypt-then-hand-up), which must run *before* a fragment ever reaches
// add_fragment(). If you wire this up directly to raw untrusted network
// input without decryption in between, an attacker can trivially force
// arbitrary garbage into "reassembled messages." The defenses built into
// this file (window eviction, byte budget, size caps) only protect against
// resource exhaustion from an authenticated-but-hostile-or-buggy peer, not
// against packet forgery.
//
// Why a sliding window at all: message IDs increase without bound over a
// session's lifetime, but memory doesn't. The window maps msg_id % N to a
// fixed pool of N slots; an incomplete message that's been superseded by
// something `window_size` IDs newer gets evicted, bounding the number of
// concurrently in-progress messages regardless of how many the sender
// starts. The global byte budget is a second, independent bound: even a
// small window can be pointed at maximum-size messages, so total buffered
// bytes across every slot is capped too - see the constructor.
//
// Why a 64-fragment cap: it lets the per-message "which fragments arrived"
// state be one uint64_t (one register, no dynamic bitset), and directly
// bounds a single message's worst-case size to
// max_fragments * max_fragment_payload (~87.5 KiB at the default fragment
// size) regardless of what frag_count value a sender claims.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace assembly {

constexpr uint16_t max_fragments = 64;
constexpr size_t max_fragment_payload = 1400;
constexpr size_t max_message_size = (size_t)max_fragments * max_fragment_payload;
constexpr size_t fragment_header_len = 8;

struct fragment_header {
    uint32_t msg_id = 0;
    uint16_t frag_index = 0;
    uint16_t frag_count = 0;
};

constexpr void encode_header(const fragment_header& hdr, uint8_t* out) {
    // Explicit little-endian field packing rather than memcpy'ing the
    // struct: struct layout (padding, host endianness) isn't a wire format.
    out[0] = (uint8_t)hdr.msg_id;
    out[1] = (uint8_t)(hdr.msg_id >> 8);
    out[2] = (uint8_t)(hdr.msg_id >> 16);
    out[3] = (uint8_t)(hdr.msg_id >> 24);
    out[4] = (uint8_t)hdr.frag_index;
    out[5] = (uint8_t)(hdr.frag_index >> 8);
    out[6] = (uint8_t)hdr.frag_count;
    out[7] = (uint8_t)(hdr.frag_count >> 8);
}

constexpr fragment_header decode_header(const uint8_t* in) {
    fragment_header hdr;
    hdr.msg_id = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    hdr.frag_index = (uint16_t)(in[4] | (in[5] << 8));
    hdr.frag_count = (uint16_t)(in[6] | (in[7] << 8));
    return hdr;
}

// Splits `data` into wire-ready fragments (header + payload each). A
// message that already fits in one fragment still gets the 8-byte header,
// trading a few bytes of overhead for a uniform receive path with no
// special case. Throws if `len` exceeds max_message_size - callers that need
// bigger payloads belong at a higher layer that chunks across multiple
// msg_ids, not by raising this cap (that cap is what bounds a single
// reassembly slot's worst-case memory).
inline std::vector<std::vector<uint8_t>> fragment_message(uint32_t msg_id, const uint8_t* data, size_t len) {
    if (len > max_message_size) throw std::invalid_argument("fragment_message: message exceeds max_message_size");

    size_t frag_count = (len + max_fragment_payload - 1) / max_fragment_payload;
    if (frag_count == 0) frag_count = 1; // an empty message still ships as one zero-length fragment

    std::vector<std::vector<uint8_t>> out;
    out.reserve(frag_count);
    for (size_t i = 0; i < frag_count; ++i) {
        size_t off = i * max_fragment_payload;
        size_t chunk_len = std::min(max_fragment_payload, len - off);

        fragment_header hdr{msg_id, (uint16_t)i, (uint16_t)frag_count};
        std::vector<uint8_t> wire(fragment_header_len + chunk_len);
        encode_header(hdr, wire.data());
        if (chunk_len > 0) std::memcpy(wire.data() + fragment_header_len, data + off, chunk_len);
        out.push_back(std::move(wire));
    }
    return out;
}

// duplicate_of_completed is distinct from rejected: it's not a validation
// failure, it's a legitimate retransmission of a message this side already
// fully has. The distinction matters because it re-marks the slot dirty
// (see reassembler::add_fragment) so a caller building SACKs from
// drain_dirty_acks() sends a fresh one - otherwise a sender whose
// *original* ack was lost has no way to ever learn its data arrived, since
// nothing about an already-completed slot would change again on its own.
// This is the reassembly-side half of what makes retrying safe to combine
// with replay protection at all: see transport_connection.hpp's
// encrypt_fragment for the other half (a retransmission must carry a fresh
// nonce, never the original ciphertext, or the replay window rejects it
// before ever reaching here).
//
// A free enum at namespace scope, not nested in reassembler<window_capacity>:
// its meaning doesn't depend on the window size, so every caller can name
// it as plain assembly::result regardless of which reassembler<N> they're
// talking to, instead of having to pick one instantiation just to spell
// the type.
enum class result { rejected, incomplete, completed, duplicate_of_completed };

// Level 6 hook: SACK generation reuses the same per-message bitmask
// reassembler already tracks rather than the receiver keeping a second,
// separate "what have I gotten" bitmap. Also free-standing for the same
// reason as result above - its shape doesn't depend on window_capacity.
struct ack_info {
    uint32_t msg_id;
    uint16_t frag_count;
    uint64_t received_mask; // full_mask for a completed message
    bool completed;
};

// window_capacity is a template parameter (not a runtime constructor argument):
// msg_id -> slot indexing uses a mask (msg_id & (window_capacity-1)) instead of
// msg_id % window_capacity on the hot per-fragment path - the difference between
// one AND and a microcoded integer division on every fragment received -
// so window_capacity being a power of two is enforced by a `requires` clause
// at compile time instead of a runtime throw: a bad value is a substitution
// failure at the point of use (e.g. `reassembler<100>`), not an assert
// buried inside the template's own definition. `slots` is a fixed-size
// std::array as a direct consequence: no heap allocation for the window
// itself, only per-slot fragment storage still comes from the heap as
// messages actually arrive.
template <size_t window_capacity = 4096>
    requires (std::has_single_bit(window_capacity))
class reassembler {
public:
    explicit reassembler(size_t global_byte_budget = 16 * 1024 * 1024) : global_byte_budget(global_byte_budget) {}

    // Feeds one fragment. On result::completed, out_message holds the fully
    // reassembled bytes in original order (out_message is left untouched on
    // rejected/incomplete).
    result add_fragment(const fragment_header& hdr, const uint8_t* payload, size_t payload_len, std::vector<uint8_t>& out_message) {
        if (hdr.frag_count == 0 || hdr.frag_count > max_fragments || hdr.frag_index >= hdr.frag_count || payload_len > max_fragment_payload) {
            ++rejected;
            return result::rejected;
        }

        // window_size is enforced to be a power of two (see constructor), so this is a mask, not a division:
        // integer division/modulo goes through the slow microcoded path on every architecture that matters
        // here, and this runs on every single fragment received.
        slot& window_slot = slots[hdr.msg_id & (slots.size() - 1)];

        if (window_slot.state == slot_state::completed && window_slot.msg_id == hdr.msg_id) {
            // A legitimate retransmission of a message we already fully have (not a validation
            // failure) - re-mark dirty so drain_dirty_acks() reports it again. Without this, a
            // sender whose original ack was lost has no way to ever learn its data arrived: nothing
            // about an already-completed window_slot changes on its own to prompt a fresh SACK.
            window_slot.dirty = true;
            return result::duplicate_of_completed;
        }

        bool is_new_message = (window_slot.state == slot_state::empty) || (window_slot.msg_id != hdr.msg_id);
        if (is_new_message) {
            if (window_slot.state != slot_state::empty && !msg_id_is_newer(hdr.msg_id, window_slot.msg_id)) {
                // Whatever currently occupies this window_slot is not older than the incoming message: either an
                // in-progress message that hasn't been superseded yet, or this is a stale/replayed msg_id
                // trying to clobber something newer. Either way, refuse to evict.
                ++rejected;
                return result::rejected;
            }
            total_bytes_buffered -= window_slot.bytes_buffered; // evict whatever was here (empty contributes 0)
            window_slot = slot{};
            window_slot.state = slot_state::in_progress;
            window_slot.msg_id = hdr.msg_id;
            window_slot.frag_count = hdr.frag_count;
            window_slot.fragments.assign(hdr.frag_count, {});
        } else if (window_slot.frag_count != hdr.frag_count) {
            // Same msg_id, different frag_count: a well-behaved sender never does this mid-message.
            ++rejected;
            return result::rejected;
        }

        uint64_t bit = 1ull << hdr.frag_index;
        if (window_slot.received_mask & bit) return result::incomplete; // duplicate fragment within this message; already have it

        if (total_bytes_buffered + payload_len > global_byte_budget) {
            ++rejected;
            return result::rejected;
        }

        window_slot.fragments[hdr.frag_index].assign(payload, payload + payload_len);
        window_slot.received_mask |= bit;
        window_slot.bytes_buffered += payload_len;
        total_bytes_buffered += payload_len;
        window_slot.dirty = true;

        uint64_t full_mask = (hdr.frag_count == 64) ? ~0ull : ((1ull << hdr.frag_count) - 1);
        if (window_slot.received_mask != full_mask) return result::incomplete;

        out_message.clear();
        out_message.reserve(window_slot.bytes_buffered);
        for (auto& frag : window_slot.fragments) out_message.insert(out_message.end(), frag.begin(), frag.end());

        total_bytes_buffered -= window_slot.bytes_buffered;
        window_slot.fragments.clear();
        window_slot.fragments.shrink_to_fit();
        window_slot.bytes_buffered = 0;
        window_slot.state = slot_state::completed;
        return result::completed;
    }

    // Level 6 hook: SACK generation reuses this same bitmask rather than
    // tracking receive state twice. Each slot remembers whether it changed
    // (new fragment landed, or just completed) since the last call; only
    // dirty slots are reported and each is cleared on the way out, so a
    // receiver can build "what changed since I last told my peer" SACKs
    // instead of re-announcing the entire window every tick.
    std::vector<ack_info> drain_dirty_acks() {
        std::vector<ack_info> out;
        for (auto& window_slot : slots) {
            if (!window_slot.dirty) continue;
            out.push_back(ack_info{window_slot.msg_id, window_slot.frag_count, window_slot.received_mask, window_slot.state == slot_state::completed});
            window_slot.dirty = false;
        }
        return out;
    }

    size_t bytes_buffered() const { return total_bytes_buffered; }
    uint64_t rejected_count() const { return rejected; }
    size_t window_size() const { return slots.size(); }

private:
    enum class slot_state : uint8_t { empty, in_progress, completed };

    struct slot {
        slot_state state = slot_state::empty;
        uint32_t msg_id = 0;
        uint16_t frag_count = 0;
        uint64_t received_mask = 0;
        size_t bytes_buffered = 0;
        bool dirty = false;
        std::vector<std::vector<uint8_t>> fragments;
    };

    static constexpr bool msg_id_is_newer(uint32_t a, uint32_t b) {
        // Serial-number-arithmetic comparison (RFC 1982 style): correct
        // across uint32_t wraparound as long as `a` and `b` are within half
        // the ID space of each other, which holds for any window_size
        // that's a tiny fraction of 2^32.
        return (int32_t)(a - b) > 0;
    }

    std::array<slot, window_capacity> slots{};
    size_t global_byte_budget;
    size_t total_bytes_buffered = 0;
    uint64_t rejected = 0;
};

} // namespace assembly
