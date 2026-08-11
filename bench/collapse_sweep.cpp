// collapse_sweep.cpp - what the collapse threshold actually costs and buys.
//
// field_storage keeps a record as its base CREATE plus a chain of overridden
// fields, and spec::max_depth decides how deep that chain may get before an
// update rewrites the record contiguously. The knob trades two things against
// each other and nothing else:
//
//   low threshold    reads are cheap (short gather) and the LOG GROWS, because
//                    every collapse writes a whole record again
//   high threshold   the log grows only by the diffs, and reads pay for the
//                    gather
//
// Both ends are measured here, at a size that fits in RAM and at one that does
// not, because the balance is not the same: out of RAM a deep gather is extra
// random reads, in RAM it is only extra memcpy.
//
// A separate variant collapses on READ instead of on update: the threshold on
// update flattens every record that gets written, this one flattens only the
// records somebody looks at.
//
// Build: g++ -std=c++23 -O3 -march=native -o collapse_sweep bench/collapse_sweep.cpp
#include "../storage/field_storage.hpp"
#include "../storage/hybrid_storage.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <memory>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

    constexpr std::size_t value_bytes = 1024;
    constexpr std::size_t fields = 32;
    std::size_t rounds = 8;   // how many DISTINCT fields each hot record gets

    struct rec {
        static constexpr storage::tag_t tag = 1;
        uint64_t key;
        uint64_t f[fields];
        uint8_t payload[value_bytes - 8 * (fields + 1)];
    };
    static_assert(sizeof(rec) == value_bytes);
    static_assert(storage::tagged_aggregate<rec>);

    constexpr std::size_t capacity = 8ull << 30;
    constexpr std::size_t max_ids = 2'000'000;
    constexpr std::size_t max_overrides = 6'000'000;

    std::size_t n_records = 0, n_touch = 0;

    // Variants H and I (first override kept in the slot) are gone with the
    // field they used: measured 9.2M upd/s against the baseline's 12.5M for the
    // same 4.08M reads, and those 16 bytes now hold the record's len and tag,
    // which keeps a read from having to touch the log to learn its length.
    template <std::size_t depth, std::size_t span = 0>
    using sweep_spec = storage::field_spec<"collapse.log", capacity, "collapse.idx",
                                           max_ids, max_overrides, depth, 2048, "", span>;

    // What a variant does between the update phase and the read phase, which
    // is where a background sweep or a compaction would sit.
    enum class between { nothing, sweep_collapse, chain_compact, full_compaction };

    double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }
    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };
    std::size_t major_faults() {
        std::ifstream st("/proc/self/stat");
        std::string f;
        std::vector<std::string> v;
        while (st >> f) v.push_back(f);
        return v.size() > 11 ? std::stoull(v[11]) : 0;
    }

    // read_collapse_at == 0 means the only collapse is the update threshold.
    template <std::size_t depth, std::size_t span = 0>
    void run(const char* label, uint32_t read_collapse_at, between mid = between::nothing, uint32_t mid_depth = 4) {
        std::remove("collapse.log"); std::remove("collapse.idx");
        std::byte* scratch = nullptr;
        int scratch_fd = -1;
        const std::size_t scratch_bytes = capacity / 2;
        storage::field_storage<sweep_spec<depth, span>> t;
        if (!t.ok()) { std::printf("could not map\n"); return; }

        rec r{};
        std::memset(r.payload, 0xAB, sizeof(r.payload));
        std::vector<storage::record_id> ids;
        ids.reserve(n_records);
        for (std::size_t i = 0; i < n_records; ++i) { r.key = i + 1; ids.push_back(t.create(r)); }
        const std::size_t after_fill = t.current_offset();

        // Touch `fields` distinct fields of each hot record, so the chain
        // would reach `fields` deep if nothing collapsed it.
        xorshift rng(11);
        std::vector<storage::record_id> hot;
        hot.reserve(n_touch);
        for (std::size_t i = 0; i < n_touch; ++i) hot.push_back(ids[rng.range(n_records)]);

        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t round = 0; round < rounds; ++round)
            for (std::size_t i = 0; i < n_touch; ++i)
                t.patch(hot[i], (uint32_t)(offsetof(rec, f) + round * 8),
                        std::as_bytes(std::span{&round, 1}));
        auto t1 = std::chrono::steady_clock::now();
        const std::size_t f1 = major_faults();
        const double update_s = secs(t0, t1);
        const std::size_t after_update = t.current_offset();

        // The maintenance step, timed on its own: this is work that would run
        // off the critical path, and hiding it inside the read or update
        // number would be exactly the lie this benchmark exists to avoid.
        double mid_s = 0;
        std::size_t mid_n = 0;
        {
            auto m0 = std::chrono::steady_clock::now();
            if (mid == between::sweep_collapse) mid_n = t.collapse_deep(mid_depth);
            else if (mid == between::chain_compact) {
                for (auto id : hot) if (t.depth(id) > mid_depth) { t.compact_chain(id); ++mid_n; }
            } else if (mid == between::full_compaction) {
                scratch_fd = ::open("collapse.scratch", O_RDWR | O_CREAT | O_TRUNC, 0644);
                ::posix_fallocate(scratch_fd, 0, scratch_bytes);
                scratch = (std::byte*)::mmap(nullptr, scratch_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, scratch_fd, 0);
                t.compact(scratch);
                mid_n = n_records;
            }
            mid_s = secs(m0, std::chrono::steady_clock::now());
        }

        rec out{};
        volatile uint64_t sink = 0;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_touch; ++i) {
            t.get(hot[i], out);
            sink = out.f[0];
            if (read_collapse_at) t.collapse_if_deeper_than(hot[i], read_collapse_at);
        }
        t1 = std::chrono::steady_clock::now();
        const std::size_t f2 = major_faults();
        const double read_s = secs(t0, t1);
        (void)sink;

        // A second read pass, so a read-driven collapse gets to show what it
        // bought: the first pass paid for it, the second enjoys it.
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_touch; ++i) { t.get(hot[i], out); sink = out.f[0]; }
        t1 = std::chrono::steady_clock::now();
        const double read2_s = secs(t0, t1);

        std::size_t total_depth = 0;
        for (auto id : hot) total_depth += t.depth(id);

        std::printf("  %-22s %9.0f upd/s %9.0f rd/s %9.0f rd2/s  log +%5.0f MB  depth %4.1f  maint %6.3f s (%zu)\n",
                    label, (n_touch * rounds) / update_s, n_touch / read_s, n_touch / read2_s,
                    (t.current_offset() - after_fill) / 1048576.0, (double)total_depth / n_touch,
                    mid_s, mid_n);
        std::fflush(stdout);
        if (scratch) { ::munmap(scratch, scratch_bytes); ::close(scratch_fd); std::remove("collapse.scratch"); scratch = nullptr; }
        std::remove("collapse.log"); std::remove("collapse.idx");
    }

    // The shipping architecture on the identical workload, for scale. It has
    // no collapse policy and cannot have one: it patches the record in place,
    // so a record is contiguous at all times and a read is a pointer, not a
    // gather. What it pays instead is a second region - the materialization -
    // and one random write per update.
    // local_sync is the mode that owns its own two files; sync() is simply
    // never called here, because the field engine in this sweep does not sync
    // either and anything else would be measuring fsync, not the architecture.
    using old_spec = storage::storage_spec<storage::durability::local_sync, "collapse_old.db",
                                           2ull << 30, "", "collapse_old.idx", "", 2'000'000>;

    void run_ours() {
        std::remove("collapse_old.db"); std::remove("collapse_old.db.data"); std::remove("collapse_old.idx");
        {
            storage::storage<old_spec> t;
            rec r{};
            std::memset(r.payload, 0xAB, sizeof(r.payload));
            std::vector<storage::record_id> ids;
            ids.reserve(n_records);
            for (std::size_t i = 0; i < n_records; ++i) { r.key = i + 1; ids.push_back(t.create(r)); }
            const std::size_t after_fill = t.current_offset();

            xorshift rng(11);
            std::vector<storage::record_id> hot;
            hot.reserve(n_touch);
            for (std::size_t i = 0; i < n_touch; ++i) hot.push_back(ids[rng.range(n_records)]);

            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t round = 0; round < rounds; ++round)
                for (std::size_t i = 0; i < n_touch; ++i)
                    t.patch(hot[i], (uint32_t)(offsetof(rec, f) + round * 8),
                            std::as_bytes(std::span{&round, 1}));
            auto t1 = std::chrono::steady_clock::now();
            const double update_s = secs(t0, t1);

            // Two read numbers, because one alone would mislead. The field
            // engine's get() ASSEMBLES the record into the caller's buffer -
            // it has no choice, the fields are scattered down the log - so the
            // comparable measurement is the one that also materializes 1024
            // bytes here. The pointer number is reported beside it because it
            // is what this architecture actually offers and dropping it would
            // understate the whole reason for keeping records contiguous.
            volatile uint64_t sink = 0;
            rec out{};
            // The destination is read through a volatile pointer INSIDE the
            // loop on purpose. Copying into a plain local and then reading one
            // field of it measured 99M/s here - the same as the bare pointer,
            // because the copies are dead stores and -O3 deletes all but the
            // last. This makes the destination unknowable, so the 1024 bytes
            // actually move, which is what the field engine's get() has to do.
            rec* volatile dst = &out;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_touch; ++i) { std::memcpy((void*)dst, t.get<rec>(hot[i]), sizeof(rec)); sink = out.f[0]; }
            t1 = std::chrono::steady_clock::now();
            const double read_s = secs(t0, t1);
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_touch; ++i) { std::memcpy((void*)dst, t.get<rec>(hot[i]), sizeof(rec)); sink = out.f[0]; }
            t1 = std::chrono::steady_clock::now();
            const double read2_s = secs(t0, t1);
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_touch; ++i) sink = t.get<rec>(hot[i])->f[0];
            t1 = std::chrono::steady_clock::now();
            const double ptr_s = secs(t0, t1);
            (void)sink;

            std::printf("  %-22s %9.0f upd/s %9.0f rd/s %9.0f rd2/s  log +%5.0f MB  depth  n/a   %.0f rd/s by pointer, plus a %.0f MB data region\n",
                        "ours (in place)", (n_touch * rounds) / update_s, n_touch / read_s, n_touch / read2_s,
                        (t.current_offset() - after_fill) / 1048576.0, n_touch / ptr_s,
                        (double)(n_records * value_bytes) / 1048576.0);
            std::fflush(stdout);
        }
        std::remove("collapse_old.db"); std::remove("collapse_old.db.data"); std::remove("collapse_old.idx");
    }

    // The unified engine. One 16-byte slot expresses both designs, and the
    // fault probe decides per update which one a record gets. data_cap 0 takes
    // the materialization away entirely, which should land on the field
    // baseline; permille 0 keeps the region but never patches it, which is the
    // "contiguous when created, append from then on" middle.
    template <std::size_t data_cap, std::size_t permille>
    using hyb_spec = storage::hybrid_spec<"hyb.log", capacity, "hyb.dat", data_cap, "hyb.idx",
                                          max_ids, max_overrides, 8, 2048, "", 4096, permille>;

    template <std::size_t data_cap, std::size_t permille>
    void run_hybrid(const char* label) {
        std::remove("hyb.log"); std::remove("hyb.dat"); std::remove("hyb.idx");
        {
            auto t = std::make_unique<storage::hybrid_storage<hyb_spec<data_cap, permille>>>();
            if (!t->ok()) { std::printf("  %-22s FAILED to map\n", label); return; }
            rec r{};
            std::memset(r.payload, 0xAB, sizeof(r.payload));
            std::vector<storage::record_id> ids;
            ids.reserve(n_records);
            for (std::size_t i = 0; i < n_records; ++i) { r.key = i + 1; ids.push_back(t->create(r)); }
            const std::size_t after_fill = t->current_offset();

            xorshift rng(11);
            std::vector<storage::record_id> hot;
            hot.reserve(n_touch);
            for (std::size_t i = 0; i < n_touch; ++i) hot.push_back(ids[rng.range(n_records)]);

            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t round = 0; round < rounds; ++round)
                for (std::size_t i = 0; i < n_touch; ++i)
                    t->patch(hot[i], (uint32_t)(offsetof(rec, f) + round * 8),
                             std::as_bytes(std::span{&round, 1}));
            auto t1 = std::chrono::steady_clock::now();
            const double update_s = secs(t0, t1);

            volatile uint64_t sink = 0;
            rec out{};
            rec* volatile dst = &out;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_touch; ++i) { (void)t->get(hot[i], *dst); sink = out.f[0]; }
            t1 = std::chrono::steady_clock::now();
            const double read_s = secs(t0, t1);
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_touch; ++i) { (void)t->get(hot[i], *dst); sink = out.f[0]; }
            t1 = std::chrono::steady_clock::now();
            const double read2_s = secs(t0, t1);

            // The pointer read, where it exists: null for any record that has
            // drifted into a chain, so this also counts how many did.
            std::size_t by_pointer = 0;
            t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n_touch; ++i) {
                const rec* p = t->template peek<rec>(hot[i]);
                if (p) { sink = p->f[0]; ++by_pointer; }
            }
            t1 = std::chrono::steady_clock::now();
            const double ptr_s = secs(t0, t1);
            (void)sink;

            std::size_t total_depth = 0;
            for (std::size_t i = 0; i < n_touch; ++i) total_depth += t->depth(hot[i]);

            std::printf("  %-22s %9.0f upd/s %9.0f rd/s %9.0f rd2/s  log +%5.0f MB  depth %4.1f  "
                        "%.0f rd/s ptr (%zu%% hit), in-place %zu / appended %zu, switches %zu, data %.0f MB\n",
                        label, (n_touch * rounds) / update_s, n_touch / read_s, n_touch / read2_s,
                        (t->current_offset() - after_fill) / 1048576.0, (double)total_depth / n_touch,
                        by_pointer / ptr_s, by_pointer * 100 / n_touch,
                        t->in_place_update_count(), t->appended_update_count(), t->mode_switch_count(),
                        t->materialized_bytes() / 1048576.0);
            std::fflush(stdout);
        }
        std::remove("hyb.log"); std::remove("hyb.dat"); std::remove("hyb.idx");
    }

} // namespace

int main(int argc, char** argv) {
    const double gb = argc > 1 ? std::strtod(argv[1], nullptr) : 0.5;
    n_records = (std::size_t)(gb * 1073741824.0 / value_bytes);
    n_touch = n_records / 4;

    std::printf("%.1f GB: %zu records of %zu bytes, %zu hot records\n\n",
                gb, n_records, value_bytes, n_touch);
    std::printf("  %-18s %16s %14s %14s %14s %10s %18s %14s\n",
                "variant", "update", "read", "read again", "log growth", "depth", "collapses", "maj faults");

    // One variant per invocation: each rewrites the whole dataset, so a
    // sweep in one process is bounded by the disk rather than by anything it
    // measures.
    const std::string v = argc > 2 ? argv[2] : "d1";
    if (v == "curve") {
        for (std::size_t r : { 1, 2, 4, 8, 16, 32 }) {
            rounds = r;
            char label[40];
            std::snprintf(label, sizeof(label), "depth %zu, no collapse", r);
            run<64>(label, 0);
        }
        return 0;
    }
    if (v == "ours")        run_ours();
    else if (v == "hybrid") run_hybrid<capacity, 20>("hybrid (auto)");
    else if (v == "hyb-nf") run_hybrid<capacity, 1000000>("hybrid (never gives up)");
    else if (v == "hyb-fld") run_hybrid<0, 20>("hybrid (no data region)");
    else if (v == "hyb-cre") run_hybrid<capacity, 0>("hybrid (contiguous create only)");
    else if (v == "d1")     run<1>("A: depth 1 on write", 0);
    else if (v == "d4")     run<4>("A: depth 4 on write", 0);
    else if (v == "none")   run<64>("baseline: no collapse", 0);
    else if (v == "read4")  run<64>("B: collapse on read >4", 4);
    else if (v == "sweep")  run<64>("C: sorted sweep, off-path", 0, between::sweep_collapse, 4);
    else if (v == "chain")  run<64>("D: compact chain >4", 0, between::chain_compact, 4);
    else if (v == "compact")run<64>("E: full compaction only", 0, between::full_compaction);
    else if (v == "span")   run<64, 1u << 20>("F: span > 1 MB", 0);
    else if (v == "span64") run<64, 64u << 20>("G: span > 64 MB", 0);
    return 0;
}
