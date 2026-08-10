// probe_commit_batching.cpp - why the same engine reports ~1200 durable
// operations per second in one benchmark and ~1000000 in another.
//
// Because those are different units. bench/kv_comparison.cpp flushes once per
// BATCH of 1000 operations; bench/schema_comparison.cpp flushes once per
// TRANSACTION, i.e. once per operation. The device does not care: it commits
// at whatever rate it commits, and everything above it is just how many
// operations were allowed to share one commit.
//
// This sweeps the batch size and reports both numbers side by side. If the
// story is right, ops/sec should scale roughly linearly with the batch while
// commits/sec stays flat at the device's rate.
//
// MEASURED (btrfs, NVMe, full app layer, 800 fsyncs per row, two runs):
//
//   ops/fsync        ops/sec      fsyncs/sec   ms/fsync
//           1           1048            1048      0.954
//           2           2078            1039      0.963
//           5           5412            1082      0.924
//          10          10224            1022      0.978
//          50          46234             925      1.081
//         100         103506            1035      0.966
//         500         483394             967      1.034
//        1000         747916             748      1.337
//        5000        2252910             451      2.219
//
// ops/sec moves by ~2000x across the sweep. fsyncs/sec does not move at all
// until the very last row: it sits at ~1000 per second, ~1 ms each, which is
// this device's commit latency and nothing to do with any engine.
//
// That reconciles the two numbers that look contradictory:
//
//   kv_comparison      980301 ops/sec   = 1000 ops per fsync x  980 fsyncs/sec
//   schema_comparison    1215 ops/sec   =    1 op  per fsync x 1215 fsyncs/sec
//
// Same machine, same engine, same ~1 ms commit. The first number was never a
// property of the storage engine - it was a property of the batch size.
//
// The last row is the one place the batch stops being free: at 5000 records
// per commit each fsync carries ~360 KB, so the bytes themselves start to
// cost and the rate halves. Somewhere around 100-1000 operations per commit
// is where the fixed cost is amortised and the payload is still small.
//
// Build:
//   g++ -std=c++23 -O3 -march=native -o probe bench/probe_commit_batching.cpp
#include "app_txn.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace app;

namespace {

    using probe_spec = storage::storage_spec<storage::durability::local_sync, "commitbatch.db", 4ull << 30>;

    constexpr std::size_t n_commits = 800; // held constant, so every row does the same number of fsyncs

    double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }

    void wipe() {
        std::remove("commitbatch.db");
        std::remove("commitbatch.db.data");
    }

    void run(std::size_t batch) {
        wipe();
        const std::size_t n_ops = n_commits * batch;
        table_t<probe_spec> table;
        uint64_t txn_id = 1;

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_ops; ++i) {
            txn<probe_spec> tx(table, txn_id++);
            tx.insert_customer(customer{ (uint64_t)i + 1, 1'000'000, 0, 0 });
            tx.commit(false);                       // the transaction ends...
            if ((i + 1) % batch == 0) table.sync(); // ...durability is a separate decision
        }
        auto t1 = std::chrono::steady_clock::now();

        const double s = secs(t0, t1);
        std::printf("  %9zu %12zu %14.0f %14.0f %12.3f\n",
                    batch, n_ops, n_ops / s, n_commits / s, s / n_commits * 1000.0);
        wipe();
    }

} // namespace

int main() {
    std::printf("full app layer (unique index, constraints, transactions), %zu fsyncs per row\n\n", n_commits);
    std::printf("  %9s %12s %14s %14s %12s\n", "ops/fsync", "ops", "ops/sec", "fsyncs/sec", "ms/fsync");
    std::printf("  %s\n", std::string(66, '-').c_str());
    for (std::size_t batch : { 1ul, 2ul, 5ul, 10ul, 50ul, 100ul, 500ul, 1000ul, 5000ul })
        run(batch);
    std::printf("\n  ops/sec tracks the batch size; fsyncs/sec does not move. The device\n"
                "  commits at its own rate and nothing above it changes that - the only\n"
                "  question a durable workload really answers is how many operations were\n"
                "  willing to share one commit.\n");
    return 0;
}
