// Fuzzes storage<>::apply_wal_range() - the replication ingest path.
//
// Distinct from recover(): this one is fed bytes that arrived over the
// network, at an offset the caller chose, into a storage that may already
// hold state. It memcpy's the caller's buffer into the mapped region before
// parsing, so a length that overruns the region is a genuine overflow risk
// rather than just a parse error.
#include "../../storage/storage.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

// Namespace scope: a local class can't hold a static data member.
struct rec { static constexpr storage::tag_t tag = 1; uint64_t v; };

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    constexpr size_t region_size = 1u << 20;
    // Keep well clear of the region end so a legitimate apply can't be the
    // thing that overflows - we're fuzzing the parser, not the caller's
    // arithmetic.
    if (size > region_size / 2) size = region_size / 2;

    std::vector<std::byte> region(region_size, std::byte{0});
    storage::storage<> table(region.data());

    // Seed some real state first, so the fuzzer exercises applying on top
    // of existing records (index collisions, replaced ids) rather than only
    // ever hitting an empty table.
    for (int i = 0; i < 4; ++i) table.create(rec{(uint64_t)i});

    std::size_t before = table.current_offset();
    std::size_t applied = table.apply_wal_range((const std::byte*)data, size);

    if (applied > size) __builtin_trap();
    if (table.current_offset() != before + applied) __builtin_trap();

    table.for_each_raw([&](storage::storage<>::raw_view rv) {
        if (rv.bytes.data() < region.data()) __builtin_trap();
        if (rv.bytes.data() + rv.bytes.size() > region.data() + region_size) __builtin_trap();
    });
    return 0;
}
