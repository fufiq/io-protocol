// kv_comparison.cpp - our storage engine vs LMDB (and RocksDB, if built with
// -DHAVE_ROCKSDB) on an identical workload.
//
// READ THIS BEFORE BELIEVING ANY NUMBER BELOW.
//
// This is not an apples-to-apples comparison and cannot be made into one.
// Our storage does drastically less than either of the others:
//   - keys are monotonically assigned record ids, not arbitrary byte strings
//   - no range scans, no iteration order, no prefix seeks
//   - no MVCC, no snapshots-for-readers, no concurrent readers at all
//   - single process, single writer thread (see HEAP_API.md §7)
//   - no compression, no column families, no compaction tuning knobs
// LMDB gives you a full B+tree with real keys, multi-process readers and
// ACID transactions. RocksDB gives you an LSM with compaction, column
// families, and a tuning surface bigger than this whole repo.
//
// So what this measures is narrow and specific: "how fast is our thing at
// the one job it was built for, next to general-purpose engines doing that
// same job". A win here means nothing about being a better database - it
// means the general-purpose machinery costs something, which was already
// obvious. It's a sanity check that we're in a sane ballpark, not a scoreboard.
//
// Durability is the other place benchmarks lie. Comparing our unsynced
// in-memory path against LMDB's sync-on-commit would be meaningless, so
// every engine is measured twice: once with no fsync at all, once with a
// flush per batch of `batch_size` operations. Only compare rows with the
// same durability label.
//
// One more caveat, specific to RocksDB: it is run here with essentially
// default options (create_if_missing and nothing else). Untuned RocksDB is
// not RocksDB at its best - no block cache sizing, no bloom filter config,
// no compaction tuning. Its read numbers below in particular should be read
// as "out of the box", not "as fast as it goes".
//
// Build:
//   g++ -std=c++23 -O3 -march=native -I.. -llmdb -o kv_comparison bench/kv_comparison.cpp
//   (add -DHAVE_ROCKSDB -lrocksdb for the three-way run)
//
// ---------------------------------------------------------------------------
// MEASURED (500k records, 64B values, batch=1000, NVMe, gcc -O3).
//
// Absolute numbers drift hard with the state of the filesystem - the runs
// below are months apart on a disk that went from 98% to 81% full, and every
// engine got 3-4x faster in between. Compare RATIOS WITHIN a run, never
// numbers ACROSS runs.
//
// And the drift is not small even back to back. Three consecutive runs of
// this binary, nothing changed in between, "ours / random update / fsync per
// batch": 50.4K/s, 967K/s, 966K/s. A 20x swing from the filesystem alone. The
// no-fsync columns are stable to a few percent and are the ones to read when
// isolating a CPU-side change; the fsync columns cannot resolve anything
// smaller than an order of magnitude on this machine.
//
// BEFORE the log/data split (disk 98% full):
//
//   no fsync          ours          lmdb        rocksdb
//   insert            14.5M/s       0.90M/s     0.46M/s
//   random read        101M/s       2.20M/s     0.43M/s
//   random update     31.1M/s       0.37M/s     0.40M/s
//
//   fsync per batch   ours          lmdb        rocksdb
//   insert             354K/s       27.7K/s     64.5K/s
//   random read        104M/s       2.20M/s     0.44M/s
//   random update     23.4K/s       17.1K/s     71.2K/s   <-- RocksDB 3.0x ahead
//
// AFTER the split (disk 81% full):
//
//   no fsync          ours          lmdb        rocksdb
//   insert            8.53M/s       0.98M/s     0.46M/s
//   random read        112M/s       2.24M/s     0.43M/s
//   random update     31.9M/s       0.39M/s     0.42M/s
//
//   fsync per batch   ours          lmdb        rocksdb
//   insert             861K/s      101.4K/s     310K/s
//   random read        113M/s       2.21M/s     0.44M/s
//   random update      980K/s      49.8K/s      291K/s   <-- ours 3.4x ahead
//
// The durable random update row is why this file exists. It used to be the
// one workload this design lost, and lost badly: an LSM turns a random update
// into a sequential append, while we appended the WAL record AND patched the
// record in place, so the per-inode fs commit had to flush dirty pages
// scattered across the whole mapping. Moving the materialized records into
// their own file (see storage.hpp) removed the scattered pages from the log's
// inode entirely. RocksDB went from 3.0x ahead to 3.4x behind, measured
// against itself in the same runs.
//
// What the split COST, and it is visible above: non-durable insert dropped
// from 14.5M/s to 8.53M/s. A create now writes its payload twice, once to the
// log and once to the data region. That is the trade, and it only shows up
// where nothing is being synced - as soon as durability is on, the flush
// dominates and the second write disappears into the noise.
//
// Reads and non-durable updates are unchanged, as expected: neither touches
// the log.
//
// ---------------------------------------------------------------------------
// The original diagnosis, kept because it is what led to the fix
// (bench/probe_inplace_update_cost.cpp, btrfs, 200k updates, 200 flushes):
//
//   A seq  + msync(tail, MS_SYNC)     1.11 ms/sync     62 MiB written
//   B rand + msync(tail, MS_SYNC)    19.46 ms/sync   1171 MiB written
//   C rand + no msync                 --             ~  1 MiB written
//   D rand + msync(tail, MS_ASYNC)    --             ~  4 MiB written
//   E rand + msync(WHOLE, MS_SYNC)   44.22 ms/sync   1348 MiB written
//
// Note B: we asked msync to flush only the log tail, yet 1171 MiB reached the
// device for 12.8 MB of logical updates - ~90x amplification. C and D show
// the dirty pages themselves are nearly free; the cost was entirely in the
// synchronous flush.
//
// It is NOT that msync discards the range. The range is honoured for
// writeback - what is not scoped to it is the filesystem commit MS_SYNC
// triggers (msync -> vfs_fsync_range -> btrfs_sync_file -> log/transaction
// commit). Swapping the syscall while keeping the identical range proved it:
//
//   msync(tail, MS_SYNC)                 3.71 s   1143 MiB
//   sync_file_range(tail, WRITE|WAIT)    0.04 s     23 MiB   (50x less I/O)
//   sync_file_range + one fsync at end   0.05 s     39 MiB
//
// Copy-on-write was a contributor but not the main term: chattr +C
// (nodatacow) only moved 1143 -> 745 MiB, while dropping the per-batch commit
// moved it to 23 MiB. Splitting the inodes is what actually fixed it, and it
// keeps the strong guarantee that sync_file_range cannot give.

#include "../storage/storage.hpp"

#include <lmdb.h>

#ifdef HAVE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr size_t n_records = 500'000;
constexpr size_t batch_size = 1000;   // ops per durability flush
constexpr size_t value_bytes = 64;

struct record {
    static constexpr storage::tag_t tag = 1;
    uint64_t key;
    uint8_t payload[value_bytes - sizeof(uint64_t)];
};
static_assert(sizeof(record) == value_bytes);
static_assert(storage::tagged_aggregate<record>);

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void row(const char* engine, const char* phase, const char* durability, size_t ops, double s) {
    std::printf("%-14s %-16s %-12s %10zu ops  %8.3f s  %12.0f ops/sec\n",
                engine, phase, durability, ops, s, ops / s);
}

std::vector<uint64_t> shuffled_keys(size_t n, uint64_t seed) {
    std::vector<uint64_t> k(n);
    for (size_t i = 0; i < n; ++i) k[i] = i + 1;
    std::mt19937_64 rng(seed);
    std::shuffle(k.begin(), k.end(), rng);
    return k;
}

// =========================================================================
// Ours
// =========================================================================
using bench_spec = storage::storage_spec<storage::durability::local_sync, "kvbench_ours.db", 1ull << 30>;

void bench_ours(bool sync_per_batch) {
    std::remove("kvbench_ours.db");
    const char* dur = sync_per_batch ? "fsync/batch" : "no fsync";
    {
        storage::storage<bench_spec> table;

        record r{};
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < n_records; ++i) {
            r.key = i + 1;
            table.create(r);
            if (sync_per_batch && (i + 1) % batch_size == 0) table.sync();
        }
        if (sync_per_batch) table.sync();
        auto t1 = std::chrono::steady_clock::now();
        row("ours", "insert", dur, n_records, secs(t0, t1));

        auto keys = shuffled_keys(n_records, 1);
        uint64_t sink = 0;
        auto t2 = std::chrono::steady_clock::now();
        for (auto k : keys) {
            const record* got = table.get<record>(k);
            if (got) sink += got->key;
        }
        auto t3 = std::chrono::steady_clock::now();
        row("ours", "random read", "n/a", n_records, secs(t2, t3));
        if (sink == 0) std::printf("  (sink=%llu)\n", (unsigned long long)sink);

        auto t4 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < keys.size(); ++i) {
            table.update<&record::key>(keys[i], keys[i]);
            if (sync_per_batch && (i + 1) % batch_size == 0) table.sync();
        }
        if (sync_per_batch) table.sync();
        auto t5 = std::chrono::steady_clock::now();
        row("ours", "random update", dur, n_records, secs(t4, t5));
    }
    std::remove("kvbench_ours.db");
}

// =========================================================================
// LMDB
// =========================================================================
#define MCHECK(call) do { int rc_ = (call); if (rc_ != MDB_SUCCESS) { \
    std::fprintf(stderr, "lmdb error %s: %s\n", #call, mdb_strerror(rc_)); std::exit(1); } } while (0)

void bench_lmdb(bool sync_per_batch) {
    std::remove("kvbench_lmdb.mdb");
    std::remove("kvbench_lmdb.mdb-lock");
    const char* dur = sync_per_batch ? "fsync/batch" : "no fsync";

    MDB_env* env;
    MCHECK(mdb_env_create(&env));
    MCHECK(mdb_env_set_mapsize(env, 2ull << 30));
    unsigned flags = MDB_NOSUBDIR;
    if (!sync_per_batch) flags |= MDB_NOSYNC | MDB_NOMETASYNC;
    MCHECK(mdb_env_open(env, "kvbench_lmdb.mdb", flags, 0664));

    MDB_dbi dbi;
    {
        MDB_txn* txn;
        MCHECK(mdb_txn_begin(env, nullptr, 0, &txn));
        MCHECK(mdb_dbi_open(txn, nullptr, 0, &dbi));
        MCHECK(mdb_txn_commit(txn));
    }

    record r{};
    // Insert, committing a transaction every batch_size puts - the same
    // granularity our sync-per-batch path uses, so the durability label
    // means the same thing on both rows.
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n_records; i += batch_size) {
        MDB_txn* txn;
        MCHECK(mdb_txn_begin(env, nullptr, 0, &txn));
        size_t end = std::min(i + batch_size, n_records);
        for (size_t j = i; j < end; ++j) {
            uint64_t key = j + 1;
            r.key = key;
            MDB_val k{sizeof(key), &key};
            MDB_val v{sizeof(r), &r};
            MCHECK(mdb_put(txn, dbi, &k, &v, 0));
        }
        MCHECK(mdb_txn_commit(txn));
    }
    auto t1 = std::chrono::steady_clock::now();
    row("lmdb", "insert", dur, n_records, secs(t0, t1));

    auto keys = shuffled_keys(n_records, 1);
    uint64_t sink = 0;
    auto t2 = std::chrono::steady_clock::now();
    {
        MDB_txn* txn;
        MCHECK(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn));
        for (auto key : keys) {
            MDB_val k{sizeof(key), &key}, v{};
            if (mdb_get(txn, dbi, &k, &v) == MDB_SUCCESS) sink += ((const record*)v.mv_data)->key;
        }
        mdb_txn_abort(txn);
    }
    auto t3 = std::chrono::steady_clock::now();
    row("lmdb", "random read", "n/a", n_records, secs(t2, t3));
    if (sink == 0) std::printf("  (sink=%llu)\n", (unsigned long long)sink);

    auto t4 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < keys.size(); i += batch_size) {
        MDB_txn* txn;
        MCHECK(mdb_txn_begin(env, nullptr, 0, &txn));
        size_t end = std::min(i + batch_size, keys.size());
        for (size_t j = i; j < end; ++j) {
            uint64_t key = keys[j];
            r.key = key;
            MDB_val k{sizeof(key), &key};
            MDB_val v{sizeof(r), &r};
            MCHECK(mdb_put(txn, dbi, &k, &v, 0));
        }
        MCHECK(mdb_txn_commit(txn));
    }
    auto t5 = std::chrono::steady_clock::now();
    row("lmdb", "random update", dur, n_records, secs(t4, t5));

    mdb_env_close(env);
    std::remove("kvbench_lmdb.mdb");
    std::remove("kvbench_lmdb.mdb-lock");
}

// =========================================================================
// RocksDB (optional)
// =========================================================================
#ifdef HAVE_ROCKSDB
void bench_rocksdb(bool sync_per_batch) {
    std::string path = "kvbench_rocks";
    rocksdb::DestroyDB(path, rocksdb::Options());
    const char* dur = sync_per_batch ? "fsync/batch" : "no fsync";

    rocksdb::DB* db = nullptr;
    rocksdb::Options opts;
    opts.create_if_missing = true;
    auto st = rocksdb::DB::Open(opts, path, &db);
    if (!st.ok()) { std::fprintf(stderr, "rocksdb open: %s\n", st.ToString().c_str()); std::exit(1); }

    rocksdb::WriteOptions wo;
    wo.sync = sync_per_batch; // sync at batch commit, matching the others

    record r{};
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n_records; i += batch_size) {
        rocksdb::WriteBatch batch;
        size_t end = std::min(i + batch_size, n_records);
        for (size_t j = i; j < end; ++j) {
            uint64_t key = j + 1;
            r.key = key;
            batch.Put(rocksdb::Slice((const char*)&key, sizeof(key)),
                      rocksdb::Slice((const char*)&r, sizeof(r)));
        }
        db->Write(wo, &batch);
    }
    auto t1 = std::chrono::steady_clock::now();
    row("rocksdb", "insert", dur, n_records, secs(t0, t1));

    auto keys = shuffled_keys(n_records, 1);
    uint64_t sink = 0;
    auto t2 = std::chrono::steady_clock::now();
    for (auto key : keys) {
        std::string val;
        if (db->Get(rocksdb::ReadOptions(), rocksdb::Slice((const char*)&key, sizeof(key)), &val).ok())
            sink += ((const record*)val.data())->key;
    }
    auto t3 = std::chrono::steady_clock::now();
    row("rocksdb", "random read", "n/a", n_records, secs(t2, t3));
    if (sink == 0) std::printf("  (sink=%llu)\n", (unsigned long long)sink);

    auto t4 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < keys.size(); i += batch_size) {
        rocksdb::WriteBatch batch;
        size_t end = std::min(i + batch_size, keys.size());
        for (size_t j = i; j < end; ++j) {
            uint64_t key = keys[j];
            r.key = key;
            batch.Put(rocksdb::Slice((const char*)&key, sizeof(key)),
                      rocksdb::Slice((const char*)&r, sizeof(r)));
        }
        db->Write(wo, &batch);
    }
    auto t5 = std::chrono::steady_clock::now();
    row("rocksdb", "random update", dur, n_records, secs(t4, t5));

    delete db;
    rocksdb::DestroyDB(path, rocksdb::Options());
}
#endif

} // namespace

int main() {
    std::printf("records=%zu  value=%zu bytes  batch=%zu ops per durability flush\n\n",
                n_records, value_bytes, batch_size);
    std::printf("%-14s %-16s %-12s %10s      %8s  %12s\n", "engine", "phase", "durability", "ops", "time", "throughput");
    std::printf("%s\n", std::string(88, '-').c_str());

    for (bool sync : {false, true}) {
        std::printf("\n--- durability: %s ---\n", sync ? "fsync per batch" : "no fsync");
        bench_ours(sync);
        bench_lmdb(sync);
#ifdef HAVE_ROCKSDB
        bench_rocksdb(sync);
#else
        if (sync) std::printf("%-14s (not built - rebuild with -DHAVE_ROCKSDB -lrocksdb)\n", "rocksdb");
#endif
    }
    return 0;
}
