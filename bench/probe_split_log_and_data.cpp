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
//   V4 one file, 2 maps   ONE file, but reached through two mappings: a
//                    MAP_SHARED window used only to append the log, and a
//                    MAP_PRIVATE view used for the in-place patches. A write
//                    to a private mapping copies-on-write into anonymous
//                    memory, so it never dirties the file -- the inode's only
//                    dirty pages are the log tail, which is what we wanted,
//                    without a second file and without giving up demand-paging
//                    of cold records.
//   V5 one file, pwrite   Same private view for patches, but the log is built
//                    in a userspace buffer and appended with pwrite(), so the
//                    file has no writable mapping at all. Durability is a
//                    plain fdatasync().
//   V6 V5, dual-write     V5 relies on a MAP_PRIVATE page that has not been
//                    copied yet reading through to the page cache, so it sees
//                    the pwrite. Linux does that, but POSIX calls it
//                    unspecified. V6 removes the dependency: every appended
//                    record is written BOTH to the pwrite buffer and into the
//                    private mapping, so the mapping is authoritative for
//                    everything this process appended and never has to observe
//                    a file write it did not make itself. This costs one extra
//                    memcpy per record plus one copy-on-write fault per page of
//                    log -- the point of measuring it is to find out how much.
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
    } else if (variant == 3) {
        log = map_file("p4_log.db", log_bytes, &log_fd);
        data = (std::byte*)::mmap(nullptr, data_bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data == MAP_FAILED) { perror("mmap anon"); exit(1); }
    } else {
        // V4/V5: one file, same layout as V1 (records at the front, log after
        // them). The difference is purely how each region is reached.
        log = map_file("p4_single.db", log_bytes + data_bytes, &log_fd);
        // V6 patches AND appends through the private view, so it needs to span
        // the log region too; V4/V5 only ever touch records there.
        const size_t private_bytes = variant == 6 ? log_bytes + data_bytes : data_bytes;
        data = (std::byte*)::mmap(nullptr, private_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, log_fd, 0);
        if (data == MAP_FAILED) { perror("mmap private"); exit(1); }
    }

    // Materialize all records first (the "create" phase), and get the log to
    // a realistic starting offset.
    size_t log_off = 0;
    if (variant == 1 || variant >= 4) log_off = data_bytes; // records occupy the front
    for (size_t i = 0; i < n; ++i) {
        // V4/V5 must materialize through the SHARED view, so the bytes land in
        // the file; the private view is left untouched until the measured loop
        // so that it faults them in from the page cache rather than caching a
        // stale copy.
        std::byte* dst = (variant >= 4 ? log : data) + i * rec_size;
        std::memset(dst, (int)(i & 0xFF), rec_size);
    }
    ::msync(log, variant == 1 || variant >= 4 ? log_bytes + data_bytes : log_bytes, MS_SYNC);
    if (variant == 2 || variant == 3) ::fsync(log_fd);

    std::vector<uint64_t> keys(n);
    for (size_t i = 0; i < n; ++i) keys[i] = i;
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7)); // the bad case

    size_t last_synced = log_off;
    std::vector<std::byte> log_buffer; // V5 only: batch of appends, flushed with pwrite
    if (variant >= 5) log_buffer.reserve(batch * log_rec);
    unsigned long long s0 = sectors_written();
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; ++i) {
        // 1. append a log record (sequential, in the log region/file)
        if (variant >= 5) {
            log_buffer.insert(log_buffer.end(), log_rec, (std::byte)(keys[i] & 0xFF));
            // V6: the private mapping is the authoritative copy of the log too.
            if (variant == 6) std::memset(data + log_off, (int)(keys[i] & 0xFF), log_rec);
        } else std::memset(log + log_off, (int)(keys[i] & 0xFF), log_rec);
        log_off += log_rec;
        // 2. patch the materialized record in place (random page). For V4/V5
        // this write copies-on-write into anonymous memory instead of dirtying
        // the file, which is the whole point of the private view.
        std::memcpy(data + keys[i] * rec_size, &keys[i], sizeof(keys[i]));

        if ((i + 1) % batch == 0) {
            // Durability point: commit the log only.
            if (variant >= 5) {
                (void)!::pwrite(log_fd, log_buffer.data(), log_buffer.size(), (off_t)last_synced);
                log_buffer.clear();
                ::fdatasync(log_fd);
            } else {
                std::size_t start = last_synced & ~(std::size_t)4095;
                ::msync(log + start, log_off - start, MS_SYNC);
            }
            last_synced = log_off;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    unsigned long long s1 = sectors_written();

    double s = secs(t0, t1);
    std::printf("  %-22s %7.3f s  %10.0f ops/s  %8llu MiB written\n",
                label, s, n / s, (s1 - s0) * 512 / (1024 * 1024));

    if (variant == 3 || variant >= 4) ::munmap(data, variant == 6 ? log_bytes + data_bytes : data_bytes);
    else if (data_fd >= 0) { ::munmap(data, data_bytes); ::close(data_fd); }
    ::munmap(log, variant == 1 || variant >= 4 ? log_bytes + data_bytes : log_bytes);
    ::close(log_fd);
    std::remove("p4_single.db"); std::remove("p4_log.db"); std::remove("p4_data.db");
}

int main() {
    std::printf("200k random durable updates, 200 flush points, btrfs\n\n");
    run("V1 one file (today)", 1);
    run("V2 log + data file", 2);
    run("V3 log + anon memory", 3);
    run("V4 one file, 2 mappings", 4);
    run("V5 one file, pwrite log", 5);
    run("V6 V5 + dual-write log", 6);
    return 0;
}
