// vps_matrix.cpp - the same workload on three engines, at two sizes, with and
// without a durability barrier. One binary, one process per cell, so nothing a
// previous cell left in the page cache can be mistaken for a result.
//
// THE BARRIER IS PLACED BY TIME, NOT BY OPERATION COUNT, and that is the
// point. "Every 1000 operations" is an arbitrary number that means something
// different for every engine and every dataset size - at 200 MB it fired 200
// times during a fill that took one second, at 1200 MB it fired 1200 times
// during six. What a device can actually absorb is a fixed number of
// durability barriers per second, and that number is measured, not guessed
// (see bench/msync_probe.c). So a cell that syncs issues at most one barrier
// per `barrier_us` microseconds, where barrier_us is that measured latency:
// the engine never asks the device to commit faster than it can, and never
// waits longer than it must.
//
// This is what real group commit does, and it adapts on its own - under load
// more operations fall into one barrier, at low rates each barrier covers
// fewer. An operation-count barrier does the opposite of adapting.
//
// Every cell that syncs places ONE durability barrier per barrier_us:
//
//   in-place   sync() - msync of the log's dirty range
//   field      sync() - the same
//   postgres   COMMIT with synchronous_commit = on, so the commit waits for
//              its WAL flush
//
// and every cell that does not sync removes exactly that: our engines never
// call sync(), postgres runs the same transaction shape with
// synchronous_commit = off, so it still groups but never waits. That is not
// the same as fsync=off - it is the closest thing that keeps the transaction
// structure identical between the two columns.
//
// MEASURED, AND THE ASSUMPTION WAS WRONG. A commit was expected to cost about
// the same regardless of payload. On this device (QEMU virtio, so hypervisor
// overhead is in every number) msync_probe.c says otherwise:
//
//     4 KB dirty     80.7 us    12396 barriers/sec
//    64 KB dirty    246.7 us     4054
//     1 MB dirty    958.9 us     1043
//     8 MB dirty   7567.7 us      132
//
// Above 64 KB the cost is linear in bytes - roughly 1 GB/sec - so the barrier
// is BANDWIDTH-bound there, not commit-rate-bound. Only the floor is fixed, at
// about 80 us for a single page.
//
// That floor is what barrier_us is set to. It is the honest reading of "one
// msync": the shortest a durability barrier can possibly take here. It also
// means the sync column pays heavily, because at a fill rate near 220 MB/sec
// each 80 us window accumulates ~18 KB and flushing that costs more than the
// floor. That is not a flaw in the measurement, it is what durability costs
// when it is demanded at the device's own latency.

#include "../storage/storage.hpp"
#include "../storage/field_storage.hpp"
#include "../storage/prefetch_gather.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <vector>

#ifdef HAVE_POSTGRES
#include <libpq-fe.h>
#endif
#ifdef HAVE_LMDB
#include <lmdb.h>
#endif
#ifdef HAVE_OBJECTBOX
#include <objectbox.h>
#include <flatbuffers/flatbuffers.h>
#endif

namespace {

    constexpr std::size_t value_bytes = 1024;
    constexpr std::size_t log_capacity = 1200ull << 20;  // in-place maps this TWICE, and the VPS has 3.5 GB free
    constexpr std::size_t max_ids = 1'500'000;
    std::size_t n_records = 0, n_ops = 10'000;
    bool do_sync = false;
    std::chrono::microseconds barrier_us{1000};
    std::size_t barrier_ops = 1000;
    // For LMDB and ObjectBox the durability unit is the transaction - there is
    // no way to make half a transaction durable. So in the sync column their
    // barrier IS their commit, and this is how many operations one transaction
    // holds. It is the knob that sets their crash window, the way
    // barrier_ops/barrier_us sets ours.
    std::size_t commit_ops = 10'000;

    // One barrier per barrier_us at most. The clock is read on every operation
    // - clock_gettime is a vDSO call of a few tens of nanoseconds, so at 200000
    // operations it costs single-digit milliseconds against a phase that runs
    // for seconds, and reading it less often would smuggle an operation count
    // back in through the side door.
    // WHICHEVER COMES FIRST: barrier_ops operations, or barrier_us elapsed.
    //
    // A pure time barrier at the device's own single-page latency turned out to
    // be the degeneration point for our engines specifically. sync() costs
    // about 87 us here and the interval was 80, so the check right after a
    // barrier always found the interval already expired and the loop collapsed
    // to a barrier every two operations: 107708 of them over 204800 inserts,
    // 9.4 s of iowait, fill down from 292498 to 13817. LMDB did not collapse
    // only because its own commit came in under the interval, at 46 us.
    //
    // A barrier must be spaced by more than it costs, or it spends all its time
    // being the previous one. The count cap bounds how much a crash can lose;
    // the time cap bounds how long a writer waits for durability. Neither alone
    // is a policy - the count alone lets a quiet period sit unflushed forever,
    // and the time alone lets a fast burst flush far more often than it needs.
    struct barrier_clock {
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        std::size_t fired = 0, since = 0;
        bool due() {
            if (!do_sync) return false;
            if (++since < barrier_ops && std::chrono::steady_clock::now() - last < barrier_us) return false;
            last = std::chrono::steady_clock::now();
            since = 0;
            ++fired;
            return true;
        }
    };

    struct blob {
        static constexpr storage::tag_t tag = 1;
        uint64_t key, counter, f1, f2;
        uint8_t payload[value_bytes - 32];
    };
    static_assert(sizeof(blob) == value_bytes);

    using inplace_spec = storage::storage_spec<storage::durability::local_sync, "vm_ip.db", log_capacity,
                                               "", "vm_ip.idx", "", max_ids>;
    using field_bench = storage::field_spec<"vm_fd.log", log_capacity, "vm_fd.idx", max_ids, 400'000, 8, value_bytes>;

    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };

    // THREE sources, because no one of them covers the field. getrusage sees
    // only this process, which is everything for our engines and almost
    // nothing for postgres - there the work happens in a backend the server
    // forked. /proc/stat sees the whole machine, which catches that backend but
    // also catches any noise. On a 1-core idle VPS the two agree for our
    // engines, and where they disagree for postgres the difference IS the
    // server's share.
    //
    // iowait is the reason this is worth measuring at all: it separates "the
    // engine is computing" from "the engine is stopped in front of the device",
    // and those two look identical in an ops/sec column.
    struct cpu_self { double user = 0, sys = 0; };
    cpu_self self_cpu() {
        struct rusage ru;
        if (::getrusage(RUSAGE_SELF, &ru) != 0) return {};
        return { ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
                 ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6 };
    }

    struct cpu_all { double user = 0, sys = 0, idle = 0, iowait = 0, steal = 0; };
    cpu_all system_cpu() {
        std::ifstream f("/proc/stat");
        std::string tag;
        f >> tag;
        if (tag != "cpu") return {};
        double v[10]{};
        for (int i = 0; i < 10 && f >> v[i]; ++i) {}
        const double hz = 100.0; // USER_HZ
        return { (v[0] + v[1]) / hz, v[2] / hz, v[3] / hz, v[4] / hz, v[7] / hz };
    }

    std::size_t major_faults() {
        struct rusage ru;
        return ::getrusage(RUSAGE_SELF, &ru) == 0 ? (std::size_t)ru.ru_majflt : 0;
    }
    std::size_t rss_mb() {
        std::ifstream f("/proc/self/status");
        std::string k;
        while (f >> k) { if (k == "VmRSS:") { std::size_t v; f >> v; return v / 1024; } }
        return 0;
    }

    const char* engine_name = "";
    struct phase_marks { cpu_self s0; cpu_all a0; std::size_t f0; std::chrono::steady_clock::time_point t0; };
    phase_marks mark() { return { self_cpu(), system_cpu(), major_faults(), std::chrono::steady_clock::now() }; }

    void row(const char* phase, std::size_t ops, const phase_marks& m, std::size_t barriers = 0) {
        const auto t1 = std::chrono::steady_clock::now();
        const cpu_self s1 = self_cpu();
        const cpu_all a1 = system_cpu();
        const double wall = std::chrono::duration<double>(t1 - m.t0).count();
        const double su = s1.user - m.s0.user, ss = s1.sys - m.s0.sys;
        const double au = a1.user - m.a0.user, as = a1.sys - m.a0.sys, aw = a1.iowait - m.a0.iowait;
        std::printf("%-8s %-4s %-14s %8zu ops %7.2f s %10.0f ops/s | self u%6.2f s%6.2f (%5.1f%% cpu) "
                    "| sys u%6.2f s%6.2f io%6.2f (%5.1f%% busy) | %6zu majflt %4zu MB %6zu barr\n",
                    engine_name, do_sync ? "sync" : "raw", phase, ops, wall, ops / wall,
                    su, ss, 100.0 * (su + ss) / wall,
                    au, as, aw, 100.0 * (au + as) / wall,
                    major_faults() - m.f0, rss_mb(), barriers);
        std::fflush(stdout);
    }

    // ---------------------------------------------------------------- ours
    template <typename table>
    void run_ours(table& t) {
        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));
        std::vector<storage::record_id> ids;
        ids.reserve(n_records);

        barrier_clock bc;
        auto m = mark();
        for (std::size_t i = 0; i < n_records; ++i) {
            b.key = i + 1; b.counter = i;
            ids.push_back(t.create(b));
            if (bc.due()) t.sync();
        }
        if (do_sync) { t.sync(); ++bc.fired; }
        row("fill", n_records, m, bc.fired);

        xorshift rng(9);
        volatile uint64_t sink = 0;
        blob out{};
        m = mark();
        for (std::size_t i = 0; i < n_ops; ++i) {
            const storage::record_id id = ids[rng.range(n_records)];
            if constexpr (requires { t.template get<blob>(id, out); }) {
                (void)t.template get<blob>(id, out);
                sink = out.counter;
            } else {
                const blob* p = t.template get<blob>(id);
                if (p) sink = p->counter;
            }
        }
        row("random read", n_ops, m);

        bc = barrier_clock{};
        m = mark();
        for (std::size_t i = 0; i < n_ops; ++i) {
            t.template update<&blob::counter>(ids[rng.range(n_records)], (uint64_t)i);
            if (bc.due()) t.sync();
        }
        if (do_sync) { t.sync(); ++bc.fired; }
        row("random update", n_ops, m, bc.fired);

        // ---- the cold-read pair -----------------------------------------
        //
        // The "random read" row above runs straight after fill, so the tail of
        // the log is still warm from writing it. That is fine for comparing
        // engines - every engine gets it - but it is useless for measuring what
        // prefetch is worth, because prefetch only pays on a page that is not
        // there yet.
        //
        // So both halves of this pair are run the same way: release_log() drops
        // the mapping and its page-cache pages first, and each half draws its
        // own picks so the first cannot warm the second. Only the field engine
        // has the piece decomposition prefetch_gather needs, so only it gets
        // these two rows.
        if constexpr (requires { t.release_log(std::size_t{}); t.raw_base(); }) {
            constexpr std::size_t chunk = 4096;
            std::vector<storage::record_id> picks(chunk);
            std::vector<std::byte> buf(chunk * value_bytes);

            auto cold_pass = [&](uint64_t seed, const char* label, bool use_prefetch) {
                t.sync();                       // clean pages, so they are droppable
                t.release_log(t.current_offset());
                xorshift r(seed);
                storage::prefetch_gather g(64);
                auto mm = mark();
                for (std::size_t done = 0; done < n_ops; done += chunk) {
                    const std::size_t k = std::min(chunk, n_ops - done);
                    for (std::size_t j = 0; j < k; ++j) picks[j] = ids[r.range(n_records)];
                    std::span<const storage::record_id> span{ picks.data(), k };
                    if (use_prefetch) {
                        g.read_many(t, span, buf.data(), value_bytes);
                    } else {
                        for (std::size_t j = 0; j < k; ++j)
                            (void)t.read_raw(span[j], std::span<std::byte>{ buf.data() + j * value_bytes, value_bytes });
                    }
                    sink = (uint64_t)buf[0];
                }
                row(label, n_ops, mm);
            };
            cold_pass(4242, "cold read", false);
            cold_pass(9797, "cold read + prefetch", true);
        }
        (void)sink;
    }

    // ------------------------------------------------------------ postgres
#ifdef HAVE_POSTGRES
    void die(PGconn* c, const char* what) {
        std::printf("postgres: %s: %s\n", what, PQerrorMessage(c));
        std::exit(1);
    }
    void exec(PGconn* c, const char* sql) {
        PGresult* r = PQexec(c, sql);
        if (PQresultStatus(r) != PGRES_COMMAND_OK && PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); die(c, sql); }
        PQclear(r);
    }

    void run_postgres(const char* conninfo) {
        PGconn* c = PQconnectdb(conninfo);
        if (PQstatus(c) != CONNECTION_OK) die(c, "connect");

        exec(c, do_sync ? "SET synchronous_commit = on" : "SET synchronous_commit = off");
        exec(c, "DROP TABLE IF EXISTS vm");
        exec(c, "CREATE TABLE vm (id bigint PRIMARY KEY, counter bigint, payload bytea)");

        std::vector<char> payload(value_bytes - 32, (char)0xAB);
        exec(c, "PREPARE ins (bigint, bigint, bytea) AS INSERT INTO vm VALUES ($1, $2, $3)");
        exec(c, "PREPARE sel (bigint) AS SELECT counter FROM vm WHERE id = $1");
        exec(c, "PREPARE upd (bigint, bigint) AS UPDATE vm SET counter = $2 WHERE id = $1");

        barrier_clock bc;
        auto m = mark();
        exec(c, "BEGIN");
        for (std::size_t i = 0; i < n_records; ++i) {
            const std::string id = std::to_string(i + 1), ctr = std::to_string(i);
            const char* vals[3] = { id.c_str(), ctr.c_str(), payload.data() };
            const int lens[3] = { 0, 0, (int)payload.size() };
            const int fmts[3] = { 0, 0, 1 };
            PGresult* r = PQexecPrepared(c, "ins", 3, vals, lens, fmts, 0);
            if (PQresultStatus(r) != PGRES_COMMAND_OK) { PQclear(r); die(c, "insert"); }
            PQclear(r);
            // The transaction is the barrier here, so a cell that does not sync
            // still has to commit sometimes - otherwise it would be one
            // gigantic transaction and would measure something else entirely.
            if (bc.due() || (!do_sync && (i + 1) % 10000 == 0)) { exec(c, "COMMIT"); exec(c, "BEGIN"); }
        }
        exec(c, "COMMIT");
        row("fill", n_records, m, bc.fired);

        // Index and table both freshly written; ANALYZE so the planner does not
        // pick a sequential scan for a point lookup and turn this into a
        // measurement of the planner.
        exec(c, "ANALYZE vm");

        xorshift rng(9);
        volatile uint64_t sink = 0;
        m = mark();
        for (std::size_t i = 0; i < n_ops; ++i) {
            const std::string id = std::to_string(rng.range(n_records) + 1);
            const char* v[1] = { id.c_str() };
            PGresult* r = PQexecPrepared(c, "sel", 1, v, nullptr, nullptr, 0);
            if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); die(c, "select"); }
            if (PQntuples(r) > 0) sink = (uint64_t)std::strtoull(PQgetvalue(r, 0, 0), nullptr, 10);
            PQclear(r);
        }
        row("random read", n_ops, m);

        bc = barrier_clock{};
        m = mark();
        exec(c, "BEGIN");
        for (std::size_t i = 0; i < n_ops; ++i) {
            const std::string id = std::to_string(rng.range(n_records) + 1), ctr = std::to_string(i);
            const char* v[2] = { id.c_str(), ctr.c_str() };
            PGresult* r = PQexecPrepared(c, "upd", 2, v, nullptr, nullptr, 0);
            if (PQresultStatus(r) != PGRES_COMMAND_OK) { PQclear(r); die(c, "update"); }
            PQclear(r);
            if (bc.due() || (!do_sync && (i + 1) % 10000 == 0)) { exec(c, "COMMIT"); exec(c, "BEGIN"); }
        }
        exec(c, "COMMIT");
        row("random update", n_ops, m, bc.fired);
        (void)sink;

        PGresult* r = PQexec(c, "SELECT pg_size_pretty(pg_total_relation_size('vm'))");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) std::printf("         table+index on disk: %s\n", PQgetvalue(r, 0, 0));
        PQclear(r);
        exec(c, "DROP TABLE vm");
        PQfinish(c);
    }
#endif

    // ------------------------------------------------------------- lmdb
    //
    // Here because it is the fastest READER among the general-purpose engines
    // measured earlier (2.20M/sec against RocksDB's 0.43M), and because it is
    // the only one that is mmap-based like ours - so its major-fault counts
    // mean the same thing in the same column, which is not true of postgres or
    // rocksdb, whose misses are read() calls the kernel never reports as faults.
    //
    // THE DURABILITY SHAPE, and the first version got it wrong in a way that
    // made LMDB look ten times worse than it is.
    //
    // It opened without MDB_NOSYNC in the sync column, so every mdb_txn_commit
    // carried a full flush - and since the barrier closes a transaction, and a
    // durable commit costs far more than the 80 us interval, it degenerated
    // into one durable commit per operation. LMDB read 1485 inserts/sec there
    // against 259616 without durability, a factor of 175. Configured the way
    // LMDB documents (bench/schema_comparison.cpp), the same engine on the same
    // machine pays 1.3x.
    //
    // Below is that documented shape, and it is the same one our engines get:
    // writes accumulate without flushing, and ONE explicit flush per barrier
    // makes everything before it durable. The transaction closes on a fixed
    // operation count in BOTH columns, so the only thing that varies is the
    // flush.
#ifdef HAVE_LMDB
    // BIG-ENDIAN KEYS, and not for style. LMDB's default comparator is
    // memcmp over the raw bytes, so a native little-endian uint64 sorts by its
    // LOW byte first: 256 (00 01 ..) lands before 255 (FF 00 ..). Ascending
    // numbers stop being ascending keys at exactly i = 256, which is where
    // MDB_APPEND - a bulk load's whole point, since it appends to the rightmost
    // page instead of searching - refuses with MDB_KEYEXIST. Byte-swapping
    // makes the tree order the numeric order.
    inline uint64_t be_key(uint64_t k) { return __builtin_bswap64(k); }

    void lmdb_check(int rc, const char* what) {
        if (rc) { std::printf("lmdb: %s: %s\n", what, mdb_strerror(rc)); std::exit(1); }
    }

    void run_lmdb() {
        ::system("rm -rf vm_lmdb && mkdir -p vm_lmdb");
        MDB_env* env = nullptr;
        lmdb_check(mdb_env_create(&env), "env_create");
        // NOT log_capacity. A B+tree needs more room than the bytes it stores:
        // page headers, splits leaving pages half full, and a free list that
        // only returns space at the next commit. Measured at 200 MB of records
        // it occupied 276 MB, a factor of 1.38, and setting the map to the data
        // size made a 1000 MB run die with MDB_MAP_FULL partway through the
        // fill. The map is address space, not disk - the file grows to what is
        // actually used - so it costs nothing to leave headroom.
        lmdb_check(mdb_env_set_mapsize(env, 2560ull << 20), "set_mapsize");
        // No MDB_WRITEMAP: with it LMDB writes through the mapping, which its
        // own documentation calls unrecoverable after a system crash when
        // combined with MDB_NOSYNC. It was originally set to make major-fault
        // counts comparable to ours; the comparison being fair matters more.
        //
        // In the sync column the flags are ZERO - LMDB's own default, where
        // mdb_txn_commit fsyncs the data file and then the meta page. An
        // earlier version of this benchmark kept MDB_NOSYNC|MDB_NOMETASYNC in
        // both columns and fired mdb_env_sync on the shared barrier clock. That
        // was not durability: the barrier landed between commits, on a file
        // nothing had been written to since the last one, so it flushed nothing
        // and cost nothing. It read as LMDB paying 5% for durability when in
        // fact it was paying for none. The crash window there was one
        // transaction - 10000 records - against our 27.
        const unsigned flags = do_sync ? 0u : (MDB_NOSYNC | MDB_NOMETASYNC);
        lmdb_check(mdb_env_open(env, "vm_lmdb", flags, 0644), "env_open");

        MDB_txn* txn = nullptr;
        MDB_dbi dbi;
        lmdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin");
        lmdb_check(mdb_dbi_open(txn, nullptr, 0, &dbi), "dbi_open");

        blob b{};
        std::memset(b.payload, 0xAB, sizeof(b.payload));

        barrier_clock bc;
        auto m = mark();
        for (std::size_t i = 0; i < n_records; ++i) {
            b.key = i + 1; b.counter = i;
            uint64_t k = be_key(i + 1);
            MDB_val mk{ sizeof(k), &k }, mv{ sizeof(b), &b };
            lmdb_check(mdb_put(txn, dbi, &mk, &mv, MDB_APPEND), "put");
            if ((i + 1) % commit_ops == 0) {
                lmdb_check(mdb_txn_commit(txn), "commit");   // durable here iff flags == 0
                lmdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin");
                ++bc.fired;
            }
        }
        lmdb_check(mdb_txn_commit(txn), "commit");
        ++bc.fired;
        row("fill", n_records, m, bc.fired);

        xorshift rng(9);
        volatile uint64_t sink = 0;
        lmdb_check(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), "rtxn");
        m = mark();
        for (std::size_t i = 0; i < n_ops; ++i) {
            uint64_t k = be_key(rng.range(n_records) + 1);
            MDB_val mk{ sizeof(k), &k }, mv{};
            if (mdb_get(txn, dbi, &mk, &mv) == 0) sink = ((const blob*)mv.mv_data)->counter;
        }
        row("random read", n_ops, m);
        mdb_txn_abort(txn);

        bc = barrier_clock{};
        lmdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin");
        m = mark();
        for (std::size_t i = 0; i < n_ops; ++i) {
            uint64_t k = be_key(rng.range(n_records) + 1);
            MDB_val mk{ sizeof(k), &k }, mv{};
            if (mdb_get(txn, dbi, &mk, &mv) == 0) {
                blob cur;
                std::memcpy(&cur, mv.mv_data, sizeof(cur));
                cur.counter = i;
                MDB_val nv{ sizeof(cur), &cur };
                lmdb_check(mdb_put(txn, dbi, &mk, &nv, 0), "put_upd");
            }
            if ((i + 1) % commit_ops == 0) {
                lmdb_check(mdb_txn_commit(txn), "commit");
                lmdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin");
                ++bc.fired;
            }
        }
        lmdb_check(mdb_txn_commit(txn), "commit");
        ++bc.fired;
        row("random update", n_ops, m, bc.fired);
        (void)sink;

        MDB_stat st{};
        lmdb_check(mdb_env_stat(env, &st), "stat");
        std::printf("         lmdb pages %zu of %u bytes = %.0f MB\n",
                    (std::size_t)(st.ms_branch_pages + st.ms_leaf_pages + st.ms_overflow_pages),
                    st.ms_psize,
                    (st.ms_branch_pages + st.ms_leaf_pages + st.ms_overflow_pages) * (double)st.ms_psize / 1048576.0);
        mdb_env_close(env);
        ::system("rm -rf vm_lmdb");
    }
#endif

    // ------------------------------------------------------------ objectbox
#ifdef HAVE_OBJECTBOX

    // ObjectBox stores whole objects as FlatBuffers, so there is no such thing
    // as a partial write here: an update to one field is a read, a rebuild and
    // a put of the entire object. That is not a handicap imposed by this
    // benchmark, it is what the storage format is - and it is exactly the axis
    // the field engine was built to attack, so it has to be measured as it is.
    //
    // Objects are built with the official FlatBufferBuilder rather than by
    // hand: property id N maps to FlatBuffers slot N-1, and getting that wrong
    // would store bytes ObjectBox cannot read. Verified separately by writing
    // objects and reading them back through ObjectBox's own reader.

    constexpr obx_schema_id obx_entity = 1;
    constexpr std::size_t obx_payload_bytes = value_bytes - 16;   // + id + counter

    void obx_check(bool ok, const char* what) {
        if (!ok) { std::printf("objectbox: %s: %s\n", what, obx_last_error_message()); std::exit(1); }
    }

    std::pair<const uint8_t*, std::size_t>
    obx_build(flatbuffers::FlatBufferBuilder& fbb, uint64_t id, uint64_t counter,
              const uint8_t* payload, std::size_t len) {
        fbb.Clear();
        const auto vec = fbb.CreateVector(payload, len);
        const auto start = fbb.StartTable();
        fbb.AddElement<uint64_t>(4, id, 0);
        fbb.AddElement<uint64_t>(6, counter, 0);
        fbb.AddOffset(8, vec);
        fbb.Finish(flatbuffers::Offset<void>(fbb.EndTable(start)));
        return { fbb.GetBufferPointer(), fbb.GetSize() };
    }

    void run_objectbox() {
        ::system("rm -rf vm_obx && mkdir -p vm_obx");

        OBX_model* model = obx_model();
        obx_check(model != nullptr, "model");
        obx_model_entity(model, "Blob", obx_entity, 1001);
        obx_model_property(model, "id", OBXPropertyType_Long, 1, 2001);
        obx_model_property_flags(model, OBXPropertyFlags_ID);
        obx_model_property(model, "counter", OBXPropertyType_Long, 2, 2002);
        obx_model_property(model, "payload", OBXPropertyType_ByteVector, 3, 2003);
        obx_model_entity_last_property_id(model, 3, 2003);
        obx_model_last_entity_id(model, obx_entity, 1001);
        obx_check(obx_model_error_code(model) == 0, "model build");

        OBX_store_options* opt = obx_opt();
        obx_opt_directory(opt, "vm_obx");
        // Same headroom argument as LMDB's map: a store needs more room than
        // the bytes it holds, and this is a cap rather than an allocation.
        obx_opt_max_db_size_in_kb(opt, 4ull * 1024 * 1024);
        // THERE IS NO RAW COLUMN FOR OBJECTBOX, and that is a property of the
        // engine rather than a gap in this benchmark. OBXWalFlags_NoSyncFile
        // ("does not wait for the disk to acknowledge; faster but not ACID
        // compliant") is the obvious analogue of MDB_NOSYNC, but asking for it
        // fails to open: "WAL is not yet supported by disk based DB provider"
        // as of 5.3.2. Every commit syncs, with no way to opt out.
        //
        // So ObjectBox is always measured durable, and the only thing that
        // moves is commit_ops - the transaction size, which IS its crash
        // window. That is the axis §8a compares on anyway, so the comparison is
        // the honest one; it just means its "no durability" cell is really
        // "the fewest syncs the API permits", one transaction for the phase.
        obx_check(obx_opt_model(opt, model) == 0, "opt_model");

        OBX_store* store = obx_store_open(opt);
        obx_check(store != nullptr, "store_open");

        std::vector<uint8_t> payload(obx_payload_bytes, 0xAB);
        flatbuffers::FlatBufferBuilder fbb(value_bytes + 128);

        barrier_clock bc;
        auto m = mark();
        {
            OBX_txn* txn = obx_txn_write(store);
            OBX_cursor* cur = obx_cursor(txn, obx_entity);
            for (std::size_t i = 0; i < n_records; ++i) {
                auto [bytes, size] = obx_build(fbb, i + 1, i, payload.data(), payload.size());
                obx_check(obx_cursor_put(cur, i + 1, bytes, size) == 0, "put");
                if ((i + 1) % commit_ops == 0) {
                    obx_cursor_close(cur);
                    obx_check(obx_txn_success(txn) == 0, "commit");
                    ++bc.fired;
                    txn = obx_txn_write(store);
                    cur = obx_cursor(txn, obx_entity);
                }
            }
            obx_cursor_close(cur);
            obx_check(obx_txn_success(txn) == 0, "commit");
            ++bc.fired;
        }
        row("fill", n_records, m, bc.fired);

        xorshift rng(9);
        volatile uint64_t sink = 0;
        {
            OBX_txn* txn = obx_txn_read(store);
            OBX_cursor* cur = obx_cursor(txn, obx_entity);
            m = mark();
            for (std::size_t i = 0; i < n_ops; ++i) {
                const void* data = nullptr;
                std::size_t size = 0;
                if (obx_cursor_get(cur, rng.range(n_records) + 1, &data, &size) == 0)
                    sink = flatbuffers::GetRoot<flatbuffers::Table>(data)->GetField<uint64_t>(6, 0);
            }
            row("random read", n_ops, m);
            obx_cursor_close(cur);
            obx_txn_close(txn);
        }

        bc = barrier_clock{};
        {
            std::vector<uint8_t> keep(obx_payload_bytes);
            OBX_txn* txn = obx_txn_write(store);
            OBX_cursor* cur = obx_cursor(txn, obx_entity);
            m = mark();
            for (std::size_t i = 0; i < n_ops; ++i) {
                const obx_id id = rng.range(n_records) + 1;
                const void* data = nullptr;
                std::size_t size = 0;
                if (obx_cursor_get(cur, id, &data, &size) == 0) {
                    // Read, modify, write the WHOLE object. The payload has to
                    // be carried across because a put replaces everything.
                    const auto* root = flatbuffers::GetRoot<flatbuffers::Table>(data);
                    const auto* vec = root->GetPointer<const flatbuffers::Vector<uint8_t>*>(8);
                    std::memcpy(keep.data(), vec->data(), keep.size());
                    auto [bytes, nsize] = obx_build(fbb, id, i, keep.data(), keep.size());
                    obx_check(obx_cursor_put(cur, id, bytes, nsize) == 0, "put_upd");
                }
                if ((i + 1) % commit_ops == 0) {
                    obx_cursor_close(cur);
                    obx_check(obx_txn_success(txn) == 0, "commit");
                    ++bc.fired;
                    txn = obx_txn_write(store);
                    cur = obx_cursor(txn, obx_entity);
                }
            }
            obx_cursor_close(cur);
            obx_check(obx_txn_success(txn) == 0, "commit");
            ++bc.fired;
            row("random update", n_ops, m, bc.fired);
        }
        (void)sink;

        obx_store_close(store);
        ::system("du -sm vm_obx | sed 's/^/         objectbox on disk: /;s/\tvm_obx/ MB/'");
        ::system("rm -rf vm_obx");
    }
#endif

} // namespace

int main(int argc, char** argv) {
    const std::string engine = argc > 1 ? argv[1] : "field";
    const double mb = argc > 2 ? std::strtod(argv[2], nullptr) : 200.0;
    do_sync = argc > 3 && std::string(argv[3]) == "sync";
    n_ops = argc > 4 ? (std::size_t)std::strtoull(argv[4], nullptr, 10) : 10'000;
    barrier_us = std::chrono::microseconds(argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 1000);
    barrier_ops = argc > 6 ? (std::size_t)std::strtoull(argv[6], nullptr, 10) : 1000;
    commit_ops = argc > 7 ? (std::size_t)std::strtoull(argv[7], nullptr, 10) : 10'000;
    n_records = (std::size_t)(mb * 1048576.0 / value_bytes);
    if (n_records >= max_ids) { std::printf("too many records for max_ids\n"); return 1; }

    engine_name = engine.c_str();
    std::printf("# %s / %s / %.0f MB / %zu records / %zu ops / barrier: %zu ops or %lld us, whichever first\n",
                engine.c_str(), do_sync ? "sync" : "raw", mb, n_records, n_ops,
                barrier_ops, (long long)barrier_us.count());

    if (engine == "inplace") {
        std::remove("vm_ip.db"); std::remove("vm_ip.db.data"); std::remove("vm_ip.idx");
        { auto t = std::make_unique<storage::storage<inplace_spec>>(); run_ours(*t); }
        std::remove("vm_ip.db"); std::remove("vm_ip.db.data"); std::remove("vm_ip.idx");
    } else if (engine == "field") {
        std::remove("vm_fd.log"); std::remove("vm_fd.idx");
        { auto t = std::make_unique<storage::field_storage<field_bench>>();
          if (!t->ok()) { std::printf("could not map\n"); return 1; }
          run_ours(*t); }
        std::remove("vm_fd.log"); std::remove("vm_fd.idx");
#ifdef HAVE_LMDB
    } else if (engine == "lmdb") {
        run_lmdb();
#endif
#ifdef HAVE_OBJECTBOX
    } else if (engine == "objectbox") {
        run_objectbox();
#endif
#ifdef HAVE_POSTGRES
    } else if (engine == "pg") {
        run_postgres(argc > 8 ? argv[8] : "host=/var/run/postgresql port=5433 user=postgres dbname=postgres");
#endif
    } else {
        std::printf("unknown engine: %s\n", engine.c_str());
        return 1;
    }
    return 0;
}
