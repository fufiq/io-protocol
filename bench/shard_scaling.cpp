// shard_scaling.cpp - does one writer per shard actually scale with cores?
//
// The claim being tested is narrow and falsifiable: N independent
// field_storage instances, one thread each, share nothing, and therefore
// should scale close to linearly until memory bandwidth stops it. If that is
// wrong - if there is hidden contention, in the allocator, the page cache, or
// the kernel's mmap handling - the curve will bend and this will show it.
//
// Every thread owns its shard outright. No routing, no queue, no atomics on
// the hot path: routing by key belongs in the layer above and would measure
// the router rather than the engine.
//
// Threads are pinned. Without pinning the scheduler migrates writers between
// cores mid-run and the mappings' pages follow them around, which showed up as
// a 20-30% run-to-run spread that had nothing to do with the thing being
// measured.

#include "../storage/field_storage.hpp"

#include <algorithm>
#include <barrier>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <vector>

namespace {

    constexpr std::size_t value_bytes = 1024;
    constexpr std::size_t max_shards = 16;
    // Sized for what the phases actually write: the fill is records × 1 KB and
    // an update appends a 24-byte header plus 8 bytes of field, so 200k updates
    // are ~6 MB. Sixteen shards of this fit comfortably; 192 MB each did not,
    // and on tmpfs that came straight out of RAM and bent the curve on its own.
    constexpr std::size_t shard_capacity = 64ull << 20;
    constexpr std::size_t shard_max_ids = 100'000;
    constexpr std::size_t shard_overrides = 100'000;

    std::size_t records_per_shard = 20'000;
    std::size_t updates_per_shard = 200'000;

    struct blob {
        static constexpr storage::tag_t tag = 1;
        uint64_t key, counter, f1, f2;
        uint8_t payload[value_bytes - 32];
    };
    static_assert(sizeof(blob) == value_bytes);

    template <std::size_t I>
    using shard_spec = storage::field_spec<
        storage::shard_path<"shard.log", I>, shard_capacity,
        storage::shard_path<"shard.idx", I>, shard_max_ids, shard_overrides,
        8, value_bytes>;

    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };

    // Per-thread, cache-line isolated. A shared counter here would measure the
    // counter: this project already lost a day to exactly that, where four
    // reader threads ping-ponging one line slowed the writer 7-16x and read as
    // a cost of the mechanism under test.
    struct alignas(64) tally {
        std::size_t updates = 0;
        double update_seconds = 0;
        bool ok = false;
        char pad[64 - sizeof(std::size_t) - sizeof(double) - sizeof(bool)] = {};
    };

    // Pinning to CPU n for shard n is wrong on any machine with SMT, and wrong
    // in a way that looks like a result: on this i5-14400F, CPUs 0 and 1 are
    // the two threads of ONE physical core, so two shards landed on one core
    // and two writers measured 0.90x of one. That reads as contention in the
    // engine and is nothing of the kind.
    //
    // So the order is built from /sys: one CPU per physical core first, and
    // only once every core has a writer do the SMT siblings get used. The bend
    // in the curve then means something.
    // Cores are also not all the same speed. This i5-14400F has six P-cores at
    // 4.7 GHz and four E-cores at 3.5 GHz, and since the parallel duration is
    // the SLOWEST shard's, the first shard placed on an E-core sets the number
    // for all of them. Sorting by max frequency makes that visible where it
    // happens instead of smearing it: the curve should rise cleanly across the
    // fast cores and bend exactly when the slow ones are reached.
    long cpu_max_khz(unsigned c) {
        char path[160];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", c);
        std::FILE* f = std::fopen(path, "r");
        long khz = 0;
        if (f) { if (std::fscanf(f, "%ld", &khz) != 1) khz = 0; std::fclose(f); }
        return khz;
    }

    std::vector<int> cpu_order() {
        struct entry { int core, cpu; long khz; };
        std::vector<entry> cpus;
        for (unsigned c = 0; c < std::thread::hardware_concurrency(); ++c) {
            char path[128];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/core_id", c);
            std::FILE* f = std::fopen(path, "r");
            int core = (int)c;
            if (f) { if (std::fscanf(f, "%d", &core) != 1) core = (int)c; std::fclose(f); }
            cpus.push_back({ core, (int)c, cpu_max_khz(c) });
        }
        std::stable_sort(cpus.begin(), cpus.end(), [](const entry& a, const entry& b) { return a.khz > b.khz; });

        std::vector<int> order, seen;
        for (int pass = 0; pass < 2; ++pass) {
            for (const entry& e : cpus) {
                const bool taken = std::find(seen.begin(), seen.end(), e.core) != seen.end();
                if (taken != (pass == 1)) continue;
                if (pass == 0) seen.push_back(e.core);
                order.push_back(e.cpu);
            }
        }
        return order;
    }

    const std::vector<int>& pin_order() {
        static const std::vector<int> order = cpu_order();
        return order;
    }

    void pin(std::size_t slot) {
        const auto& order = pin_order();
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(order[slot % order.size()], &set);
        ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    }

    // The fill is memory-bandwidth work and would otherwise be counted against
    // a phase that is not it, so every shard fills first and they all wait at
    // the barrier before a single update is timed.
    template <std::size_t I>
    void run_shard(tally& out, std::barrier<>& gate) {
        pin(I);
        auto t = std::make_unique<storage::field_storage<shard_spec<I>>>();
        if (!t->ok()) { gate.arrive_and_drop(); return; }

        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));
        std::vector<storage::record_id> ids;
        ids.reserve(records_per_shard);
        for (std::size_t i = 0; i < records_per_shard; ++i) {
            b.key = i + 1;
            b.counter = i;
            ids.push_back(t->create(b));
        }

        xorshift rng(9 + I);
        gate.arrive_and_wait();

        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < updates_per_shard; ++i)
            t->template update<&blob::counter>(ids[rng.range(records_per_shard)], (uint64_t)i);
        out.update_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        out.updates = updates_per_shard;
        out.ok = true;
    }

    // The shard index has to be a compile-time constant (it is baked into the
    // paths), while the shard COUNT is a runtime sweep. This bridges the two:
    // spawn threads for indices 0..N-1, skipping the ones past the count.
    template <std::size_t... I>
    void spawn(std::size_t count, std::vector<tally>& out, std::barrier<>& gate, std::index_sequence<I...>) {
        std::vector<std::thread> threads;
        (..., (I < count ? (void)threads.emplace_back([&out, &gate] { run_shard<I>(out[I], gate); }) : void()));
        for (auto& th : threads) th.join();
    }

    void cleanup() {
        for (std::size_t i = 0; i < max_shards; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "shard.%zu.log", i);
            std::remove(path);
            std::snprintf(path, sizeof(path), "shard.%zu.idx", i);
            std::remove(path);
        }
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) updates_per_shard = (std::size_t)std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) records_per_shard = (std::size_t)std::strtoull(argv[2], nullptr, 10);

    std::printf("# %u hardware threads, %zu records and %zu updates per shard, %zu-byte records\n",
                std::thread::hardware_concurrency(), records_per_shard, updates_per_shard, value_bytes);
    std::printf("%7s %14s %12s %9s\n", "shards", "updates/sec", "per shard", "scaling");

    double one = 0;
    // Swept finely around the physical core count, because two different
    // things bend this curve and a coarse sweep cannot tell them apart:
    // running out of physical cores, and running out of memory bandwidth.
    for (std::size_t n : { 1u, 2u, 4u, 6u, 8u, 10u, 12u, 16u }) {
        if (n > max_shards) break;
        cleanup();
        std::vector<tally> out(max_shards);
        std::barrier gate((std::ptrdiff_t)n);

        spawn(n, out, gate, std::make_index_sequence<max_shards>{});

        // The slowest shard is the parallel duration: they start together at
        // the barrier, so taking the max is what "how long did this take with
        // n writers running" means. Averaging would flatter the result.
        std::size_t total = 0, live = 0;
        double wall = 0;
        for (std::size_t i = 0; i < n; ++i) {
            total += out[i].updates;
            live += out[i].ok ? 1 : 0;
            wall = std::max(wall, out[i].update_seconds);
        }
        if (live != n) { std::printf("  %zu of %zu shards failed to map\n", n - live, n); break; }

        const double rate = total / wall;
        if (n == 1) one = rate;
        std::printf("%7zu %14.0f %12.0f %8.2fx\n", n, rate, rate / n, rate / one);
        std::fflush(stdout);
    }

    cleanup();
    return 0;
}
