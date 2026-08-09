// probe_inplace_update_cost.cpp - isolates WHY durable random updates are
// slow (see kv_comparison.cpp): same update count, same sync count, only the
// key order differs. Sequential vs random separates "fsync is expensive"
// from "in-place writes scatter dirty pages across the mapping".
//
// Build: g++ -std=c++23 -O3 -march=native -o probe bench/probe_inplace_update_cost.cpp
// Probe: is our durable-update slowness caused by the in-place write
// scattering dirty pages across the mapping, or is it just fsync cost?
//
// Same number of updates, same number of syncs - only the key ORDER differs.
// If sequential is much faster than random, the scatter is the cause.
#include "../storage/storage.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using spec = storage::storage_spec<storage::durability::local_sync, "probe.db", 1ull << 30>;
struct rec { static constexpr storage::tag_t tag = 1; uint64_t key; uint8_t pad[56]; };

double secs(auto a, auto b) { return std::chrono::duration<double>(b - a).count(); }

int main() {
    constexpr size_t n = 200'000, batch = 1000;
    for (int mode = 0; mode < 2; ++mode) {
        std::remove("probe.db");
        storage::storage<spec> t;
        rec r{};
        for (size_t i = 0; i < n; ++i) { r.key = i + 1; t.create(r); }
        t.sync();

        std::vector<uint64_t> keys(n);
        for (size_t i = 0; i < n; ++i) keys[i] = i + 1;
        if (mode == 1) std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7));

        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < n; ++i) {
            t.update<&rec::key>(keys[i], keys[i]);
            if ((i + 1) % batch == 0) t.sync();
        }
        t.sync();
        auto t1 = std::chrono::steady_clock::now();
        double s = secs(t0, t1);
        std::printf("%-12s %zu durable updates, %zu syncs: %7.3f s  %10.0f ops/sec  (%6.2f ms/sync)\n",
                    mode ? "RANDOM key" : "SEQUENTIAL", n, n / batch, s, n / s, 1000.0 * s / (n / batch));
        std::remove("probe.db");
    }
    return 0;
}
