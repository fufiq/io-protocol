// probe_snapshot_cost.cpp - what background_compact()'s phase-1 snapshot
// actually costs in memory, since that snapshot is the one structure in the
// engine with no eviction story at all: it is a std::vector plus a
// std::unordered_map, i.e. anonymous memory, which can only be swapped.
//
// MEASURED, 400k live records of 64 bytes (live_bytes = 25000 KB):
//
//   heap arena + unordered_map    RSS +49708 KB   anonymous, +63.3 B/record
//   both halves mapped from files RSS +31316 KB   file-backed, +16 B/record
//
// Read the second column, not the first. RSS did NOT go to zero and could
// not: the arena still holds a copy of every live record and the ref array
// still holds a slot per id, so the same bytes get touched either way. What
// changed is WHAT KIND of memory they are. Anonymous pages can only go to
// swap; these are page-cache pages, so writeback cleans them and the kernel
// reclaims them under pressure and faults them back on demand.
//
// The per-record overhead on top of the payload fell from 63.3 bytes to 16
// as well, because the unordered_map (node + key + bucket) became one flat
// 16-byte arena_ref indexed by id.
//
// This was the last allocation in the engine that grew with the dataset and
// had no eviction story, and phase 1 makes it while holding the writer's
// mutex.
//
// compact() (the blocking variant) builds no snapshot at all - it streams
// straight from the index into the scratch regions. That is still the
// variant to use when the live set approaches RAM, since it needs neither
// the arena nor the ref array.
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
    std::printf("sizeof(arena_ref) = %zu bytes, one per id\n", sizeof(S::arena_ref));

    constexpr std::size_t n = 400'000;
    std::vector<std::byte> region(n * 128 + (1u << 20));
    S table(region.data());
    for (std::size_t i = 0; i < n; ++i) table.create(rec{i, {}});

    std::size_t before = rss_kb();
    S::snapshot snap;
    if (!snap.open("probe_snap.arena", 256ull << 20, "probe_snap.refs", 4'000'000)) { std::puts("open failed"); return 1; }
    table.snapshot_into(snap);
    std::size_t after = rss_kb();

    std::printf("\n%zu live records of %zu bytes  (live_bytes = %zu KB)\n", n, sizeof(rec), table.live_bytes() / 1024);
    std::printf("  RSS grew by            %zu KB\n", after - before);
    std::printf("  arena used             %zu KB (file-backed, evictable)\n", snap.used() / 1024);
    std::printf("  overflowed             %s\n", snap.overflowed() ? "YES" : "no");
    return 0;
}
