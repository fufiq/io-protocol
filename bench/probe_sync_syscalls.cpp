// Is the write amplification btrfs-CoW specific, and do file-level flags or
// a narrower syscall avoid it?
//
//   B  msync(tail, MS_SYNC)                 <- what sync() does today
//   F  sync_file_range(tail, WRITE|WAIT)    <- range writeback, no fs commit
//   G  sync_file_range(tail) + fsync at end <- range writeback + one commit
//
// Run once in a normal directory and once in a `chattr +C` (nodatacow)
// directory; new files inherit the flag, so the same code exercises both.
#include "../storage/storage.hpp"
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using spec = storage::storage_spec<storage::durability::local_sync, "probe3.db", 1ull << 28>;
struct rec { static constexpr storage::tag_t tag = 1; uint64_t key; uint8_t pad[56]; };

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

unsigned long long sectors_written() {
    std::ifstream f("/proc/diskstats");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(" nvme0n1 ") == std::string::npos) continue;
        unsigned long long v[8]{}; char name[64]; int mj, mn;
        if (std::sscanf(line.c_str(), "%d %d %63s %llu %llu %llu %llu %llu %llu %llu",
                        &mj, &mn, name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) >= 10)
            return v[6];
    }
    return 0;
}

enum class mode { msync_tail, sfr_only, sfr_plus_fsync };

void run(const char* label, mode m) {
    constexpr size_t n = 200'000, batch = 1000;
    std::remove("probe3.db");
    {
        storage::storage<spec> t;
        rec r{};
        for (size_t i = 0; i < n; ++i) { r.key = i + 1; t.create(r); }
        t.sync();

        // Our own fd on the same file, for the sync_file_range variants.
        int fd = ::open("probe3.db", O_RDWR);

        std::vector<uint64_t> keys(n);
        for (size_t i = 0; i < n; ++i) keys[i] = i + 1;
        std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7)); // always random: that's the bad case

        std::size_t last = t.current_offset();
        unsigned long long s0 = sectors_written();
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < n; ++i) {
            t.update<&rec::key>(keys[i], keys[i]);
            if ((i + 1) % batch == 0) {
                std::size_t now = t.current_offset();
                switch (m) {
                    case mode::msync_tail:
                        t.sync();
                        break;
                    case mode::sfr_only:
                    case mode::sfr_plus_fsync:
                        ::sync_file_range(fd, (off_t)last, (off_t)(now - last),
                                          SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
                        break;
                }
                last = now;
            }
        }
        if (m == mode::sfr_plus_fsync) ::fsync(fd);
        auto t1 = std::chrono::steady_clock::now();
        unsigned long long s1 = sectors_written();
        ::close(fd);

        double s = secs(t0, t1);
        std::printf("  %-26s %7.3f s  %10.0f ops/s  %8llu MiB written\n",
                    label, s, n / s, (s1 - s0) * 512 / (1024 * 1024));
    }
    std::remove("probe3.db");
}

int main(int argc, char**) {
    std::printf("%s\n", argc > 1 ? "" : "");
    run("B msync(tail, MS_SYNC)", mode::msync_tail);
    run("F sync_file_range only", mode::sfr_only);
    run("G sync_file_range + fsync", mode::sfr_plus_fsync);
    return 0;
}
