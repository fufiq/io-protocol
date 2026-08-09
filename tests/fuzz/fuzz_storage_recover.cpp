// Fuzzes storage<>::recover() - the WAL parser, fed arbitrary bytes.
//
// This is the highest-value storage target: recover() is the one place that
// parses a file the process did not just write itself. A truncated tail, a
// torn record, a corrupt length, a bogus id - all of it has to stop cleanly
// at the first bad record rather than read past the end, loop forever, or
// index the record index out of bounds.
#include "../../storage/storage.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    constexpr size_t region_size = 1u << 20;
    if (size > region_size) size = region_size;

    // A fresh region every run: recover() must never depend on, or be
    // corrupted by, whatever a previous input left behind.
    std::vector<std::byte> region(region_size, std::byte{0});
    std::memcpy(region.data(), data, size);

    storage::storage<> table(region.data());
    std::size_t recovered = table.recover(size);

    // Whatever it accepted must be internally consistent: it can never
    // claim to have recovered more bytes than it was given.
    if (recovered > size) __builtin_trap();
    if (table.current_offset() != recovered) __builtin_trap();

    // Every id it decided is live must be readable, and its bytes must sit
    // inside the region - a header whose len ran off the end must have been
    // rejected, not indexed.
    table.for_each_raw([&](storage::storage<>::raw_view rv) {
        if (rv.bytes.data() < region.data()) __builtin_trap();
        if (rv.bytes.data() + rv.bytes.size() > region.data() + region_size) __builtin_trap();
    });
    return 0;
}
