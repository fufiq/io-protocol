// probe_snapshot_cost.cpp - what background_compact()'s phase-1 snapshot
// actually costs in memory, since that snapshot is the one structure in the
// engine with no eviction story at all: it is a std::vector plus a
// std::unordered_map, i.e. anonymous memory, which can only be swapped.
//
// MEASURED, 400k live records of 64 bytes:
//   live_bytes             25000 KB
//   RSS growth             49708 KB
//   overhead per record     63.3 bytes ON TOP of the payload
//
// So a snapshot costs roughly live_bytes + 64 bytes per live record. For
// small records that is a little over 2x the live set, in memory the kernel
// cannot reclaim, held while the writer's mutex is taken in phase 1.
//
// compact() (the blocking variant) builds no snapshot at all - it streams
// straight from the index into the scratch regions, which are file-backed
// and evictable. That is the variant to use when the live set approaches RAM.
//
// Build: g++ -std=c++23 -O2 -march=native -o probe bench/probe_snapshot_cost.cpp
#include "../storage/storage.hpp"
#include <cstdio>
#include <fstream>
#include <string>
using S = storage::storage<>;
struct rec { static constexpr storage::tag_t tag = 1; uint64_t v; uint8_t pad[56]; };
std::size_t rss_kb() {
    std::ifstream f("/proc/self/status"); std::string k;
    while (f >> k) if (k == "VmRSS:") { std::size_t v; f >> v; return v; }
    return 0;
}
int main() {
    std::printf("sizeof(arena_ref)                = %zu bytes\n", sizeof(S::arena_ref));
    std::printf("sizeof(unordered_map node approx)= key %zu + value %zu + ptr 8\n",
                sizeof(storage::record_id), sizeof(S::arena_ref));

    constexpr std::size_t n = 400'000;
    std::vector<std::byte> region(n * 128 + (1u << 20));
    S table(region.data());
    for (std::size_t i = 0; i < n; ++i) table.create(rec{i, {}});

    std::size_t before = rss_kb();
    S::snapshot snap;
    table.snapshot_into(snap);
    std::size_t after = rss_kb();

    std::printf("\n%zu live records of %zu bytes  (live_bytes = %zu KB)\n", n, sizeof(rec), table.live_bytes() / 1024);
    std::printf("  RSS grew by            %zu KB\n", after - before);
    std::printf("  arena capacity         %zu KB\n", snap.arena.capacity() / 1024);
    std::printf("  map holds              %zu entries, %zu buckets\n", snap.index.size(), snap.index.bucket_count());
    std::printf("  => snapshot overhead per live record: %.1f bytes on top of the %zu-byte payload\n",
                (double)((after - before) * 1024 - table.live_bytes()) / n, sizeof(rec));
    return 0;
}
