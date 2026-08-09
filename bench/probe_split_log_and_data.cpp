// probe_split_log_and_data.cpp - measures the "give the log its own inode"
// idea: fs commits are per-inode, so separating the append-only log from the
// in-place materialization should make an fsync of the log cheap regardless
// of how scattered the data writes are. It does. See the results below.
//
// MEASURED (btrfs, 200k random durable updates, 200 flush points):
//   V1 one file (today)     2.94 s    68K ops/s   1019 MiB written
//   V2 log + data file      0.38 s   528K ops/s     37 MiB written
//   V3 log + anon memory    0.20 s   993K ops/s     37 MiB written
//
// Caveat on V2: 37 MiB is the log only. The data file's dirty pages are still
// written eventually by background writeback - V2 moves that cost off the
// critical path rather than removing it. V3 removes it, at the price of
// needing a full log replay to rebuild state on restart.
//
// Build: g++ -std=c++23 -O3 -march=native -o probe bench/probe_split_log_and_data.cpp
// Does splitting the log and the in-place materialization into separate
// inodes actually make durable random updates cheap?
//
// The theory: fs commits are per-inode, not per-range. Today the WAL record
// and the materialized value are the SAME bytes in the SAME file, so an
// fsync of the log tail drags the whole inode's scattered dirty pages into
// the commit. Give them separate inodes and the log's fsync should only ever
// see its own small, sequential tail.
//
//   V1 one file      append log + patch in place in that same file, msync tail
//   V2 two files     append log (fsync) + patch a separate mmap'd data file
//   V3 log + anon    append log (fsync) + patch ANONYMOUS memory (no data file)
//
// All three do the same number of updates and the same number of durability
// points, and all three keep the same guarantee: on return from the flush,
// the log is durable. Only where the materialized value lives differs.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

constexpr size_t n = 200'000, batch = 1000;
constexpr size_t rec_size = 64;      // materialized record
constexpr size_t log_rec = 32;       // header + patched field, appended per update
constexpr size_t data_bytes = n * rec_size;
constexpr size_t log_bytes = (n + n) * log_rec + (1u << 20);

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

std::byte* map_file(const char* path, size_t bytes, int* out_fd) {
    std::remove(path);
    int fd = ::open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    if (::posix_fallocate(fd, 0, bytes) != 0) { perror("fallocate"); exit(1); }
    void* m = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    *out_fd = fd;
    return (std::byte*)m;
}

void run(const char* label, int variant) {
    int log_fd = -1, data_fd = -1;
    std::byte* log = nullptr;
    std::byte* data = nullptr;

    if (variant == 1) {
        // One file holding both: the "record" lives in the log region and is
        // patched there, exactly like the real engine does today.
        log = map_file("p4_single.db", log_bytes + data_bytes, &log_fd);
        data = log; // materialization is inside the log itself
    } else if (variant == 2) {
        log = map_file("p4_log.db", log_bytes, &log_fd);
        data = map_file("p4_data.db", data_bytes, &data_fd);
    } else {
        log = map_file("p4_log.db", log_bytes, &log_fd);
        data = (std::byte*)::mmap(nullptr, data_bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data == MAP_FAILED) { perror("mmap anon"); exit(1); }
    }

    // Materialize all records first (the "create" phase), and get the log to
    // a realistic starting offset.
    size_t log_off = 0;
    if (variant == 1) log_off = data_bytes; // records occupy the front in V1
    for (size_t i = 0; i < n; ++i) std::memset(data + i * rec_size, (int)(i & 0xFF), rec_size);
    ::msync(log, log_bytes, MS_SYNC);
    if (variant != 1) ::fsync(log_fd);

    std::vector<uint64_t> keys(n);
    for (size_t i = 0; i < n; ++i) keys[i] = i;
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7)); // the bad case

    size_t last_synced = log_off;
    unsigned long long s0 = sectors_written();
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; ++i) {
        // 1. append a log record (sequential, in the log region/file)
        std::memset(log + log_off, (int)(keys[i] & 0xFF), log_rec);
        log_off += log_rec;
        // 2. patch the materialized record in place (random page)
        std::memcpy(data + keys[i] * rec_size, &keys[i], sizeof(keys[i]));

        if ((i + 1) % batch == 0) {
            // Durability point: flush the log only.
            std::size_t start = last_synced & ~(std::size_t)4095;
            ::msync(log + start, log_off - start, MS_SYNC);
            last_synced = log_off;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    unsigned long long s1 = sectors_written();

    double s = secs(t0, t1);
    std::printf("  %-22s %7.3f s  %10.0f ops/s  %8llu MiB written\n",
                label, s, n / s, (s1 - s0) * 512 / (1024 * 1024));

    if (variant == 3) ::munmap(data, data_bytes);
    else if (data_fd >= 0) { ::munmap(data, data_bytes); ::close(data_fd); }
    ::munmap(log, variant == 1 ? log_bytes + data_bytes : log_bytes);
    ::close(log_fd);
    std::remove("p4_single.db"); std::remove("p4_log.db"); std::remove("p4_data.db");
}

int main() {
    std::printf("200k random durable updates, 200 flush points, btrfs\n\n");
    run("V1 one file (today)", 1);
    run("V2 log + data file", 2);
    run("V3 log + anon memory", 3);
    return 0;
}
