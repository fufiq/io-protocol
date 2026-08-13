// shard_scaling.cpp - does one writer per shard actually scale with cores?
//
// SHARDING IS NOT A FEATURE OF THE ENGINE. It is N instances of one spec with N
// sets of paths, and this file is the whole implementation of it: a vector of
// field_storage, a thread each, and nothing shared. That is the claim being
// tested - that nothing is shared - and it is falsifiable: every `static` in
// field_storage.hpp is a pure function, every scratch buffer is a local, and
// all mutable state lives behind the instance's own two mappings. If that were
// wrong, or if there were hidden contention in the allocator, the page cache or
// the kernel's mmap handling, the curve would bend and this would show it.
//
// No routing and no queue: which key goes to which shard belongs in the layer
// above, and putting it here would measure the router rather than the engine.
//
// Threads are pinned, and to physical cores rather than to CPU numbers - see
// cpu_order() for why that distinction cost a wrong result once already.

#include "../storage/field_storage.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
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

    // Durability, swept alongside the shard count, because whether it scales is
    // a separate question from whether the writers do.
    //
    // There are two reasons to expect it might, and one to expect it might not.
    // For: every shard owns its own log file, so the per-inode commit that §8
    // warns about does not couple them, and the logs are posix_fallocate'd, so
    // an append never extends i_size and never needs a metadata journal commit.
    // Against: there is one device and one filesystem journal underneath all of
    // them, and that is exactly where concurrent syncs would serialise.
    //
    // 2000 operations at 32 bytes each is 64 KB of dirty tail per barrier,
    // which is the floor §13 asks for.
    //
    // THE COUNT IS THE ONLY BOUND HERE, deliberately, and the time bound is
    // parked out of reach. Production wants both - the count bounds what a
    // crash loses, the time bounds how long a writer waits - but a time bound
    // makes this particular measurement meaningless. Run with 2000 ops or 1 ms,
    // sixteen shards fired 968,907 barriers over 3.2M updates: one per 3.3
    // operations. That is the §8 degeneration, and it happens because msync
    // under sixteen concurrent writers takes far longer than 1 ms, so the check
    // right after a barrier always finds the interval already expired. It
    // measures the collapse, not the scaling.
    //
    // Fixed at 2000 operations, every shard has the same crash window at every
    // shard count, and the barrier count is 100 per shard by construction. Then
    // the only thing left moving is the device.
    bool do_sync = false;
    std::size_t barrier_ops = 2000;
    std::chrono::microseconds barrier_us{std::chrono::hours(1)};

    struct blob {
        static constexpr storage::tag_t tag = 1;
        uint64_t key, counter, f1, f2;
        uint8_t payload[value_bytes - 32];
    };
    static_assert(sizeof(blob) == value_bytes);

    // ONE spec for every shard. What separates them is where their files go,
    // which is now a run-time argument - so the shard count is an ordinary
    // number and this loop is an ordinary loop.
    using shard_spec = storage::field_spec<
        "shard.log", shard_capacity, "shard.idx", shard_max_ids, shard_overrides,
        8, value_bytes>;
    using shard = storage::field_storage<shard_spec>;

    std::string shard_dir(std::size_t i) { return "shard." + std::to_string(i); }

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
        std::size_t barriers = 0;
        double update_seconds = 0;
        bool ok = false;
        char pad[64 - 2 * sizeof(std::size_t) - sizeof(double) - sizeof(bool)] = {};
    };

    // Pinning to CPU n for shard n is wrong on any machine with SMT, and wrong
    // in a way that looks like a result: on this i5-14400F, CPUs 0 and 1 are
    // the two threads of ONE physical core, so two shards landed on one core
    // and two writers measured 0.90x of one. That reads as contention in the
    // engine and is nothing of the kind.
    //
    // Cores are also not all the same speed. This CPU has six P-cores at 4.7
    // GHz and four E-cores at 3.5, and since the parallel duration is the
    // SLOWEST shard's, the first shard placed on an E-core sets the number for
    // all of them. So the order is: one CPU per physical core, fastest first,
    // and only once every core has a writer do the SMT siblings get used. The
    // bend in the curve then means something.
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
    void run_shard(std::size_t i, tally& out, std::barrier<>& gate) {
        pin(i);
        const std::string dir = shard_dir(i);
        ::mkdir(dir.c_str(), 0755);

        auto t = std::make_unique<shard>(storage::storage_paths::under<shard_spec>(dir));
        if (!t->ok()) { gate.arrive_and_drop(); return; }

        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));
        std::vector<storage::record_id> ids;
        ids.reserve(records_per_shard);
        for (std::size_t k = 0; k < records_per_shard; ++k) {
            b.key = k + 1;
            b.counter = k;
            ids.push_back(t->create(b));
        }

        xorshift rng(9 + i);
        gate.arrive_and_wait();

        const auto t0 = std::chrono::steady_clock::now();
        auto last = t0;
        std::size_t since = 0, fired = 0;
        for (std::size_t k = 0; k < updates_per_shard; ++k) {
            t->update<&blob::counter>(ids[rng.range(records_per_shard)], (uint64_t)k);
            if (!do_sync) continue;
            const auto now = std::chrono::steady_clock::now();
            if (++since < barrier_ops && now - last < barrier_us) continue;
            t->sync();
            since = 0;
            last = now;
            ++fired;
        }
        if (do_sync) { t->sync(); ++fired; }
        out.update_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        out.updates = updates_per_shard;
        out.barriers = fired;
        out.ok = true;
    }

    void cleanup() {
        for (std::size_t i = 0; i < max_shards; ++i) {
            const std::string dir = shard_dir(i);
            std::remove((dir + "/shard.log").c_str());
            std::remove((dir + "/shard.idx").c_str());
            ::rmdir(dir.c_str());
        }
    }

    // Swept finely around the physical core count, because two different things
    // bend this curve and a coarse sweep cannot tell them apart: running out of
    // physical cores, and running out of memory bandwidth.
    void run_at(std::size_t n, std::vector<tally>& out) {
        cleanup();
        std::barrier gate((std::ptrdiff_t)n);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < n; ++i)
            threads.emplace_back([i, &out, &gate] { run_shard(i, out[i], gate); });
        for (auto& th : threads) th.join();
    }

    void sweep() {
        // Thrown away. The first point of a sweep pays for the first mkdir, the
        // first file creation and a cold cache, and since every scaling figure
        // is normalised against n=1, a slow first point corrupts the whole
        // column rather than just its own row. Seen as 17.6M at one shard
        // against 25.0M, which then reported two shards as 2.72x.
        { std::vector<tally> warm(max_shards); run_at(1, warm); }

        double one = 0;
        for (std::size_t n : { 1u, 2u, 4u, 6u, 8u, 10u, 12u, 16u }) {
            if (n > max_shards) break;
            std::vector<tally> out(max_shards);
            run_at(n, out);

            // The slowest shard is the parallel duration: they start together
            // at the barrier, so taking the max is what "how long did this take
            // with n writers running" means. Averaging would flatter it.
            std::size_t total = 0, live = 0, barriers = 0;
            double wall = 0;
            for (std::size_t i = 0; i < n; ++i) {
                total += out[i].updates;
                barriers += out[i].barriers;
                live += out[i].ok ? 1 : 0;
                wall = std::max(wall, out[i].update_seconds);
            }
            if (live != n) { std::printf("  %zu of %zu shards failed to map\n", n - live, n); break; }

            const double rate = total / wall;
            if (n == 1) one = rate;
            std::printf("%7zu %14.0f %12.0f %8.2fx %10zu\n", n, rate, rate / n, rate / one, barriers);
            std::fflush(stdout);
        }
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) updates_per_shard = (std::size_t)std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) records_per_shard = (std::size_t)std::strtoull(argv[2], nullptr, 10);
    if (argc > 3) barrier_ops = (std::size_t)std::strtoull(argv[3], nullptr, 10);
    if (argc > 4) barrier_us = std::chrono::microseconds(std::strtoull(argv[4], nullptr, 10));

    std::printf("# %u hardware threads, %zu records and %zu updates per shard, %zu-byte records\n",
                std::thread::hardware_concurrency(), records_per_shard, updates_per_shard, value_bytes);

    for (int pass = 0; pass < 2; ++pass) {
        do_sync = pass == 1;
        std::printf("\n# %s\n", do_sync
            ? "durable: every shard syncs its own log every 2000 updates (fixed crash window)"
            : "no durability barrier");
        std::printf("%7s %14s %12s %9s %10s\n", "shards", "updates/sec", "per shard", "scaling", "barriers");
        sweep();
    }

    cleanup();
    return 0;
}
