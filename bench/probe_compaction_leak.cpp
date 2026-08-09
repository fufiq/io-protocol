// probe_compaction_leak.cpp - does a compaction leak the region it replaced?
//
// MEASURED, 64 MB storage, 20 compactions:
//   before adopt_region()   VmSize +2560 MB, mappings 42 -> 82
//   after                   VmSize     +0 MB, mappings 42 -> 42
//
// 2560 MB is exactly 20 * 2 * 64 MB: every compaction was stranding both the
// log and the data mapping, because base/data_base were simply reassigned.
//
// Build: g++ -std=c++23 -O2 -march=native -o probe bench/probe_compaction_leak.cpp
//
// Does a compaction still leak its old mapping? Address space is the
// measurement: a leaked mapping stays in VmSize even after the pointer is
// gone, and it is capacity-sized, so 20 compactions of a 64 MB storage would
// show up as over a gigabyte.
#include "../storage/storage.hpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <fcntl.h>

using spec = storage::storage_spec<storage::durability::local_sync, "leakchk.db", 64ull * 1024 * 1024>;
struct rec { static constexpr storage::tag_t tag = 1; uint64_t v; };

std::size_t vm_size_kb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    while (f >> k) { if (k == "VmSize:") { std::size_t v; f >> v; return v; } }
    return 0;
}
std::size_t mapping_count() {
    std::ifstream f("/proc/self/maps");
    std::string line; std::size_t n = 0;
    while (std::getline(f, line)) ++n;
    return n;
}

std::byte* fresh_region(const char* path) {
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ::posix_fallocate(fd, 0, spec::capacity);
    std::byte* p = (std::byte*)::mmap(nullptr, spec::capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd); // the mapping keeps the inode alive
    return p;
}

int main() {
    storage::storage<spec> table;
    for (int i = 0; i < 500; ++i) table.create(rec{(uint64_t)i});

    std::printf("            VmSize        mappings\n");
    std::size_t v0 = vm_size_kb(), m0 = mapping_count();
    std::printf("  start   %8zu MB   %8zu\n", v0 / 1024, m0);

    for (int round = 1; round <= 20; ++round) {
        table.compact(fresh_region("leakchk.log.tmp"), fresh_region("leakchk.data.tmp"));
        if (round % 5 == 0)
            std::printf("  after %2d compactions %5zu MB   %8zu\n", round, vm_size_kb() / 1024, mapping_count());
    }
    std::size_t v1 = vm_size_kb();
    std::printf("\n  growth over 20 compactions: %zu MB (each region is %zu MB;\n"
                "  a leak would show ~%zu MB)\n",
                (v1 - v0) / 1024, (std::size_t)spec::capacity / (1024 * 1024),
                20 * 2 * (std::size_t)spec::capacity / (1024 * 1024));

    int alive = 0;
    table.for_each_raw([&](storage::storage<spec>::raw_view) { ++alive; });
    std::printf("  records still readable after all that: %d\n", alive);
    std::remove("leakchk.db"); std::remove("leakchk.db.data");
    std::remove("leakchk.log.tmp"); std::remove("leakchk.data.tmp");
    return 0;
}
