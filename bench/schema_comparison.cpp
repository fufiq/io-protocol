// schema_comparison.cpp - the three engines doing the SAME schema work.
//
// bench/kv_comparison.cpp compares the bare engine against LMDB and RocksDB
// doing bare key/value puts. That stopped being the interesting question once
// bench/app_layer.hpp existed: our engine now carries a secondary index, a
// UNIQUE constraint and a transaction on every write, and the other two were
// still doing a plain put. Beating them at less work proves nothing.
//
// So here every engine performs the identical logical operation:
//
//   "insert a customer record and its unique email -> id index entry,
//    atomically, rejecting a duplicate email"
//
// which each one expresses in its own idiom:
//
//   ours     the app_layer observer maintains the index; app_txn checks
//            UNIQUE and writes a commit marker
//   lmdb     two named DBs in one env, both written inside one mdb_txn -
//            LMDB gives real ACID across them
//   rocksdb  two column families, both written in one WriteBatch, which
//            RocksDB applies atomically
//
// THE ASYMMETRY THAT MATTERS, stated up front because it is the whole result:
// their index is ON DISK and survives a restart. Ours is in memory and is
// rebuilt by replaying the log. We are not doing the same amount of work - we
// are moving that work from write time to start-up time. So the reopen phase
// at the bottom is not a footnote, it is the other half of the comparison.
//
// MEASURED, NVMe, gcc -O3, one logical transaction per operation.
//
// NO FSYNC, 200k records (ops/sec, and how many times slower than ours):
//
//   phase                      ours        lmdb              rocksdb
//   insert + unique index    3846597    100887   38x         127164   30x
//   read by primary id      49904757   4724059   11x         581371   86x
//   read by secondary key   85259451   5040112   17x         587696  145x
//   update non-indexed       6090834    131105   46x         158918   38x
//   update INDEXED field     6384116     69277   92x          90225   71x
//   delete + index entry     8656813     54509  159x         111360   78x
//   REOPEN                    0.057 s    0.001 s              3.873 s
//
// FSYNC PER TRANSACTION, 20k records:
//
//   phase                      ours        lmdb              rocksdb
//   insert + unique index       1215       520  2.3x            1208  1.0x
//   update non-indexed          1170       557  2.1x            1181  1.0x
//   update INDEXED field        1140       508  2.2x            1117  1.0x
//   delete + index entry        1138       501  2.3x             440  0.4x
//   REOPEN                    0.006 s    0.000 s              0.361 s
//
// THE ANSWER, in three parts, because there is no single one:
//
// 1. WITHOUT DURABILITY WE WIN EVERYTHING, by 30x to 160x. That is not a
//    tuning result, it is an architectural one: our index lookup is a hash
//    probe in RAM and our write is an append plus a pointer store, while both
//    of theirs is a B+tree descent or an LSM write path plus a persistent
//    index update. The widest gaps are exactly where their index has to be
//    modified on disk - updating the INDEXED field, and delete.
//
// 2. WITH DURABILITY THE ANSWER IS "NOBODY". At one fsync per transaction all
//    three land at 1100-1200 ops/sec, which is this device's commit latency
//    and nothing else. We tie RocksDB exactly. LMDB is 2.2x behind because its
//    commit writes more (data pages, then the meta page, with its own barrier).
//    Above roughly a millisecond per transaction, the engine stops mattering
//    and only batching does.
//
// 3. REOPEN IS WHERE THE ASYMMETRY SHOWS, and it does not go the way the
//    trade-off suggests. Our index is not on disk, so we replay 56.5 MB of log
//    to rebuild it: 0.057 s. LMDB does not rebuild anything - its B+tree IS
//    the file, so it opens in 0.001 s regardless of size, and that is a real
//    architectural advantage we do not have. But RocksDB takes 3.873 s, 68x
//    longer than us, because it must replay a WAL nothing had flushed. So
//    "they persist the index, we rebuild it" costs us against LMDB and wins
//    against RocksDB.
//
// FOUR CAVEATS, all of which cut against us:
//
// * One transaction per operation is the worst case for LMDB and RocksDB.
//   Both are built for batched writes and would close much of the no-fsync gap
//   if allowed to batch. Ours would improve too, but less: our per-operation
//   cost is already small.
// * RocksDB runs on defaults here - no bloom filters on the column families,
//   no block cache sizing, no compaction tuning. Its read numbers in
//   particular are "out of the box", not "as fast as it goes".
// * Our index lives in RAM and is bounded by it. Theirs is on disk and is not.
//   At a dataset that does not fit in memory this comparison stops being
//   meaningful in our favour.
// * LMDB gives multi-process readers with real MVCC, and RocksDB gives
//   concurrent writers, range scans and compaction. We give one writer in one
//   process. That is most of why we are faster.
//
// Build:
//   g++ -std=c++23 -O3 -march=native -o schema_comparison bench/schema_comparison.cpp -llmdb -lrocksdb -DHAVE_ROCKSDB
//   ./schema_comparison [ours|lmdb|rocksdb|all] [nosync|sync|both] [n_records]
#include "app_txn.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <lmdb.h>

#ifdef HAVE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#endif

using namespace app;

namespace {

    // Runtime, not constant: the durable runs do one fsync per operation, so
    // the same count that takes seconds without durability takes hours with it.
    std::size_t n_records = 200'000;

    using bench_spec = storage::storage_spec<storage::durability::local_sync, "schemacmp_ours.db", 2ull << 30>;

    double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }

    void line(const char* engine, const char* phase, const char* durability, std::size_t ops, double s) {
        std::printf("%-9s %-26s %-12s %9zu ops  %8.3f s  %12.0f ops/sec\n",
                    engine, phase, durability, ops, s, ops / s);
    }

    void wipe_ours() {
        std::remove("schemacmp_ours.db");
        std::remove("schemacmp_ours.db.data");
    }
    void wipe_lmdb() {
        std::remove("schemacmp_lmdb.mdb");
        std::remove("schemacmp_lmdb.mdb-lock");
    }
#ifdef HAVE_ROCKSDB
    void wipe_rocks() { (void)std::system("rm -rf schemacmp_rocks"); }
#endif

    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };

    // =================================================================
    // ours
    // =================================================================
    void bench_ours(bool durable) {
        const char* d = durable ? "fsync/txn" : "no fsync";
        wipe_ours();
        std::vector<record_id> ids;
        ids.reserve(n_records);
        double reopen_seconds = 0;
        std::size_t log_bytes = 0;

        {
            table_t<bench_spec> table;
            uint64_t txn_id = 1;

            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn<bench_spec> tx(table, txn_id++);
                ids.push_back(tx.insert_customer(customer{ (uint64_t)i + 1, 1'000'000, (uint32_t)(i % 40), 0 }));
                tx.commit(durable);
            }
            auto t1 = std::chrono::steady_clock::now();
            line("ours", "insert + unique index", d, n_records, secs(t0, t1));

            xorshift rng(5);
            volatile int64_t sink = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) sink = table.get<customer>(ids[rng.range(n_records)])->balance;
            t1 = std::chrono::steady_clock::now();
            line("ours", "read by primary id", "n/a", n_records, secs(t0, t1));

            volatile record_id id_sink = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) id_sink = find_customer_by_email(table, rng.range(n_records) + 1);
            t1 = std::chrono::steady_clock::now();
            line("ours", "read by secondary key", "n/a", n_records, secs(t0, t1));
            (void)sink; (void)id_sink;

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn<bench_spec> tx(table, txn_id++);
                tx.update<&customer::balance>(ids[i], (int64_t)i);
                tx.commit(durable);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours", "update non-indexed field", d, n_records, secs(t0, t1));

            // The expensive shape: the indexed column changes, so the index
            // entry has to be retracted and re-inserted.
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn<bench_spec> tx(table, txn_id++);
                tx.update<&customer::email_hash>(ids[i], (uint64_t)(i + 1 + n_records));
                tx.commit(durable);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours", "update INDEXED field", d, n_records, secs(t0, t1));

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn<bench_spec> tx(table, txn_id++);
                tx.delete_customer(ids[i]);
                tx.commit(durable);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours", "delete + index entry", d, n_records, secs(t0, t1));

            table.sync();
            log_bytes = table.current_offset();
        }
        {
            // Reopen: the index does not exist on disk, so this is where we
            // pay for everything the other two paid for at write time.
            table_t<bench_spec> restored;
            auto t0 = std::chrono::steady_clock::now();
            restored.recover(bench_spec::capacity);
            auto t1 = std::chrono::steady_clock::now();
            reopen_seconds = secs(t0, t1);
        }
        std::printf("%-9s %-26s %-12s %9.1f MB %8.3f s  %12.0f MB/sec\n",
                    "ours", "REOPEN (rebuild index)", d, log_bytes / 1048576.0, reopen_seconds,
                    log_bytes / 1048576.0 / reopen_seconds);
        wipe_ours();
    }

    // =================================================================
    // lmdb - two named DBs in one env, one transaction spanning both
    // =================================================================
    void bench_lmdb(bool durable) {
        const char* d = durable ? "fsync/txn" : "no fsync";
        wipe_lmdb();

        MDB_env* env = nullptr;
        mdb_env_create(&env);
        mdb_env_set_maxdbs(env, 4);
        mdb_env_set_mapsize(env, 4ull << 30);
        unsigned flags = MDB_NOSUBDIR;
        if (!durable) flags |= MDB_NOSYNC | MDB_NOMETASYNC;
        if (mdb_env_open(env, "schemacmp_lmdb.mdb", flags, 0664) != 0) { std::printf("lmdb: open failed\n"); return; }

        MDB_dbi dbi_main, dbi_idx;
        {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            mdb_dbi_open(t, "main", MDB_CREATE | MDB_INTEGERKEY, &dbi_main);
            mdb_dbi_open(t, "by_email", MDB_CREATE | MDB_INTEGERKEY, &dbi_idx);
            mdb_txn_commit(t);
        }

        std::vector<uint64_t> ids(n_records);
        for (std::size_t i = 0; i < n_records; ++i) ids[i] = i + 1;

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            customer c{ (uint64_t)i + 1, 1'000'000, (uint32_t)(i % 40), 0 };
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            MDB_val ek{ sizeof(uint64_t), &c.email_hash }, found{};
            if (mdb_get(t, dbi_idx, &ek, &found) == 0) { mdb_txn_abort(t); continue; } // UNIQUE
            MDB_val k{ sizeof(uint64_t), &ids[i] }, v{ sizeof(customer), &c };
            mdb_put(t, dbi_main, &k, &v, 0);
            MDB_val iv{ sizeof(uint64_t), &ids[i] };
            mdb_put(t, dbi_idx, &ek, &iv, 0);
            mdb_txn_commit(t);
        }
        auto t1 = std::chrono::steady_clock::now();
        line("lmdb", "insert + unique index", d, n_records, secs(t0, t1));

        xorshift rng(5);
        volatile int64_t sink = 0;
        {
            MDB_txn* rt = nullptr;
            mdb_txn_begin(env, nullptr, MDB_RDONLY, &rt);
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                uint64_t key = ids[rng.range(n_records)];
                MDB_val k{ sizeof(key), &key }, v{};
                if (mdb_get(rt, dbi_main, &k, &v) == 0) sink = ((const customer*)v.mv_data)->balance;
            }
            t1 = std::chrono::steady_clock::now();
            line("lmdb", "read by primary id", "n/a", n_records, secs(t0, t1));

            volatile uint64_t id_sink = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                uint64_t email = rng.range(n_records) + 1;
                MDB_val k{ sizeof(email), &email }, v{};
                if (mdb_get(rt, dbi_idx, &k, &v) == 0) id_sink = *(const uint64_t*)v.mv_data;
            }
            t1 = std::chrono::steady_clock::now();
            line("lmdb", "read by secondary key", "n/a", n_records, secs(t0, t1));
            mdb_txn_abort(rt);
            (void)id_sink;
        }
        (void)sink;

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            MDB_val k{ sizeof(uint64_t), &ids[i] }, v{};
            if (mdb_get(t, dbi_main, &k, &v) == 0) {
                customer c = *(const customer*)v.mv_data;
                c.balance = (int64_t)i;
                MDB_val nv{ sizeof(customer), &c };
                mdb_put(t, dbi_main, &k, &nv, 0);
            }
            mdb_txn_commit(t);
        }
        t1 = std::chrono::steady_clock::now();
        line("lmdb", "update non-indexed field", d, n_records, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            MDB_val k{ sizeof(uint64_t), &ids[i] }, v{};
            if (mdb_get(t, dbi_main, &k, &v) == 0) {
                customer c = *(const customer*)v.mv_data;
                MDB_val old_key{ sizeof(uint64_t), &c.email_hash };
                mdb_del(t, dbi_idx, &old_key, nullptr);          // retract
                c.email_hash = (uint64_t)(i + 1 + n_records);
                MDB_val nv{ sizeof(customer), &c };
                mdb_put(t, dbi_main, &k, &nv, 0);
                MDB_val new_key{ sizeof(uint64_t), &c.email_hash }, iv{ sizeof(uint64_t), &ids[i] };
                mdb_put(t, dbi_idx, &new_key, &iv, 0);           // re-insert
            }
            mdb_txn_commit(t);
        }
        t1 = std::chrono::steady_clock::now();
        line("lmdb", "update INDEXED field", d, n_records, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            MDB_val k{ sizeof(uint64_t), &ids[i] }, v{};
            if (mdb_get(t, dbi_main, &k, &v) == 0) {
                customer c = *(const customer*)v.mv_data;
                MDB_val ek{ sizeof(uint64_t), &c.email_hash };
                mdb_del(t, dbi_idx, &ek, nullptr);
                mdb_del(t, dbi_main, &k, nullptr);
            }
            mdb_txn_commit(t);
        }
        t1 = std::chrono::steady_clock::now();
        line("lmdb", "delete + index entry", d, n_records, secs(t0, t1));

        mdb_env_close(env);

        struct ::stat st{};
        ::stat("schemacmp_lmdb.mdb", &st);
        t0 = std::chrono::steady_clock::now();
        MDB_env* reopened = nullptr;
        mdb_env_create(&reopened);
        mdb_env_set_maxdbs(reopened, 4);
        mdb_env_set_mapsize(reopened, 4ull << 30);
        mdb_env_open(reopened, "schemacmp_lmdb.mdb", MDB_NOSUBDIR, 0664);
        {   // touch both DBs so the reopen is comparable: the index must be usable
            MDB_txn* t = nullptr; MDB_dbi a, b;
            mdb_txn_begin(reopened, nullptr, MDB_RDONLY, &t);
            mdb_dbi_open(t, "main", MDB_INTEGERKEY, &a);
            mdb_dbi_open(t, "by_email", MDB_INTEGERKEY, &b);
            mdb_txn_abort(t);
        }
        t1 = std::chrono::steady_clock::now();
        std::printf("%-9s %-26s %-12s %9.1f MB %8.3f s  %12s\n",
                    "lmdb", "REOPEN (index on disk)", d, st.st_size / 1048576.0, secs(t0, t1), "-");
        mdb_env_close(reopened);
        wipe_lmdb();
    }

#ifdef HAVE_ROCKSDB
    // =================================================================
    // rocksdb - two column families, one WriteBatch spanning both
    // =================================================================
    void bench_rocksdb(bool durable) {
        const char* d = durable ? "fsync/txn" : "no fsync";
        wipe_rocks();

        rocksdb::DB* db = nullptr;
        rocksdb::Options options;
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        std::vector<rocksdb::ColumnFamilyDescriptor> cfs = {
            { rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions() },
            { "by_email", rocksdb::ColumnFamilyOptions() },
        };
        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        auto status = rocksdb::DB::Open(rocksdb::DBOptions(options), "schemacmp_rocks", cfs, &handles, &db);
        if (!status.ok()) { std::printf("rocksdb: open failed: %s\n", status.ToString().c_str()); return; }

        rocksdb::WriteOptions wo;
        wo.sync = durable;
        rocksdb::ReadOptions ro;

        // NOT a key_of(uint64_t) helper returning a Slice: the parameter is a
        // local, so the Slice would point at a dead stack slot the moment it
        // returned. That bug made the durable insert phase report 4M ops/sec
        // - the UNIQUE probe was reading garbage, matching nothing or
        // everything at random, and the writes were being skipped. Keys are
        // spelled out as named locals here so each one outlives its Slice.
        auto slice_of = [](const uint64_t& v) { return rocksdb::Slice((const char*)&v, sizeof(v)); };
        std::vector<uint64_t> ids(n_records);
        for (std::size_t i = 0; i < n_records; ++i) ids[i] = i + 1;

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            customer c{ (uint64_t)i + 1, 1'000'000, (uint32_t)(i % 40), 0 };
            const uint64_t id = ids[i], email = c.email_hash;
            std::string found;
            if (db->Get(ro, handles[1], slice_of(email), &found).ok()) continue; // UNIQUE
            rocksdb::WriteBatch batch;
            batch.Put(handles[0], slice_of(id), rocksdb::Slice((const char*)&c, sizeof(c)));
            batch.Put(handles[1], slice_of(email), slice_of(id));
            db->Write(wo, &batch);
        }
        auto t1 = std::chrono::steady_clock::now();
        line("rocksdb", "insert + unique index", d, n_records, secs(t0, t1));

        xorshift rng(5);
        volatile int64_t sink = 0;
        std::string value;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            const uint64_t id = ids[rng.range(n_records)];
            if (db->Get(ro, handles[0], slice_of(id), &value).ok())
                sink = ((const customer*)value.data())->balance;
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "read by primary id", "n/a", n_records, secs(t0, t1));

        volatile uint64_t id_sink = 0;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            const uint64_t email = rng.range(n_records) + 1;
            if (db->Get(ro, handles[1], slice_of(email), &value).ok())
                id_sink = *(const uint64_t*)value.data();
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "read by secondary key", "n/a", n_records, secs(t0, t1));
        (void)sink; (void)id_sink;

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            const uint64_t id = ids[i];
            if (!db->Get(ro, handles[0], slice_of(id), &value).ok()) continue;
            customer c = *(const customer*)value.data();
            c.balance = (int64_t)i;
            rocksdb::WriteBatch batch;
            batch.Put(handles[0], slice_of(id), rocksdb::Slice((const char*)&c, sizeof(c)));
            db->Write(wo, &batch);
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "update non-indexed field", d, n_records, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            const uint64_t id = ids[i];
            if (!db->Get(ro, handles[0], slice_of(id), &value).ok()) continue;
            customer c = *(const customer*)value.data();
            const uint64_t old_email = c.email_hash;
            c.email_hash = (uint64_t)(i + 1 + n_records);
            const uint64_t new_email = c.email_hash;
            rocksdb::WriteBatch batch;
            batch.Delete(handles[1], slice_of(old_email));                 // retract
            batch.Put(handles[0], slice_of(id), rocksdb::Slice((const char*)&c, sizeof(c)));
            batch.Put(handles[1], slice_of(new_email), slice_of(id));      // re-insert
            db->Write(wo, &batch);
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "update INDEXED field", d, n_records, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_records; ++i) {
            const uint64_t id = ids[i];
            if (!db->Get(ro, handles[0], slice_of(id), &value).ok()) continue;
            const customer c = *(const customer*)value.data();
            const uint64_t email = c.email_hash;
            rocksdb::WriteBatch batch;
            batch.Delete(handles[1], slice_of(email));
            batch.Delete(handles[0], slice_of(id));
            db->Write(wo, &batch);
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "delete + index entry", d, n_records, secs(t0, t1));

        for (auto* h : handles) db->DestroyColumnFamilyHandle(h);
        delete db;

        t0 = std::chrono::steady_clock::now();
        rocksdb::DB* reopened = nullptr;
        std::vector<rocksdb::ColumnFamilyHandle*> h2;
        rocksdb::DB::Open(rocksdb::DBOptions(options), "schemacmp_rocks", cfs, &h2, &reopened);
        t1 = std::chrono::steady_clock::now();
        std::printf("%-9s %-26s %-12s %9s %8.3f s  %12s\n",
                    "rocksdb", "REOPEN (index on disk)", d, "-", secs(t0, t1), "-");
        for (auto* h : h2) reopened->DestroyColumnFamilyHandle(h);
        delete reopened;
        wipe_rocks();
    }
#endif

} // namespace

int main(int argc, char** argv) {
    const std::string engine = argc > 1 ? argv[1] : "all";
    const std::string mode = argc > 2 ? argv[2] : "both";
    if (argc > 3) n_records = (std::size_t)std::strtoull(argv[3], nullptr, 10);

    std::printf("records=%zu   one logical transaction per operation, unique secondary index on email\n\n", n_records);
    std::printf("%-9s %-26s %-12s %9s      %8s  %12s\n", "engine", "phase", "durability", "ops", "time", "throughput");
    std::printf("%s\n", std::string(96, '-').c_str());

    for (bool sync : { false, true }) {
        if (mode == "nosync" && sync) continue;
        if (mode == "sync" && !sync) continue;
        std::printf("\n--- durability: %s ---\n", sync ? "fsync per transaction" : "no fsync");
        if (engine == "all" || engine == "ours") bench_ours(sync);
        if (engine == "all" || engine == "lmdb") bench_lmdb(sync);
#ifdef HAVE_ROCKSDB
        if (engine == "all" || engine == "rocksdb") bench_rocksdb(sync);
#endif
    }
    return 0;
}
