// probe_restart_cost.cpp - how long rebuilding state from the log takes.
// This is the number that decides V2 (materialized data file, fast restart)
// vs V3 (anonymous memory, full replay) - see probe_split_log_and_data.cpp.
//
// MEASURED (btrfs, NVMe):
//    128 MB log,  1.5M records | warm 0.021 s | cold 0.042 s (3031 MB/s)
//    512 MB log,  6.1M records | warm 0.086 s | cold 0.181 s (2823 MB/s)
//   2048 MB log, 24.4M records | warm 0.354 s | cold 0.980 s (2089 MB/s)
//
// Cold is the honest number: a real restart reads from the device, not from
// page cache (forced here with posix_fadvise(DONTNEED), no root needed).
// Warm ~5.8 GB/s is the parse rate; cold ~2 GB/s is the device. So recovery
// is I/O bound, not CPU bound - it is already about as fast as it can be,
// and it scales linearly: roughly 0.5 s per GB of log.
// How long does rebuilding state from the log actually take?
//
// This is the number that decides V2 (log + materialized data file, fast
// restart) vs V3 (log + anonymous memory, full replay on restart).
//
// Measured warm AND cold. Warm is what stress_heap reports (~4.7 GB/s) and
// it is misleading for this question: a real restart after a crash or reboot
// reads from the device, not from page cache. Cold is forced with
// posix_fadvise(DONTNEED), which needs no root.
#include "../storage/storage.hpp"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using spec = storage::storage_spec<storage::durability::local_sync, "p5.db", 6ull << 30>;
struct rec { static constexpr storage::tag_t tag = 1; uint64_t key; uint8_t pad[56]; };

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void drop_cache(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
}

void run(size_t target_mb) {
    std::remove("p5.db");
    size_t written = 0, records = 0;
    {
        storage::storage<spec> t;
        rec r{};
        while (t.current_offset() < target_mb * 1024 * 1024) {
            r.key = records++;
            t.create(r);
        }
        t.sync();
        written = t.current_offset();
    }

    // --- warm: the log is still entirely in page cache ---
    double warm;
    {
        storage::storage<spec> t;
        auto t0 = std::chrono::steady_clock::now();
        t.recover(written);
        auto t1 = std::chrono::steady_clock::now();
        warm = secs(t0, t1);
    }

    // --- cold: force the pages out first, like a real restart ---
    drop_cache("p5.db");
    double cold;
    {
        storage::storage<spec> t;
        auto t0 = std::chrono::steady_clock::now();
        t.recover(written);
        auto t1 = std::chrono::steady_clock::now();
        cold = secs(t0, t1);
    }

    double mb = written / (1024.0 * 1024.0);
    std::printf("%7.0f MB log  %8zu records | warm %6.3f s (%7.0f MB/s) | cold %6.3f s (%7.0f MB/s)\n",
                mb, records, warm, mb / warm, cold, mb / cold);
    std::remove("p5.db");
}

int main() {
    std::printf("recover() throughput - this is V3's restart cost\n\n");
    for (size_t mb : {128, 512, 2048}) run(mb);
    return 0;
}
