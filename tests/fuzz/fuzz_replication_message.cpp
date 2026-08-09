// Fuzzes replica_applier::on_message() - replication's wire parser.
//
// These payloads are AEAD-authenticated in production, so this models a
// compromised-or-buggy primary rather than an off-path attacker. That's
// still worth hardening: a replica must not be corruptible by whatever its
// primary sends, and the framing here (kind byte, offsets, lengths) is all
// hand-parsed.
#include "../../replication/replication.hpp"

#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;

    constexpr size_t region_size = 1u << 20;
    std::vector<std::byte> region(region_size, std::byte{0});
    storage::storage<> table(region.data());
    replication::replica_applier<storage::storage_spec<>> applier;

    // Drive several messages out of one input, varying msg_id so the
    // reorder buffer and the in-order drain both get exercised.
    const uint8_t msg_id_seed = data[0];
    const uint8_t* p = data + 1;
    size_t left = size - 1;

    int replies = 0;
    auto count_replies = [&](const std::vector<uint8_t>& r) {
        ++replies;
        // Every reply this layer emits is one of its own two shapes.
        if (r.empty()) __builtin_trap();
        auto k = (replication::msg_kind)r[0];
        if (k != replication::msg_kind::progress_ack && k != replication::msg_kind::resync_request) __builtin_trap();
    };

    for (int i = 0; i < 8 && left >= 2; ++i) {
        size_t chunk = 1 + (p[0] % (left > 255 ? 255 : left));
        ++p; --left;
        if (chunk > left) chunk = left;

        std::vector<uint8_t> msg(p, p + chunk);
        applier.on_message(table, (uint32_t)((msg_id_seed + i) % 4), msg, count_replies);

        p += chunk;
        left -= chunk;
    }

    // The reorder buffer must stay bounded by the number of distinct msg_ids
    // we actually fed, never grow on its own.
    if (applier.reorder_buffer_size() > 8) __builtin_trap();

    // Anything it decided to apply must have landed inside the region.
    table.for_each_raw([&](storage::storage<>::raw_view rv) {
        if (rv.bytes.data() < region.data()) __builtin_trap();
        if (rv.bytes.data() + rv.bytes.size() > region.data() + region_size) __builtin_trap();
    });
    return 0;
}
