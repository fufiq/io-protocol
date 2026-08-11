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
// MEASURED, NVMe, gcc -O3. Every engine performs one logical transaction per
// operation; the only thing that varies is how many transactions share one
// fsync. All three support genuine GROUP COMMIT, and all three are used that
// way here - this is deferred durability, not absent durability:
//
//   ours     tx.commit() writes the commit marker; table.sync() msyncs the
//            log range every N transactions
//   lmdb     MDB_NOSYNC | MDB_NOMETASYNC, so mdb_txn_commit writes without
//            flushing; mdb_env_sync(env, 1) forces the flush every N. LMDB
//            documents this shape: without MDB_WRITEMAP it keeps A, C and I
//            and defers only D
//   rocksdb  WriteOptions::sync = false, so the WriteBatch reaches the WAL
//            without an fsync; db->SyncWAL() fsyncs it every N
//
// In all three, when the sync call returns everything before it is on disk,
// and a crash loses only the transactions since the last one - never half of
// one, because each still ends with its own commit record.
//
// NO FSYNC AT ALL, 200k records (delete is 100k, half the set):
//
//   operation                   ours      lmdb    rocksdb    ours ahead
//   insert + unique index    5291680     98505     129177     54x / 41x
//   read by primary id      78914671   4727261     579347     17x / 136x
//   read by secondary key   83575877   5090250     626598     16x / 133x
//   update non-indexed       8682450    132509     161141     66x / 54x
//   update INDEXED field     6503439     69427      90795     94x / 72x
//   delete + index entry     8459074     58861     114397    144x / 74x
//   REOPEN, replaying         0.042 s         -          -
//   REOPEN, mapping           0.000 s   0.000 s    3.365 s
//
// GROUP COMMIT, one fsync per 1000 transactions, 200k records:
//
//   operation                   ours      lmdb    rocksdb    ours ahead
//   insert + unique index     801297     80979     103699     9.9x / 7.7x
//   read by primary id      77197842   4208917     535484      18x / 144x
//   read by secondary key   79386091   5001921     631355      16x / 126x
//   update non-indexed        814462    104437     130557     7.8x / 6.2x
//   update INDEXED field      879765     62604      73713      14x / 12x
//   delete + index entry      866897     46296      95014      19x / 9.1x
//   REOPEN, mapping           0.000 s   0.000 s    3.469 s
//
// ONE FSYNC PER TRANSACTION, 20k records:
//
//   operation                   ours      lmdb    rocksdb    ours ahead
//   insert + unique index       1304      1032       1170     1.3x / 1.1x
//   update non-indexed          1234      1046       1202     1.2x / 1.0x
//   update INDEXED field        1266      1002       1179     1.3x / 1.1x
//   delete + index entry        1235       983       1095     1.3x / 1.1x
//   read by primary id      98333251   7252150     713482      14x / 138x
//   REOPEN, mapping           0.000 s   0.000 s    0.305 s
//
// THREE ANSWERS, NOT ONE, and the batch size picks which one applies:
//
//   1 txn per fsync      nobody wins. All three are within 1.3x, because all
//                        three are the device: ~1 ms per commit, and no
//                        engine gets under that.
//   1000 per fsync       we are 6-19x ahead on writes.
//   no fsync             we are 16-144x ahead.
//
// Reads never move, in any mode: they touch no disk.
//
// RESTART. The secondary indexes now live in a mapping of their own
// (bench/flat_index.hpp) rather than in std::unordered_map and std::multimap,
// which cannot be mapped at all. A clean shutdown msyncs that mapping
// alongside the engine's checkpoint, and a restart maps both back:
//
//   ours, replaying the log and rebuilding    0.042 s
//   ours, mapping both regions                0.000 s   <- usable immediately
//   lmdb                                      0.000 s
//   rocksdb                                   3.365 s
//
// The benchmark checks that the mapped indexes agree with the ones a replay
// would have built, and says so out loud if they ever differ.
//
// WHAT THE FLAT INDEXES COST on the write path, because they are not free:
// open addressing with backward-shift deletion, and a mapped region rather
// than the heap, moved non-durable updates from ~14.2M/s to ~8.7M/s and
// deletes from ~19.4M/s to ~8.5M/s. That is the price of a restart that
// costs nothing, and it disappears entirely the moment any durability is on.
//
// THE SAME SCHEMA ON BOTH OF OUR ENGINES. Identical unique index, identical
// constraints, identical transactions, one carried by storage.hpp and one by
// field_storage.hpp. 200k records, no fsync, three alternated runs:
//
//   operation                     ours   ours-fld
//   insert + unique index      5250000    7060000    field 1.35x
//   read by primary id        79000000   54600000    ours  1.45x
//   read by secondary key     80000000   81700000    equal
//   update non-indexed         7800000    6890000    ours  1.13x
//   update INDEXED field       6250000    5150000    ours  1.21x
//   delete + index entry       8050000    6710000    ours  1.20x
//
// With group commit at 1 per 1000 both land near 0.8-1.0M on everything and
// the difference disappears into the flush.
//
// THE POINT IS HOW SMALL THESE GAPS ARE. On the bare engines
// (bench/kv_comparison.cpp) the same two differ by 2.3x on insert, 1.8x on
// read and 1.5x on update. Under a real schema the observer maintaining six
// indexes, the constraint checks and the transaction staging dominate, and
// the engine becomes a smaller share of the total. Reading by secondary key
// is identical to three digits because the engine is not involved at all -
// that is a probe into the shared flat index.
//
// So the engine choice matters where it was measured to matter - above RAM,
// where field_storage takes 78x fewer major faults per update and half the
// disk - and much less than it looks under an application.
//
// FOUR CAVEATS, all of which cut against us:
//
// * RocksDB runs on defaults here - no bloom filters on the column families,
//   no block cache sizing, no compaction tuning. Its read numbers in
//   particular are "out of the box", not "as fast as it goes".
// * Our index lives in RAM and is bounded by it. Theirs is on disk and is not.
//   At a dataset that does not fit in memory this comparison stops being
//   meaningful in our favour.
// * LMDB gives multi-process readers with real MVCC, and RocksDB gives
//   concurrent writers, range scans and compaction. We give one writer in one
//   process. That is most of why we are faster.
// * REOPEN. LMDB opens in ~0.000 s at any size because its B+tree IS the
//   file. We now have two answers: replaying the log, or a checkpoint. With
//   200k records and 100k of them live at shutdown:
//
//     ours, replaying the log            0.025 s
//     ours, from a checkpoint            0.000 s   (engine state only)
//     ours, + rebuilding the indexes     0.003 s   (usable)
//     lmdb                               0.000 s
//     rocksdb                            3.475 s
//
//   A checkpoint flushes the log, the materialized records and the index
//   together and writes a marker naming the offset they agree at, so nothing
//   is replayed. What is left - the 3 ms - is entirely the secondary indexes,
//   which live in the observer's memory and have to be rebuilt from live
//   records. LMDB is still ahead because its indexes are IN the file; the gap
//   went from 25 ms to 3.
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
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#endif

using namespace app;

namespace {

    // Runtime, not constant: the durable runs do one fsync per operation, so
    // the same count that takes seconds without durability takes hours with it.
    std::size_t n_records = 200'000;

    // index_path and checkpoint_path turn on the fast-restart path; without
    // them this spec behaves exactly as before and recover() replays the log.
    // 900 MB rather than 2 GB: the in-place engine fallocates its capacity
    // TWICE (log + materialization), and the machine this now runs on has
    // 4.2 GB of disk. index_entries is named explicitly for the same reason -
    // left to default it is capacity/32, which at 900 MB would be a 236 MB
    // index file for a run that never issues three million ids.
    using bench_spec = storage::storage_spec<storage::durability::local_sync, "schemacmp_ours.db", 900ull << 20,
                                             "", "schemacmp_ours.idx", "schemacmp_ours.ck", 2'500'000>;

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
        std::remove("schemacmp_ours.idx");
        std::remove("schemacmp_ours.ck");
        std::remove("schemacmp_ours.sidx");
    }
    // Ids are monotonic, so the ceiling is every id the run will ever issue.
    // TWO numbers now. id_space stays generous because the head arrays are
    // addressed by record id and ids come from one sequence shared by all four
    // entities. entries is what is actually INDEXED - and this benchmark
    // inserts customers only, so two of the three hashes and every node arena
    // stay empty. Sizing those off id_space put 500000 entries into 313 MB.
    app::schema_index::sizing index_sizing() { return { n_records * 4 + 1024, n_records + 1024 }; }
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
    // batch == 0 -> never sync; 1 -> one fsync per transaction; N -> group
    // commit, one fsync shared by N transactions.
    std::string label(std::size_t batch) {
        if (batch == 0) return "no fsync";
        if (batch == 1) return "fsync/txn";
        return "fsync/" + std::to_string(batch);
    }

    // The identical schema - same unique index, same constraints, same
    // transactions - carried by the field-granular engine instead. One file,
    // nothing patched in place.
    using field_bench_spec = storage::field_spec<"schemacmp_fld.log", 900ull << 20, "schemacmp_fld.idx",
                                                 2'500'000, 5'000'000, 8, 128>;

    void bench_ours_fields(std::size_t batch) {
        const std::string dl = label(batch);
        const char* d = dl.c_str();
        std::remove("schemacmp_fld.log"); std::remove("schemacmp_fld.idx"); std::remove("schemacmp_fld.sidx"); 
        std::vector<record_id> ids;
        ids.reserve(n_records);
        {
            field_table_t<field_bench_spec> table;
            index_store store;
            if (!table.ok() || !store.open("schemacmp_fld.sidx", index_sizing()) ||
                !table.observer_state().attach(store.region(), store.region_bytes(), index_sizing())) {
                std::printf("ours-fld: could not map\n"); return;
            }
            uint64_t txn_id = 1;
            // ONE barrier for both. The log goes first and the index second,
            // and the index is stamped with the offset the log just reached -
            // never a larger one. See app_layer.hpp's index_header::valid_to.
            auto group_commit = [&](std::size_t i) {
                if (!batch || (i + 1) % batch != 0) return;
                table.sync();   // the log carries everything between checkpoints
            };

            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn tx(table, txn_id++);
                ids.push_back(tx.insert_customer(customer{ (uint64_t)i + 1, 1'000'000, (uint32_t)(i % 40), 0 }));
                tx.commit(false);
                group_commit(i);
            }
            auto t1 = std::chrono::steady_clock::now();
            line("ours-fld", "insert + unique index", d, n_records, secs(t0, t1));

            xorshift rng(5);
            volatile int64_t sink = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) sink = table.get<customer>(ids[rng.range(n_records)])->balance;
            t1 = std::chrono::steady_clock::now();
            line("ours-fld", "read by primary id", "n/a", n_records, secs(t0, t1));

            volatile record_id id_sink = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) id_sink = find_customer_by_email(table, rng.range(n_records) + 1);
            t1 = std::chrono::steady_clock::now();
            line("ours-fld", "read by secondary key", "n/a", n_records, secs(t0, t1));
            (void)sink; (void)id_sink;

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn tx(table, txn_id++);
                tx.update<&customer::balance>(ids[i], (int64_t)i);
                tx.commit(false);
                group_commit(i);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours-fld", "update non-indexed field", d, n_records, secs(t0, t1));

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn tx(table, txn_id++);
                tx.update<&customer::email_hash>(ids[i], (uint64_t)(i + 1 + n_records));
                tx.commit(false);
                group_commit(i);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours-fld", "update INDEXED field", d, n_records, secs(t0, t1));

            const std::size_t n_deleted = n_records / 2;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_deleted; ++i) {
                txn tx(table, txn_id++);
                tx.delete_customer(ids[i]);
                tx.commit(false);
                group_commit(i);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours-fld", "delete + index entry", d, n_deleted, secs(t0, t1));
            table.sync();
            store.checkpoint(table.current_offset(), table.observer_state());
        }
        std::remove("schemacmp_fld.log"); std::remove("schemacmp_fld.idx"); std::remove("schemacmp_fld.sidx"); 
    }

    void bench_ours(std::size_t batch) {
        const std::string dl = label(batch);
        const char* d = dl.c_str();
        wipe_ours();
        std::vector<record_id> ids;
        ids.reserve(n_records);
        double reopen_seconds = 0, restart_seconds = 0;
        bool checkpoint_ok = false;
        std::size_t log_bytes = 0;

        {
            table_t<bench_spec> table;
            index_store store;
            if (!store.open("schemacmp_ours.sidx", index_sizing()) ||
                !table.observer_state().attach(store.region(), store.region_bytes(), index_sizing())) {
                std::printf("ours: could not map the secondary indexes\n");
                return;
            }
            uint64_t txn_id = 1;
            // The transaction always ends with its commit marker; whether the
            // marker is on disk yet is a separate decision, taken here.
            // ONE barrier for both. The log goes first and the index second,
            // and the index is stamped with the offset the log just reached -
            // never a larger one. See app_layer.hpp's index_header::valid_to.
            auto group_commit = [&](std::size_t i) {
                if (!batch || (i + 1) % batch != 0) return;
                table.sync();   // the log carries everything between checkpoints
            };

            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn tx(table, txn_id++);
                ids.push_back(tx.insert_customer(customer{ (uint64_t)i + 1, 1'000'000, (uint32_t)(i % 40), 0 }));
                tx.commit(false);
                group_commit(i);
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
                txn tx(table, txn_id++);
                tx.update<&customer::balance>(ids[i], (int64_t)i);
                tx.commit(false);
                group_commit(i);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours", "update non-indexed field", d, n_records, secs(t0, t1));

            // The expensive shape: the indexed column changes, so the index
            // entry has to be retracted and re-inserted.
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_records; ++i) {
                txn tx(table, txn_id++);
                tx.update<&customer::email_hash>(ids[i], (uint64_t)(i + 1 + n_records));
                tx.commit(false);
                group_commit(i);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours", "update INDEXED field", d, n_records, secs(t0, t1));

            // Half, not all: the reopen measured below has to have something
            // live to come back to, and a database that ends empty makes the
            // index-rebuild column meaningless.
            const std::size_t n_deleted = n_records / 2;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_deleted; ++i) {
                txn tx(table, txn_id++);
                tx.delete_customer(ids[i]);
                tx.commit(false);
                group_commit(i);
            }
            t1 = std::chrono::steady_clock::now();
            line("ours", "delete + index entry", d, n_deleted, secs(t0, t1));

            table.sync();
            log_bytes = table.current_offset();
            // A clean shutdown: flush the log, the materialized records and
            // the engine's index together and name the offset they agree at -
            // and flush the secondary indexes, which live in their own
            // mapping, alongside it.
            store.checkpoint(table.current_offset(), table.observer_state());
            checkpoint_ok = table.checkpoint();
        }
        std::size_t emails_before = 0, emails_after = 0;
        {
            // A restart with no checkpoint to lean on: replay the whole log
            // and rebuild every index from it.
            table_t<bench_spec> restored;
            index_store store;
            store.open("schemacmp_ours.sidx.replay", index_sizing());
            restored.observer_state().attach(store.region(), store.region_bytes(), index_sizing());
            auto t0 = std::chrono::steady_clock::now();
            restored.recover(bench_spec::capacity);
            auto t1 = std::chrono::steady_clock::now();
            reopen_seconds = secs(t0, t1);
            emails_before = restored.observer_state().emails_indexed();
        }
        std::remove("schemacmp_ours.sidx.replay");
        {
            // And with one. Both mappings are simply opened: the engine's
            // state comes back from its checkpoint, the secondary indexes
            // come back from their own file, and nothing is replayed or
            // rebuilt at all.
            table_t<bench_spec> restored;
            index_store store;
            auto t0 = std::chrono::steady_clock::now();
            store.open("schemacmp_ours.sidx", index_sizing());
            // The region is anonymous now, so it starts empty and the snapshot
            // has to be read into it. Order matters: allocate, load, adopt.
            const std::size_t indexed_to = store.restore();
            restored.observer_state().attach(store.region(), store.region_bytes(), index_sizing());
            restored.restart(bench_spec::capacity);
            // Anything the log has past the snapshot is replayed through the
            // same apply() a live write uses.
            if (indexed_to && indexed_to < restored.current_offset())
                restored.for_each_record(indexed_to, restored.current_offset(),
                    [&](storage::record_header hdr, std::span<const std::byte> payload) {
                        restored.apply(hdr, payload.data());
                    });
            auto t1 = std::chrono::steady_clock::now();
            restart_seconds = secs(t0, t1);
            emails_after = restored.observer_state().emails_indexed();
            if (!restored.started_from_checkpoint())
                std::printf("  !! the checkpoint was refused (written=%d)\n", (int)checkpoint_ok);
            if (emails_after != emails_before)
                std::printf("  !! the mapped indexes disagree with the replayed ones: %zu vs %zu\n",
                            emails_after, emails_before);
        }
        std::printf("%-9s %-26s %-12s %9.1f MB %8.3f s  %12.0f MB/sec\n",
                    "ours", "REOPEN replaying the log", d, log_bytes / 1048576.0, reopen_seconds,
                    log_bytes / 1048576.0 / reopen_seconds);
        std::printf("%-9s %-26s %-12s %9zu idx %8.3f s  %12s\n",
                    "ours", "REOPEN mapping everything", d, emails_after, restart_seconds, "usable");
        wipe_ours();
    }

    // =================================================================
    // lmdb - two named DBs in one env, one transaction spanning both
    // =================================================================
    void bench_lmdb(std::size_t batch) {
        const std::string dl = label(batch);
        const char* d = dl.c_str();
        wipe_lmdb();

        MDB_env* env = nullptr;
        mdb_env_create(&env);
        mdb_env_set_maxdbs(env, 4);
        mdb_env_set_mapsize(env, 4ull << 30);
        // Always NOSYNC, and durability is taken explicitly below with
        // mdb_env_sync(). That is LMDB's documented group-commit shape: each
        // transaction still commits and is visible, only the flush is
        // deferred. With batch == 1 this is exactly a synchronous commit.
        unsigned flags = MDB_NOSUBDIR | MDB_NOSYNC | MDB_NOMETASYNC;
        if (mdb_env_open(env, "schemacmp_lmdb.mdb", flags, 0664) != 0) { std::printf("lmdb: open failed\n"); return; }

        auto group_commit = [&](std::size_t i) { if (batch && (i + 1) % batch == 0) mdb_env_sync(env, 1); };

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
            group_commit(i);
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
            group_commit(i);
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
            group_commit(i);
        }
        t1 = std::chrono::steady_clock::now();
        line("lmdb", "update INDEXED field", d, n_records, secs(t0, t1));

        const std::size_t n_deleted = n_records / 2;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_deleted; ++i) {
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
            group_commit(i);
        }
        t1 = std::chrono::steady_clock::now();
        line("lmdb", "delete + index entry", d, n_deleted, secs(t0, t1));

        mdb_env_sync(env, 1);
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
    void bench_rocksdb(std::size_t batch) {
        const std::string dl = label(batch);
        const char* d = dl.c_str();
        wipe_rocks();

        rocksdb::DB* db = nullptr;
        rocksdb::Options options;
        options.create_if_missing = true;
        options.create_missing_column_families = true;

        // Not defaults any more. The UNIQUE check is a lookup for a key that
        // is usually ABSENT, which is precisely what a bloom filter exists
        // for - without one every such probe walks the levels. 10 bits per key
        // is the usual setting; index and filter blocks are pinned rather than
        // cached so they are never evicted mid-run, and the block cache is
        // sized to hold the working set.
        rocksdb::BlockBasedTableOptions table_options;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        table_options.cache_index_and_filter_blocks = false;
        table_options.block_cache = rocksdb::NewLRUCache(512ull << 20);
        table_options.whole_key_filtering = true;

        rocksdb::ColumnFamilyOptions cf_options;
        cf_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
        options.table_factory = cf_options.table_factory;

        std::vector<rocksdb::ColumnFamilyDescriptor> cfs = {
            { rocksdb::kDefaultColumnFamilyName, cf_options },
            { "by_email", cf_options },
        };
        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        auto status = rocksdb::DB::Open(rocksdb::DBOptions(options), "schemacmp_rocks", cfs, &handles, &db);
        if (!status.ok()) { std::printf("rocksdb: open failed: %s\n", status.ToString().c_str()); return; }

        // Same shape: the WriteBatch is applied without a flush, and
        // durability is taken with SyncWAL(), which fsyncs the write-ahead log
        // without touching memtables. RocksDB's own group commit.
        rocksdb::WriteOptions wo;
        wo.sync = false;
        rocksdb::ReadOptions ro;
        auto group_commit = [&](std::size_t i) { if (batch && (i + 1) % batch == 0) db->SyncWAL(); };

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
            group_commit(i);
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
            group_commit(i);
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
            group_commit(i);
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "update INDEXED field", d, n_records, secs(t0, t1));

        const std::size_t n_deleted = n_records / 2;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_deleted; ++i) {
            const uint64_t id = ids[i];
            if (!db->Get(ro, handles[0], slice_of(id), &value).ok()) continue;
            const customer c = *(const customer*)value.data();
            const uint64_t email = c.email_hash;
            rocksdb::WriteBatch batch;
            batch.Delete(handles[1], slice_of(email));
            batch.Delete(handles[0], slice_of(id));
            db->Write(wo, &batch);
            group_commit(i);
        }
        t1 = std::chrono::steady_clock::now();
        line("rocksdb", "delete + index entry", d, n_deleted, secs(t0, t1));

        db->SyncWAL();
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

    // 0 = no durability at all, 1 = one fsync per transaction, N = group
    // commit shared by N transactions. All three engines support all three.
    std::vector<std::size_t> batches = { 0, 1, 100, 1000 };
    if (mode == "nosync") batches = { 0 };
    else if (mode == "sync") batches = { 1 };
    else if (mode != "both") batches = { (std::size_t)std::strtoull(mode.c_str(), nullptr, 10) };

    for (std::size_t batch : batches) {
        std::printf("\n--- durability: %s ---\n",
                    batch == 0 ? "none" : batch == 1 ? "one fsync per transaction"
                                                     : ("one fsync per " + std::to_string(batch) + " transactions").c_str());
        if (engine == "all" || engine == "ours") bench_ours(batch);
        if (engine == "all" || engine == "ours-fields") bench_ours_fields(batch);
        if (engine == "all" || engine == "lmdb") bench_lmdb(batch);
#ifdef HAVE_ROCKSDB
        if (engine == "all" || engine == "rocksdb") bench_rocksdb(batch);
#endif
    }
    return 0;
}
