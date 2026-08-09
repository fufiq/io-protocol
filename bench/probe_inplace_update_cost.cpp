// Why are durable RANDOM updates slow, when sync() only msyncs the log tail?
//
// Variants, all with identical update counts:
//   A seq  + msync(tail, MS_SYNC)   <- what sync() actually does
//   B rand + msync(tail, MS_SYNC)
//   C rand + no msync at all
//   D rand + msync(tail, MS_ASYNC)
//   E rand + msync(WHOLE FILE, MS_SYNC)
//
// Plus device sectors written per variant, which is the measurement that
// actually distinguishes "msync flushed more" from "the kernel wrote more
// behind our back".
#include "../storage/storage.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <sys/mman.h>
#include <vector>

using spec = storage::storage_spec<storage::durability::local_sync, "probe2.db", 1ull << 30>;
struct rec { static constexpr storage::tag_t tag = 1; uint64_t key; uint8_t pad[56]; };

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// sectors written to nvme0n1 (field 7 of /proc/diskstats), 512B each
unsigned long long sectors_written() {
    std::ifstream f("/proc/diskstats");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(" nvme0n1 ") == std::string::npos) continue;
        unsigned long long v[17]{};
        char name[64];
        int major, minor;
        if (std::sscanf(line.c_str(), "%d %d %63s %llu %llu %llu %llu %llu %llu %llu",
                        &major, &minor, name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) >= 10)
            return v[6]; // sectors written
    }
    return 0;
}

enum class mode { seq_sync, rand_sync, rand_nosync, rand_async, rand_fullsync };

void run(const char* label, mode m) {
    constexpr size_t n = 200'000, batch = 1000;
    std::remove("probe2.db");
    {
        storage::storage<spec> t;
        rec r{};
        for (size_t i = 0; i < n; ++i) { r.key = i + 1; t.create(r); }
        t.sync();

        std::vector<uint64_t> keys(n);
        for (size_t i = 0; i < n; ++i) keys[i] = i + 1;
        if (m != mode::seq_sync) std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7));

        // Snapshot device counters only around the update phase.
        unsigned long long sec0 = sectors_written();
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < n; ++i) {
            t.update<&rec::key>(keys[i], keys[i]);
            if ((i + 1) % batch == 0) {
                switch (m) {
                    case mode::seq_sync:
                    case mode::rand_sync:      t.sync(); break;
                    case mode::rand_nosync:    break;
                    case mode::rand_async:     ::msync(t.raw_base(), t.current_offset(), MS_ASYNC); break;
                    case mode::rand_fullsync:  ::msync(t.raw_base(), t.current_offset(), MS_SYNC); break;
                }
            }
        }
        if (m == mode::seq_sync || m == mode::rand_sync) t.sync();
        auto t1 = std::chrono::steady_clock::now();
        unsigned long long sec1 = sectors_written();

        double s = secs(t0, t1);
        std::printf("%-22s %7.3f s  %10.0f ops/s  %8.2f ms/sync  %8llu MiB written\n",
                    label, s, n / s, 1000.0 * s / (n / batch), (sec1 - sec0) * 512 / (1024 * 1024));
    }
    std::remove("probe2.db");
}

int main() {
    std::printf("200k updates, 200 flush points, 64B records, btrfs\n\n");
    run("A seq  + msync tail", mode::seq_sync);
    run("B rand + msync tail", mode::rand_sync);
    run("C rand + no msync", mode::rand_nosync);
    run("D rand + MS_ASYNC", mode::rand_async);
    run("E rand + msync WHOLE", mode::rand_fullsync);
    return 0;
}
