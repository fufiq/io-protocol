// platform_check.cpp - run this on the platform you are porting to.
//
//   c++ -std=c++23 -O2 embedded/platform_check.cpp -o platform_check && ./platform_check
//
// The Darwin branches of platform.hpp were written against Apple's documented
// behaviour and never compiled, because the machine they were written on has no
// Darwin toolchain. This is the smallest thing that decides whether they work:
// it exercises each shim against a real file and checks the property the engine
// depends on, rather than merely that the call returned zero.
//
// It also runs on Linux, where it is a regression test for the same properties
// through the other branch - so a change that breaks the contract shows up on
// the machine doing the development, not only on the one doing the port.

#include "platform.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

    int failures = 0;

    void check(bool ok, const char* what) {
        std::printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }

    off_t size_of(const char* path) {
        struct ::stat st {};
        return ::stat(path, &st) == 0 ? st.st_size : -1;
    }

    // Blocks actually reserved, in 512-byte units. This is the whole point of
    // preallocating rather than truncating: a sparse file has the right size
    // and no blocks, and then raises SIGBUS when the disk fills mid-write.
    long blocks_of(const char* path) {
        struct ::stat st {};
        return ::stat(path, &st) == 0 ? (long)st.st_blocks : -1;
    }

} // namespace

int main() {
    using namespace storage::platform;

    std::printf("page size: %zu\n", page_size());
    check(page_size() >= 4096 && (page_size() & (page_size() - 1)) == 0,
          "page size is a power of two and at least 4096");

    const char* path = "platform_check.tmp";
    std::remove(path);

    const std::size_t want = 4ull << 20;   // 4 MB
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open a fresh file");
    if (fd < 0) return 1;

    check(preallocate(fd, want) == 0, "preallocate 4 MB");
    check(size_of(path) == (off_t)want, "file is exactly 4 MB afterwards");

    // 4 MB is 8192 blocks of 512. Anything near that means real allocation;
    // near zero means a sparse file, which is the failure this call exists to
    // prevent.
    const long blocks = blocks_of(path);
    std::printf("  st_blocks = %ld (4 MB dense would be ~%d)\n", blocks, 8192);
    check(blocks > 4096, "the space is really reserved, not sparse");

    // Idempotent: the engine calls this on every open, including restarts, and
    // posix_fallocate is idempotent. On Darwin the naive F_PREALLOCATE call is
    // NOT - F_PEOFPOSMODE allocates past the current end - so a second open
    // would double the file every time. This is that regression.
    check(preallocate(fd, want) == 0, "preallocate again on the same file");
    check(size_of(path) == (off_t)want, "still exactly 4 MB - the call is idempotent");

    void* map = ::mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "map it");
    if (map != MAP_FAILED) {
        std::memset(map, 0xAB, 65536);

        // The engine rounds the flush start down to a page boundary. If
        // page_size() is wrong, the address is not page-aligned and msync
        // rejects it - and since the engine does not check the return value,
        // that is durability failing in silence. This is the check that catches
        // it, on a deliberately unaligned offset rounded the engine's way.
        const std::size_t unaligned = 5000;
        const std::size_t start = unaligned & ~(page_size() - 1);
        check(flush_mapping((char*)map + start, 65536 - start, fd) == 0,
              "flush a page-aligned subrange of the mapping");

        check(flush_mapping(map, want, fd) == 0, "flush the whole mapping");
        ::munmap(map, want);
    }

    check(flush_file(fd) == 0, "flush the file by descriptor");

    // A hint. Failing it costs page cache, never correctness, so this only has
    // to not crash or hang.
    release_prefix(fd, 65536);
    check(true, "release a prefix (hint only - cannot fail meaningfully)");

    // The barrier mode is a Darwin-only distinction, but setting it must be
    // harmless everywhere so callers need not guard the call.
    durability_mode() = sync_mode::barrier;
    check(flush_file(fd) == 0, "flush again in barrier mode");
    durability_mode() = sync_mode::full;

    ::close(fd);
    std::remove(path);

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
