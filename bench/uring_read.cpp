// uring_read.cpp - what reading through a ring costs, and what it buys.
//
// Three paths over the SAME field storage, so the only variable is how the
// bytes get from the file into the caller's object:
//
//   mmap     t.get(id, out) - the assemble() the engine has always used. A
//            dereference per piece; a miss is a page fault, and a fault is one
//            request at a time by construction.
//   pread    the same pieces, one pread() syscall each, still strictly one
//            request at a time. This isolates the syscall cost from the depth,
//            which is the thing the ring is actually for.
//   uring    the same pieces submitted to a ring, up to `depth` outstanding.
//
// Run it on a dataset that FITS in RAM and it measures overhead, not benefit:
// every read is served from the page cache, there is no device latency to hide,
// and depth cannot help because nothing is being waited on. That is a number
// worth knowing - it is the price of the option - but it is not evidence about
// the out-of-RAM case, and it will be reported as what it is.

#include "../storage/uring_gather.hpp"
#include "../storage/direct_gather.hpp"
#include "../storage/prefetch_gather.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {

    constexpr std::size_t value_bytes = 1024;
    constexpr std::size_t log_capacity = 4ull << 30;
    constexpr std::size_t max_ids = 600'000;

    struct blob {
        static constexpr storage::tag_t tag = 1;
        uint64_t key, counter, f1, f2, f3, f4;
        uint8_t payload[value_bytes - 48];
    };
    static_assert(sizeof(blob) == value_bytes);

    using bench_spec = storage::field_spec<"ur.log", log_capacity, "ur.idx", max_ids, 1'200'000, 8, value_bytes>;

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
        struct rusage ru;
        return ::getrusage(RUSAGE_SELF, &ru) == 0 ? (std::size_t)ru.ru_majflt : 0;
    }

    void row(const char* label, std::size_t ops, double s, std::size_t faults, const char* note = "") {
        std::printf("  %-26s %10zu ops %8.3f s %12.0f ops/sec  %7zu maj faults  %s\n",
                    label, ops, s, ops / s, faults, note);
        std::fflush(stdout);
    }

} // namespace

int main(int argc, char** argv) {
    const double mb = argc > 1 ? std::strtod(argv[1], nullptr) : 512.0;
    const std::size_t n_records = (std::size_t)(mb * 1048576.0 / value_bytes);
    const std::size_t n_reads = argc > 2 ? (std::size_t)std::strtoull(argv[2], nullptr, 10) : 200'000;
    const std::size_t avg_depth = argc > 3 ? (std::size_t)std::strtoull(argv[3], nullptr, 10) : 4;

    std::printf("%.0f MB: %zu records of %zu bytes, %zu random reads, up to %zu overridden fields each\n",
                mb, n_records, value_bytes, n_reads, avg_depth);

    std::remove("ur.log"); std::remove("ur.idx");
    auto holder = std::make_unique<storage::field_storage<bench_spec>>();
    auto& t = *holder;
    if (!t.ok()) { std::printf("could not map the log\n"); return 1; }

    blob b{};
    std::memset(b.payload, 0xAB, sizeof(b.payload));
    std::vector<storage::record_id> ids;
    ids.reserve(n_records);
    for (std::size_t i = 0; i < n_records; ++i) { b.key = i + 1; b.counter = i; ids.push_back(t.create(b)); }

    // Overrides, so the gather has something to gather. Without them every
    // record is one piece and the whole intra-record parallelism question
    // never arises.
    for (std::size_t i = 0; i < n_records; ++i) {
        const std::size_t d = i % (avg_depth + 1);
        if (d > 0) t.update<&blob::f1>(ids[i], i);
        if (d > 1) t.update<&blob::f3>(ids[i], i);
        if (d > 2) t.update<&blob::counter>(ids[i], i);
        if (d > 3) t.update<&blob::f4>(ids[i], i);
    }

    // A FRESH set of records for every path, and the log's pages dropped
    // before each one. Sharing one set made the first path pay every fault and
    // handed the rest a warm cache: the ring "read" 43667 pieces in 0.016 s,
    // which is 2.7M reads/sec against a device measured at 239k IOPS - it was
    // reading memory. 10000 records is 10 MB, so one path warms the entire
    // working set for its successors however large the dataset is.
    //
    // Distinct picks make the overlap between paths about 2% at these sizes,
    // and release_log() drops what the previous path left behind. Correctness
    // is not re-checked here: the gather is verified byte-for-byte against
    // assemble() under ASan/UBSan in its own test, and comparing paths that
    // read different records would need a reference per path, which would warm
    // the cache all over again.
    auto fresh_picks = [&](uint64_t seed) {
        xorshift rng(seed);
        std::vector<storage::record_id> p;
        p.reserve(n_reads);
        for (std::size_t i = 0; i < n_reads; ++i) p.push_back(ids[rng.range(n_records)]);
        return p;
    };
    t.sync();                       // so the log's pages are clean and therefore droppable
    auto cold = [&] { t.release_log(t.current_offset()); };

    std::vector<std::byte> out(n_reads * value_bytes);
    volatile uint64_t sink = 0;

    std::printf("index: %.0f MB, slot %zu bytes, pinned: %s\n",
                bench_spec::index_bytes / 1048576.0, sizeof(storage::field_slot),
                t.pin_index() ? "mlock" : "MADV_WILLNEED only (RLIMIT_MEMLOCK)");
    std::printf("\n  %-26s %10s %10s %14s %20s\n", "path", "ops", "time", "rate", "");

    // ---- mmap ----------------------------------------------------------
    {
        auto picks = fresh_picks(101);
        cold();
        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_reads; ++i)
            (void)t.read_raw(picks[i], std::span<std::byte>{ out.data() + i * value_bytes, value_bytes });
        auto t1 = std::chrono::steady_clock::now();
        sink = (uint64_t)out[0];
        row("mmap assemble", n_reads, secs(t0, t1), major_faults() - f0, "depth 1 by construction");
    }

    // ---- mmap, with the slots prefetched a batch ahead -------------------
    {
        auto picks = fresh_picks(150);
        cold();
        constexpr std::size_t lookahead = 16;
        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < lookahead && k < n_reads; ++k) t.prefetch(picks[k]);
        for (std::size_t i = 0; i < n_reads; ++i) {
            if (i + lookahead < n_reads) t.prefetch(picks[i + lookahead]);
            (void)t.read_raw(picks[i], std::span<std::byte>{ out.data() + i * value_bytes, value_bytes });
        }
        auto t1 = std::chrono::steady_clock::now();
        sink = (uint64_t)out[0];
        row("mmap + slot prefetch", n_reads, secs(t0, t1), major_faults() - f0, "depth 1, slots pulled ahead");
    }

    // ---- mmap with the pages advised a window ahead ----------------------
    //
    // Same read path, same mapping, same assemble(). The only difference is
    // that the kernel is told which pages the next window will want before the
    // current one is assembled, so several reads are in flight instead of one.
    for (std::size_t window : { 16u, 64u, 256u }) {
        auto picks = fresh_picks(400 + window);
        storage::prefetch_gather g(window);
        cold();
        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        auto r = g.read_many(t, std::span<const storage::record_id>{ picks }, out.data(), value_bytes);
        auto t1 = std::chrono::steady_clock::now();
        sink = (uint64_t)out[0];
        char note[112];
        std::snprintf(note, sizeof(note), "%zu pieces -> %zu advises, %.1f MB advised",
                      r.pieces, r.advises, r.advised_bytes / 1048576.0);
        char label[48];
        std::snprintf(label, sizeof(label), "mmap + WILLNEED w=%zu", window);
        row(label, n_reads, secs(t0, t1), major_faults() - f0, note);
    }

    // ---- pread ---------------------------------------------------------
    {
        auto picks = fresh_picks(202);
        cold();
        const int fd = t.log_fd();
        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_reads; ++i) {
            std::byte* dst = out.data() + i * value_bytes;
            t.for_each_piece(picks[i], [&](std::size_t from, uint32_t at, uint32_t len) {
                (void)::pread(fd, dst + at, len, (off_t)from);
            });
        }
        auto t1 = std::chrono::steady_clock::now();
        sink = (uint64_t)out[0];
        row("pread per piece", n_reads, secs(t0, t1), major_faults() - f0, "depth 1, syscall per piece");
    }

    // ---- io_uring ------------------------------------------------------
    for (unsigned depth : { 32u, 512u }) {
        auto picks = fresh_picks(300 + depth);
        storage::uring_gather g(depth);
        cold();
        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        auto r = g.read_many(t, std::span<const storage::record_id>{ picks }, out.data(), value_bytes);
        auto t1 = std::chrono::steady_clock::now();
        sink = (uint64_t)out[0];
        char note[96];
        std::snprintf(note, sizeof(note), "%zu reads / %zu waves%s", r.reads, r.waves,
                      r.failures ? " FAILURES" : "");
        char label[48];
        std::snprintf(label, sizeof(label), "io_uring depth %u", depth);
        row(label, n_reads, secs(t0, t1), major_faults() - f0, note);
    }

    // ---- io_uring + O_DIRECT -------------------------------------------
    //
    // sync() first, and not as a formality: the log is written through a
    // shared mapping, and an O_DIRECT read has no guarantee of seeing a
    // buffered write that has not reached the device.
    {
        auto s0 = std::chrono::steady_clock::now();
        t.sync();
        std::printf("\n  msync before O_DIRECT: %.3f s (required for coherency, not part of the read cost)\n",
                    secs(s0, std::chrono::steady_clock::now()));
    }
    for (unsigned depth : { 32u, 512u }) {
        auto picks = fresh_picks(700 + depth);
        storage::direct_gather g("ur.log", depth, value_bytes);
        if (!g.ok()) { std::printf("  O_DIRECT open failed\n"); break; }
        cold();
        const std::size_t f0 = major_faults();
        auto t0 = std::chrono::steady_clock::now();
        auto r = g.read_many(t, std::span<const storage::record_id>{ picks }, out.data(), value_bytes);
        auto t1 = std::chrono::steady_clock::now();
        sink = (uint64_t)out[0];
        char note[128];
        std::snprintf(note, sizeof(note), "%zu reads / %zu waves / %.1fx amplification%s",
                      r.reads, r.waves, (double)r.block_bytes / (n_reads * value_bytes),
                      r.failures ? " FAILURES" : "");
        char label[48];
        std::snprintf(label, sizeof(label), "O_DIRECT depth %u", depth);
        row(label, n_reads, secs(t0, t1), major_faults() - f0, note);
    }

    (void)sink;
    holder.reset();
    std::remove("ur.log"); std::remove("ur.idx");
    return 0;
}
