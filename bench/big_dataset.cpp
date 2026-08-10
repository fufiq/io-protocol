// big_dataset.cpp - what happens when the data stops fitting in RAM.
//
// Every other benchmark here runs on a working set the page cache swallows
// whole, which measures the engine and nothing else. This one deliberately
// builds a dataset about twice the size of memory, so that a random read has
// to come off the device more often than not, and the question becomes how
// gracefully each engine degrades rather than how fast it is.
//
// ONE ENGINE PER RUN, because three of these do not fit on the disk at once:
//
//   big_dataset ours     [gigabytes]
//   big_dataset lmdb     [gigabytes]
//   big_dataset rocksdb  [gigabytes]
//
// Filling is done with no fsync at all - the point is to reach the size, not
// to measure durability, which every other benchmark here already covers.
//
// A note on OUR disk footprint, since it is the one asymmetry that matters at
// this scale: a local_sync storage keeps the log AND the materialized records,
// so it occupies twice the logical data. LMDB and RocksDB keep one copy. That
// is the price of the log/data split, and it is most visible exactly here.
//
// MEASURED: 26 GB of logical data (27262976 records of 1024 bytes) on a
// machine with 31 GB of RAM, ~25 GB of it available. 200k random operations.
//
//                        ours        lmdb     rocksdb    postgres
//   fill (rec/sec)     227902     1994078       39642       74675
//   fill (seconds)      119.6        13.7       687.7       365.1
//   random read          3262        2249        2263        4570
//   random update       10148        1010        2642        6172
//   RSS (MB)             7181        2759        1951    14 (client)
//   on disk (GB)         56.2        34.8        26.9        30.5
//
// * FILL varies by 50x between the extremes. LMDB writes one copy with
//   write() and MDB_APPEND; we write two (log AND materialized records) and
//   dirty mmap pages; RocksDB writes 26.9 GB of final data but moves several
//   times that through compaction levels, which is where its 687 seconds go.
//
// * RANDOM READ puts everyone within 2x, because everyone is now the device's
//   random-read latency. Postgres is fastest, and the reason is instructive:
//   it reads 8 KB pages with pread() into its own buffer pool, while we take
//   mmap faults. Its 8 GB shared_buffers plus the page cache holds a larger
//   effective working set than our resident mapping does.
//
// * RANDOM UPDATE is where the designs separate most. Ours writes a 32-byte
//   diff at the log tail and patches 8 bytes in place: one page touched,
//   14252 major faults for 200k updates. LMDB reads the whole kilobyte,
//   modifies it, and copies-on-write the entire B+tree path: 82153 faults for
//   the same 200k. Postgres writes a new tuple version without rewriting a
//   tree.
//
// * MAJOR FAULT COUNTS ARE ONLY COMPARABLE BETWEEN OURS AND LMDB. Both use
//   mmap, so their misses are page faults. RocksDB and Postgres read with
//   pread(), so their device traffic never appears as a fault at all - their
//   zeros mean "not measured here", not "no I/O".
//
// * WE PAY TWICE THE DISK, and this is where that shows. 56.2 GB against
//   26.9-34.8 GB, because a local_sync storage keeps the log AND the
//   materialized records. (The figure is also the fallocated capacity rather
//   than bytes in use, so it is a reservation as much as a footprint.)
//
// A NEGATIVE RESULT WORTH KEEPING: madvise(MADV_RANDOM) on the data region
// looks obviously right - the access pattern really is random - and makes
// everything worse. Reads 3262 -> 2516, updates 10148 -> 6180, major faults
// roughly doubled on reads and 10x on updates, fill 228k -> 150k rec/s. The
// region is written sequentially during fill and read randomly afterwards,
// and one advice cannot serve both; and a large readahead that pays off even
// occasionally beats many small faults, because the device is much better at
// one big read than at thirty-two small ones.
//
// A FIFTH ARM, `ours-fields`, is the same engine indexed per FIELD rather than
// per record: one file, nothing patched in place, an update appends the
// changed field and moves a pointer (storage/field_storage.hpp). Three runs of
// each, ALTERNATED, with writeback drained between them:
//
//                             ours   ours-fields    ratio
//   fill (rec/sec)          185662        321555     1.7x
//   random read               3168          7987     2.5x
//   random update             8120         11340     1.4x
//   read hot, before         10694         26400     2.5x
//   hot update x5             5419          8583     1.6x
//   read hot, after       31800000       3830000    0.12x
//   read hot, collapsed          -       9940000
//   faults / read            0.397         0.233     1.7x
//   faults / update          0.126        0.0016      78x
//   RSS                     6.1 GB       19.3 GB
//   on disk                56.2 GB       28.5 GB     2.0x
//
// ALTERNATING IS WHAT MADE THIS MEASURABLE. Run back to back, the same binary
// on the same data gave 1345113 and 257805 records/sec for the identical fill
// - a 5x spread that turned out to be the previous run's dirty pages rather
// than anything about the engine. Alternating the two engines and waiting for
// /proc/meminfo's Dirty and Writeback to drain first brings the spread to
// 1.1-1.6x, which is small enough for every row above to mean something.
//
// Postgres, for context, is not the same kind of thing: every operation above
// is a round trip to another process over a unix socket, and in exchange it
// gives SQL, a planner, MVCC and concurrent writers. It is configured to
// match where it can be - fsync off, synchronous_commit off, full page writes
// off, 8 GB shared_buffers, rows inserted one at a time in transactions of
// 10k, exactly as LMDB and RocksDB are batched here.
//
// Build:
//   g++ -std=c++23 -O3 -march=native -o big_dataset bench/big_dataset.cpp \
//       -llmdb -lrocksdb -lpq -I/usr/include/postgresql -DHAVE_ROCKSDB -DHAVE_POSTGRES
#include "../storage/storage.hpp"
#include "../storage/field_storage.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <vector>

#include <lmdb.h>

#ifdef HAVE_POSTGRES
#include <libpq-fe.h>
#endif

#ifdef HAVE_ROCKSDB
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#endif

namespace {

    constexpr std::size_t value_bytes = 1024;
    constexpr std::size_t log_capacity = 28ull << 30;
    constexpr std::size_t max_ids = 32'000'000;
    constexpr std::size_t n_random = 200'000;
    constexpr std::size_t n_hot = 100'000;      // records updated over and over
    constexpr std::size_t hot_rounds = 5;       // distinct fields touched per hot record

    std::size_t n_records = 0; // set from argv

    // Several scalar fields, not one: the field-granular prototype is only
    // interesting when a record accumulates overrides on DIFFERENT fields,
    // and one counter would never build a chain deeper than 1.
    struct blob {
        static constexpr storage::tag_t tag = 1;
        uint64_t key;
        uint64_t counter;
        uint64_t f1, f2, f3, f4;
        uint8_t payload[value_bytes - 48];
    };
    static_assert(sizeof(blob) == value_bytes);
    static_assert(storage::tagged_aggregate<blob>);

    using big_spec = storage::storage_spec<storage::durability::local_sync, "big_ours.db", log_capacity,
                                           "", "big_ours.idx", "", max_ids>;
    // max_depth 8, so five overridden fields never trip the collapse and the
    // gather is measured at its real depth rather than being optimised away.
    using field_bench_spec = storage::field_spec<"big_fields.log", log_capacity, "big_fields.idx",
                                                 max_ids, 2'000'000, 8, 2048>;

    double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }

    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };

    // Resident memory and page faults, which is the whole story once the data
    // stops fitting: minor faults are the page cache handing a page back,
    // major faults are the device being asked for it.
    struct pressure {
        std::size_t rss_mb, minor, major;
    };

    pressure sample() {
        pressure p{};
        std::ifstream stat("/proc/self/stat");
        std::string field;
        std::vector<std::string> f;
        while (stat >> field) f.push_back(field);
        if (f.size() > 23) {
            p.minor = std::stoull(f[9]);
            p.major = std::stoull(f[11]);
            p.rss_mb = std::stoull(f[23]) * 4096 / (1024 * 1024);
        }
        return p;
    }

    std::size_t free_ram_mb() {
        std::ifstream mi("/proc/meminfo");
        std::string k;
        while (mi >> k) {
            if (k == "MemAvailable:") { std::size_t v; mi >> v; return v / 1024; }
        }
        return 0;
    }

    std::size_t dir_bytes(const char* path) {
        std::string cmd = std::string("du -sb ") + path + " 2>/dev/null | cut -f1";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return 0;
        char buf[64]{};
        if (!std::fgets(buf, sizeof(buf), f)) { pclose(f); return 0; }
        pclose(f);
        return (std::size_t)std::strtoull(buf, nullptr, 10);
    }

    void report(const char* engine, const char* phase, std::size_t ops, double s) {
        std::printf("  %-8s %-22s %10zu ops  %8.2f s  %11.0f ops/sec  avail RAM %6zu MB\n",
                    engine, phase, ops, s, ops / s, free_ram_mb());
        std::fflush(stdout);
    }

    void progress(std::size_t done, std::size_t total, double s, std::size_t bytes) {
        std::printf("      %3.0f%%  %8.1f GB written  %8.0f rec/sec  avail RAM %6zu MB\n",
                    100.0 * done / total, bytes / 1073741824.0, done / s, free_ram_mb());
        std::fflush(stdout);
    }

    // =================================================================
    void run_ours() {
        std::remove("big_ours.db"); std::remove("big_ours.db.data"); std::remove("big_ours.idx");
        {
            storage::storage<big_spec> table;
            blob b{};
            std::memset(b.payload, 0xAB, sizeof(b.payload));

            auto t0 = std::chrono::steady_clock::now();
            std::vector<storage::record_id> ids;
            ids.reserve(n_records);
            for (std::size_t i = 0; i < n_records; ++i) {
                b.key = i + 1;
                b.counter = i;
                ids.push_back(table.create(b));
                if ((i + 1) % (n_records / 10) == 0)
                    progress(i + 1, n_records, secs(t0, std::chrono::steady_clock::now()), table.current_offset() * 2);
            }
            auto t1 = std::chrono::steady_clock::now();
            report("ours", "fill", n_records, secs(t0, t1));

            const pressure before = sample();
            xorshift rng(9);
            volatile uint64_t sink = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_random; ++i) sink = table.get<blob>(ids[rng.range(n_records)])->counter;
            t1 = std::chrono::steady_clock::now();
            report("ours", "random read", n_random, secs(t0, t1));
            (void)sink;
            const pressure after_read = sample();

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_random; ++i)
                table.update<&blob::counter>(ids[rng.range(n_records)], (uint64_t)i);
            t1 = std::chrono::steady_clock::now();
            report("ours", "random update", n_random, secs(t0, t1));
            const pressure after_update = sample();

            // A hot subset, read before and after being updated many times.
            // This is what separates the two designs: patching in place keeps
            // a record contiguous no matter how often it changes, so reading
            // it must cost the same afterwards as before.
            std::vector<storage::record_id> hot;
            hot.reserve(n_hot);
            for (std::size_t i = 0; i < n_hot; ++i) hot.push_back(ids[rng.range(n_records)]);

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_hot; ++i) sink = table.get<blob>(hot[i])->f1;
            t1 = std::chrono::steady_clock::now();
            report("ours", "read hot, before", n_hot, secs(t0, t1));

            t0 = std::chrono::steady_clock::now();
            for (std::size_t r = 0; r < hot_rounds; ++r)
                for (std::size_t i = 0; i < n_hot; ++i) {
                    const storage::record_id id = hot[i];
                    switch (r) {
                        case 0: table.update<&blob::counter>(id, r); break;
                        case 1: table.update<&blob::f1>(id, r); break;
                        case 2: table.update<&blob::f2>(id, r); break;
                        case 3: table.update<&blob::f3>(id, r); break;
                        default: table.update<&blob::f4>(id, r); break;
                    }
                }
            t1 = std::chrono::steady_clock::now();
            report("ours", "hot update x5", n_hot * hot_rounds, secs(t0, t1));

            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_hot; ++i) sink = table.get<blob>(hot[i])->f1;
            t1 = std::chrono::steady_clock::now();
            report("ours", "read hot, after", n_hot, secs(t0, t1));

            std::printf("      RSS %zu MB, major faults: %zu during reads, %zu during updates\n",
                        sample().rss_mb, after_read.major - before.major, after_update.major - after_read.major);
        }
        std::printf("      on disk: %.1f GB (log + materialized records + index)\n",
                    (dir_bytes("big_ours.db") + dir_bytes("big_ours.db.data") + dir_bytes("big_ours.idx")) / 1073741824.0);
        std::remove("big_ours.db"); std::remove("big_ours.db.data"); std::remove("big_ours.idx");
    }

    // =================================================================
    void run_lmdb() {
        std::remove("big_lmdb.mdb"); std::remove("big_lmdb.mdb-lock");
        MDB_env* env = nullptr;
        mdb_env_create(&env);
        mdb_env_set_mapsize(env, 48ull << 30);
        if (mdb_env_open(env, "big_lmdb.mdb", MDB_NOSUBDIR | MDB_NOSYNC | MDB_NOMETASYNC, 0664) != 0) {
            std::printf("lmdb: open failed\n"); return;
        }
        MDB_dbi dbi;
        { MDB_txn* t; mdb_txn_begin(env, nullptr, 0, &t); mdb_dbi_open(t, nullptr, MDB_INTEGERKEY, &dbi); mdb_txn_commit(t); }

        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));

        // Batched: one transaction per record would measure LMDB's commit
        // path, which earlier benchmarks already do. Here the point is to
        // reach the size.
        auto t0 = std::chrono::steady_clock::now();
        constexpr std::size_t batch = 10'000;
        for (std::size_t base = 0; base < n_records; base += batch) {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            for (std::size_t i = base; i < base + batch && i < n_records; ++i) {
                uint64_t key = i + 1;
                b.key = key; b.counter = i;
                MDB_val k{ sizeof(key), &key }, v{ sizeof(b), &b };
                mdb_put(t, dbi, &k, &v, MDB_APPEND);
            }
            mdb_txn_commit(t);
            if ((base + batch) % (n_records / 10) < batch)
                progress(base + batch, n_records, secs(t0, std::chrono::steady_clock::now()), dir_bytes("big_lmdb.mdb"));
        }
        auto t1 = std::chrono::steady_clock::now();
        report("lmdb", "fill", n_records, secs(t0, t1));

        const pressure before = sample();
        xorshift rng(9);
        volatile uint64_t sink = 0;
        {
            MDB_txn* rt = nullptr;
            mdb_txn_begin(env, nullptr, MDB_RDONLY, &rt);
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_random; ++i) {
                uint64_t key = rng.range(n_records) + 1;
                MDB_val k{ sizeof(key), &key }, v{};
                if (mdb_get(rt, dbi, &k, &v) == 0) sink = ((const blob*)v.mv_data)->counter;
            }
            t1 = std::chrono::steady_clock::now();
            mdb_txn_abort(rt);
        }
        report("lmdb", "random read", n_random, secs(t0, t1));
        (void)sink;
        const pressure after_read = sample();

        t0 = std::chrono::steady_clock::now();
        for (std::size_t base = 0; base < n_random; base += 1000) {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            for (std::size_t i = base; i < base + 1000 && i < n_random; ++i) {
                uint64_t key = rng.range(n_records) + 1;
                MDB_val k{ sizeof(key), &key }, v{};
                if (mdb_get(t, dbi, &k, &v) != 0) continue;
                blob cur;
                std::memcpy(&cur, v.mv_data, sizeof(cur));
                cur.counter = i;
                MDB_val nv{ sizeof(cur), &cur };
                mdb_put(t, dbi, &k, &nv, 0);
            }
            mdb_txn_commit(t);
        }
        t1 = std::chrono::steady_clock::now();
        report("lmdb", "random update", n_random, secs(t0, t1));
        const pressure after_update = sample();
        std::printf("      RSS %zu MB, major faults: %zu during reads, %zu during updates\n",
                    after_update.rss_mb, after_read.major - before.major, after_update.major - after_read.major);

        mdb_env_sync(env, 1);
        mdb_env_close(env);
        std::printf("      on disk: %.1f GB\n", dir_bytes("big_lmdb.mdb") / 1073741824.0);
        std::remove("big_lmdb.mdb"); std::remove("big_lmdb.mdb-lock");
    }

#ifdef HAVE_ROCKSDB
    // =================================================================
    void run_rocksdb() {
        (void)std::system("rm -rf big_rocks");
        rocksdb::DB* db = nullptr;
        rocksdb::Options options;
        options.create_if_missing = true;
        options.compression = rocksdb::kNoCompression; // the others do not compress either
        rocksdb::BlockBasedTableOptions table_options;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        table_options.block_cache = rocksdb::NewLRUCache(2ull << 30);
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
        auto st = rocksdb::DB::Open(options, "big_rocks", &db);
        if (!st.ok()) { std::printf("rocksdb: open failed: %s\n", st.ToString().c_str()); return; }

        rocksdb::WriteOptions wo;
        wo.sync = false;
        rocksdb::ReadOptions ro;

        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));

        auto t0 = std::chrono::steady_clock::now();
        constexpr std::size_t batch = 10'000;
        for (std::size_t base = 0; base < n_records; base += batch) {
            rocksdb::WriteBatch wb;
            for (std::size_t i = base; i < base + batch && i < n_records; ++i) {
                const uint64_t key = i + 1;
                b.key = key; b.counter = i;
                wb.Put(rocksdb::Slice((const char*)&key, sizeof(key)), rocksdb::Slice((const char*)&b, sizeof(b)));
            }
            db->Write(wo, &wb);
            if ((base + batch) % (n_records / 10) < batch)
                progress(base + batch, n_records, secs(t0, std::chrono::steady_clock::now()), dir_bytes("big_rocks"));
        }
        auto t1 = std::chrono::steady_clock::now();
        report("rocksdb", "fill", n_records, secs(t0, t1));

        const pressure before = sample();
        xorshift rng(9);
        volatile uint64_t sink = 0;
        std::string value;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_random; ++i) {
            const uint64_t key = rng.range(n_records) + 1;
            if (db->Get(ro, rocksdb::Slice((const char*)&key, sizeof(key)), &value).ok())
                sink = ((const blob*)value.data())->counter;
        }
        t1 = std::chrono::steady_clock::now();
        report("rocksdb", "random read", n_random, secs(t0, t1));
        (void)sink;
        const pressure after_read = sample();

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_random; ++i) {
            const uint64_t key = rng.range(n_records) + 1;
            if (!db->Get(ro, rocksdb::Slice((const char*)&key, sizeof(key)), &value).ok()) continue;
            blob cur;
            std::memcpy(&cur, value.data(), sizeof(cur));
            cur.counter = i;
            db->Put(wo, rocksdb::Slice((const char*)&key, sizeof(key)), rocksdb::Slice((const char*)&cur, sizeof(cur)));
        }
        t1 = std::chrono::steady_clock::now();
        report("rocksdb", "random update", n_random, secs(t0, t1));
        const pressure after_update = sample();
        std::printf("      RSS %zu MB, major faults: %zu during reads, %zu during updates\n",
                    after_update.rss_mb, after_read.major - before.major, after_update.major - after_read.major);

        delete db;
        std::printf("      on disk: %.1f GB\n", dir_bytes("big_rocks") / 1073741824.0);
        (void)std::system("rm -rf big_rocks");
    }
#endif

    // =================================================================
    // ours-fields - the same log, indexed per FIELD, with no second region
    // and nothing patched in place. See bench/field_log.hpp.
    // =================================================================
    void run_ours_fields() {
        std::remove("big_fields.log"); std::remove("big_fields.idx");
        storage::field_storage<field_bench_spec> fs;
        if (!fs.ok()) { std::printf("ours-fields: could not map the log\n"); return; }

        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));
        blob out{};

        auto t0 = std::chrono::steady_clock::now();
        std::vector<storage::record_id> ids;
        ids.reserve(n_records);
        for (std::size_t i = 0; i < n_records; ++i) {
            b.key = i + 1; b.counter = i;
            ids.push_back(fs.create(b));
            if ((i + 1) % (n_records / 10) == 0)
                progress(i + 1, n_records, secs(t0, std::chrono::steady_clock::now()), fs.current_offset());
        }
        auto t1 = std::chrono::steady_clock::now();
        report("ours-fld", "fill", n_records, secs(t0, t1));

        const pressure before = sample();
        xorshift rng(9);
        volatile uint64_t sink = 0;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_random; ++i) { fs.get(ids[rng.range(n_records)], out); sink = out.counter; }
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "random read", n_random, secs(t0, t1));
        const pressure after_read = sample();

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_random; ++i) fs.update<&blob::counter>(ids[rng.range(n_records)], (uint64_t)i);
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "random update", n_random, secs(t0, t1));
        const pressure after_update = sample();

        std::vector<storage::record_id> hot;
        hot.reserve(n_hot);
        for (std::size_t i = 0; i < n_hot; ++i) hot.push_back(ids[rng.range(n_records)]);

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_hot; ++i) { fs.get(hot[i], out); sink = out.f1; }
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "read hot, before", n_hot, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t r = 0; r < hot_rounds; ++r)
            for (std::size_t i = 0; i < n_hot; ++i)
                switch (r) {
                    case 0: fs.update<&blob::counter>(hot[i], r); break;
                    case 1: fs.update<&blob::f1>(hot[i], r); break;
                    case 2: fs.update<&blob::f2>(hot[i], r); break;
                    case 3: fs.update<&blob::f3>(hot[i], r); break;
                    default: fs.update<&blob::f4>(hot[i], r); break;
                }
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "hot update x5", n_hot * hot_rounds, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_hot; ++i) { fs.get(hot[i], out); sink = out.f1; }
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "read hot, after", n_hot, secs(t0, t1));
        (void)sink;

        // And with the safety valve applied by hand rather than by threshold.
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_hot; ++i) fs.collapse(hot[i]);
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "collapse hot", n_hot, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_hot; ++i) { fs.get(hot[i], out); sink = out.f1; }
        t1 = std::chrono::steady_clock::now();
        report("ours-fld", "read hot, collapsed", n_hot, secs(t0, t1));

        std::printf("      RSS %zu MB, major faults: %zu during reads, %zu during updates\n",
                    sample().rss_mb, after_read.major - before.major, after_update.major - after_read.major);
        std::printf("      overrides live: %zu, collapses: %zu (including the explicit phase above), index file %.0f MB\n",
                    fs.overrides_used(), fs.collapse_count(), field_bench_spec::index_bytes / 1048576.0);
        std::printf("      on disk: %.1f GB (log %.1f + index %.1f)\n",
                    (dir_bytes("big_fields.log") + dir_bytes("big_fields.idx")) / 1073741824.0,
                    dir_bytes("big_fields.log") / 1073741824.0, dir_bytes("big_fields.idx") / 1073741824.0);
        std::remove("big_fields.log"); std::remove("big_fields.idx");
    }

#ifdef HAVE_POSTGRES
    // =================================================================
    // postgres - a different class of system, and the comparison has to say
    // so. It is a SERVER: every operation below is a round trip over a unix
    // socket to another process, which none of the other three pay. It also
    // gives what none of the other three give - SQL, a planner, MVCC,
    // multiple concurrent writers, and constraints the database enforces
    // rather than the application.
    //
    // Configured to match the others where it can be matched: fsync off,
    // synchronous_commit off, full page writes off, and rows inserted one at
    // a time inside transactions of 10k, exactly as LMDB and RocksDB are
    // batched here. COPY would be faster and is Postgres's real bulk path,
    // but the other three have no equivalent, so it is reported separately
    // rather than used as the headline.
    // =================================================================
    void pg_check(PGresult* r, PGconn* c, const char* what) {
        const ExecStatusType st = PQresultStatus(r);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK && st != PGRES_COPY_IN)
            std::printf("      postgres %s failed: %s\n", what, PQerrorMessage(c));
    }

    void run_postgres() {
        PGconn* c = PQconnectdb("host=/tmp/pgsock dbname=postgres");
        if (PQstatus(c) != CONNECTION_OK) { std::printf("postgres: %s\n", PQerrorMessage(c)); return; }

        pg_check(PQexec(c, "DROP TABLE IF EXISTS big"), c, "drop");
        pg_check(PQexec(c, "CREATE TABLE big (id bigint PRIMARY KEY, counter bigint, payload bytea)"), c, "create");
        pg_check(PQexec(c, "ALTER TABLE big ALTER COLUMN payload SET STORAGE EXTERNAL"), c, "no-compress");

        std::vector<char> payload(value_bytes - 16, (char)0xAB);
        // bytea hex format: \x followed by two characters per byte.
        std::string hex = "\\x";
        static const char* digits = "0123456789abcdef";
        for (char b : payload) { hex += digits[((unsigned char)b) >> 4]; hex += digits[((unsigned char)b) & 15]; }

        pg_check(PQprepare(c, "ins", "INSERT INTO big VALUES ($1::bigint, $2::bigint, $3::bytea)", 3, nullptr), c, "prepare insert");
        pg_check(PQprepare(c, "sel", "SELECT counter FROM big WHERE id = $1::bigint", 1, nullptr), c, "prepare select");
        pg_check(PQprepare(c, "upd", "UPDATE big SET counter = $2::bigint WHERE id = $1::bigint", 2, nullptr), c, "prepare update");

        auto t0 = std::chrono::steady_clock::now();
        constexpr std::size_t batch = 10'000;
        for (std::size_t base = 0; base < n_records; base += batch) {
            PQclear(PQexec(c, "BEGIN"));
            for (std::size_t i = base; i < base + batch && i < n_records; ++i) {
                const std::string id = std::to_string(i + 1), counter = std::to_string(i);
                const char* vals[3] = { id.c_str(), counter.c_str(), hex.c_str() };
                PQclear(PQexecPrepared(c, "ins", 3, vals, nullptr, nullptr, 0));
            }
            PQclear(PQexec(c, "COMMIT"));
            if ((base + batch) % (n_records / 10) < batch) {
                PGresult* r = PQexec(c, "SELECT pg_total_relation_size('big')");
                const std::size_t sz = PQntuples(r) ? std::strtoull(PQgetvalue(r, 0, 0), nullptr, 10) : 0;
                PQclear(r);
                progress(base + batch, n_records, secs(t0, std::chrono::steady_clock::now()), sz);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        report("postgres", "fill (batched INSERT)", n_records, secs(t0, t1));

        const pressure before = sample();
        xorshift rng(9);
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_random; ++i) {
            const std::string id = std::to_string(rng.range(n_records) + 1);
            const char* vals[1] = { id.c_str() };
            PQclear(PQexecPrepared(c, "sel", 1, vals, nullptr, nullptr, 0));
        }
        t1 = std::chrono::steady_clock::now();
        report("postgres", "random read", n_random, secs(t0, t1));
        const pressure after_read = sample();

        t0 = std::chrono::steady_clock::now();
        for (std::size_t base = 0; base < n_random; base += 1000) {
            PQclear(PQexec(c, "BEGIN"));
            for (std::size_t i = base; i < base + 1000 && i < n_random; ++i) {
                const std::string id = std::to_string(rng.range(n_records) + 1), counter = std::to_string(i);
                const char* vals[2] = { id.c_str(), counter.c_str() };
                PQclear(PQexecPrepared(c, "upd", 2, vals, nullptr, nullptr, 0));
            }
            PQclear(PQexec(c, "COMMIT"));
        }
        t1 = std::chrono::steady_clock::now();
        report("postgres", "random update", n_random, secs(t0, t1));
        const pressure after_update = sample();
        std::printf("      client RSS %zu MB, major faults: %zu reads, %zu updates"
                    " (the SERVER's are not counted - it is another process)\n",
                    after_update.rss_mb, after_read.major - before.major, after_update.major - after_read.major);

        PGresult* r = PQexec(c, "SELECT pg_total_relation_size('big'), pg_relation_size('big'), pg_indexes_size('big')");
        if (PQntuples(r))
            std::printf("      on disk: %.1f GB total (heap %.1f GB + indexes %.1f GB)\n",
                        std::strtoull(PQgetvalue(r, 0, 0), nullptr, 10) / 1073741824.0,
                        std::strtoull(PQgetvalue(r, 0, 1), nullptr, 10) / 1073741824.0,
                        std::strtoull(PQgetvalue(r, 0, 2), nullptr, 10) / 1073741824.0);
        PQclear(r);
        PQclear(PQexec(c, "DROP TABLE big"));
        PQfinish(c);
    }
#endif

} // namespace

int main(int argc, char** argv) {
    const std::string engine = argc > 1 ? argv[1] : "ours";
    const double gigabytes = argc > 2 ? std::strtod(argv[2], nullptr) : 26.0;
    n_records = (std::size_t)(gigabytes * 1073741824.0 / value_bytes);

    std::printf("%.1f GB of logical data: %zu records of %zu bytes\n", gigabytes, n_records, value_bytes);
    std::printf("available RAM at start: %zu MB - the dataset is meant to exceed it\n\n", free_ram_mb());
    std::fflush(stdout);

    if (engine == "ours") run_ours();
    else if (engine == "ours-fields") run_ours_fields();
    else if (engine == "lmdb") run_lmdb();
#ifdef HAVE_ROCKSDB
    else if (engine == "rocksdb") run_rocksdb();
#endif
#ifdef HAVE_POSTGRES
    else if (engine == "postgres") run_postgres();
#endif
    else std::printf("unknown engine: %s\n", engine.c_str());
    return 0;
}
