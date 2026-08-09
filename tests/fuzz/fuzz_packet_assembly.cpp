// Fuzzes fragment header decoding + reassembly.
//
// packet_assembly.hpp says outright that it "trusts every fragment handed
// to it" - that trust is only safe because the AEAD layer above it
// authenticates the header first. This target removes that assumption on
// purpose: it feeds the reassembler arbitrary msg_id/frag_index/frag_count
// combinations to prove the trust is bounded - no OOB slot indexing, no
// unbounded allocation from a huge frag_count, no infinite loop.
#include "../../net/packet_assembly.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < assembly::fragment_header_len) return 0;

    // Small window on purpose: makes msg_id -> slot collisions (and thus
    // eviction/overwrite paths) far more likely to be hit than they'd be
    // with the production 4096.
    assembly::reassembler<16> re(/*global_byte_budget=*/64 * 1024);

    const uint8_t* p = data;
    size_t left = size;

    // Chew through the input as a sequence of (header, payload) records so
    // one input can drive many fragments into the same window.
    while (left >= assembly::fragment_header_len + 1) {
        auto hdr = assembly::decode_header(p);
        p += assembly::fragment_header_len;
        left -= assembly::fragment_header_len;

        size_t payload_len = p[0];
        ++p; --left;
        if (payload_len > left) payload_len = left;

        std::vector<uint8_t> out;
        auto res = re.add_fragment(hdr, p, payload_len, out);
        p += payload_len;
        left -= payload_len;

        // A completed message must be internally consistent: reassembly
        // reported the whole thing, so the bytes it handed back cannot
        // exceed what the fragment count could possibly carry.
        if (res == assembly::result::completed) {
            if (out.size() > (size_t)hdr.frag_count * assembly::max_fragment_payload) __builtin_trap();
        }
    }

    // Buffered bytes must stay inside the budget the reassembler was given -
    // otherwise a peer could drive unbounded memory growth by sending
    // fragments of messages it never completes.
    if (re.bytes_buffered() > 64 * 1024) __builtin_trap();

    for (auto& ack : re.drain_dirty_acks()) {
        if (ack.frag_count == 0) __builtin_trap();
    }
    return 0;
}
