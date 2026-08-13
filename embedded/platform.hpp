#pragma once
// platform.hpp - the four system calls this engine needs that are not the same
// everywhere, behind four functions that are.
//
// ============================================================================
// THE DARWIN PATHS IN THIS FILE ARE UNVERIFIED.
//
// They were written against Apple's documented behaviour and reviewed, but not
// compiled and not run: the machine this was developed on has no Darwin
// toolchain. Treat every #if defined(__APPLE__) branch as needing a build and a
// test run on a real Mac before it is trusted. The Linux paths are the ones
// under continuous test.
// ============================================================================
//
// WHAT IS ACTUALLY DIFFERENT, and why each one matters rather than being
// portability boilerplate:
//
//   posix_fallocate       Does not exist on Darwin. Apple never implemented it.
//                         This is not cosmetic: the alternative is a sparse
//                         file, and a sparse mapping raises SIGBUS when the
//                         filesystem runs out of space in the middle of a
//                         write. A signal is not a failure you can handle, and
//                         a phone with a full disk is an ordinary Tuesday.
//
//   fsync / msync         On Darwin these do NOT flush the drive's own write
//                         cache. They hand the data to the device and return.
//                         Power loss after that can still lose it, which is
//                         exactly what "durable" is supposed to exclude, so an
//                         engine whose whole §8 is about durability cannot use
//                         them as-is. See sync_mode below - the choice is real
//                         and has to be made deliberately.
//
//   posix_fadvise         Does not exist on Darwin either. Only used to hint
//                         that a log prefix is finished with, so losing it
//                         costs page cache, not correctness.
//
//   page size             Assumed 4096 by everyone until it is not. Apple
//                         silicon uses 16 KB pages and recent Android devices
//                         do too. msync requires a page-aligned address, so
//                         rounding to the wrong page size hands it an address
//                         it rejects - and since nothing checks msync's return
//                         value, that is durability failing silently. It is
//                         asked of the OS now instead of assumed.
//
// mlock is deliberately absent from this file. It is already treated as
// best-effort by the one caller that uses it (pin_index returns false when it
// fails), which is the correct behaviour on iOS, where it is restricted.

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace storage::platform {

    // The real page size, asked once. sysconf is a vDSO read on Linux and cheap
    // everywhere, but this sits under sync() on the barrier path, so it is
    // cached rather than asked per call.
    inline std::size_t page_size() noexcept {
        static const std::size_t cached = [] {
            const long v = ::sysconf(_SC_PAGESIZE);
            return v > 0 ? (std::size_t)v : (std::size_t)4096;
        }();
        return cached;
    }

    // HOW HARD A DURABILITY BARRIER PUSHES, on Darwin only. Everywhere else
    // fsync already means what it says and this is ignored.
    //
    //   full     fcntl(F_FULLFSYNC) - flushes the drive's own cache. This is
    //            what fsync means on Linux, and the only setting under which
    //            §8's guarantees hold after power loss. It is also slow, and on
    //            some devices dramatically so.
    //
    //   barrier  fcntl(F_BARRIERFSYNC) - orders this write ahead of later ones
    //            without waiting for the medium. Survives a process or kernel
    //            crash, not a power cut. This is what Apple's own SQLite build
    //            uses even when an application asks for full synchronisation,
    //            so it is the platform's de facto normal rather than a corner
    //            worth apologising for.
    //
    // The default is `full`, because the safe choice should be the one you get
    // by not thinking about it, and because a caller who has decided a power
    // cut is acceptable can say so in one line.
    enum class sync_mode { full, barrier };

    inline sync_mode& durability_mode() noexcept {
        static sync_mode mode = sync_mode::full;
        return mode;
    }

    // Reserve `length` bytes so that writing them cannot fail for want of
    // space. Returns 0 on success, an errno value otherwise - posix_fallocate's
    // convention, not errno's.
    inline int preallocate(int fd, std::size_t length) noexcept {
#if defined(__APPLE__)
        // MUST BE IDEMPOTENT, because posix_fallocate is and every caller here
        // reopens an existing store on restart. F_PEOFPOSMODE allocates its
        // length PAST THE CURRENT END of the file, not up to an absolute size -
        // so asking for `length` on a file that already has `length` bytes
        // would double it every time the process started.
        //
        // Hence the fstat: allocate only the shortfall, and do nothing at all
        // when there is none.
        struct ::stat st {};
        if (::fstat(fd, &st) != 0) return errno;
        if ((off_t)length > st.st_size) {
            fstore_t store{};
            store.fst_flags = F_ALLOCATECONTIG;   // a preference, not a requirement
            store.fst_posmode = F_PEOFPOSMODE;
            store.fst_offset = 0;                 // ignored under F_PEOFPOSMODE
            store.fst_length = (off_t)length - st.st_size;
            if (::fcntl(fd, F_PREALLOCATE, &store) < 0) {
                // Retry willing to take fragments before giving up.
                store.fst_flags = F_ALLOCATEALL;
                if (::fcntl(fd, F_PREALLOCATE, &store) < 0) return errno;
            }
        }
        // F_PREALLOCATE reserves blocks without moving the file's end, so this
        // still has to follow or the mapping is short.
        if (::ftruncate(fd, (off_t)length) != 0) return errno;
        return 0;
#else
        return ::posix_fallocate(fd, 0, (off_t)length);
#endif
    }

    // Make a mapped range durable. Returns 0 on success.
    //
    // `fd` is the file behind the mapping, needed only on Darwin: msync there
    // gets the pages to the device and stops, so the barrier has to be asked
    // for separately through the descriptor.
    inline int flush_mapping(void* addr, std::size_t length, int fd) noexcept {
        if (::msync(addr, length, MS_SYNC) != 0) return errno;
#if defined(__APPLE__)
        const int op = durability_mode() == sync_mode::full ? F_FULLFSYNC : F_BARRIERFSYNC;
        if (::fcntl(fd, op) < 0) {
            // F_BARRIERFSYNC is not supported on every filesystem Darwin can
            // mount. Falling back to the stronger call is the safe direction.
            if (op == F_BARRIERFSYNC && ::fcntl(fd, F_FULLFSYNC) >= 0) return 0;
            return errno;
        }
        return 0;
#else
        (void)fd;
        return 0;
#endif
    }

    // Make a written file durable - used for the checkpoint record, which is
    // written with pwrite rather than through a mapping.
    inline int flush_file(int fd) noexcept {
#if defined(__APPLE__)
        const int op = durability_mode() == sync_mode::full ? F_FULLFSYNC : F_BARRIERFSYNC;
        if (::fcntl(fd, op) >= 0) return 0;
        if (op == F_BARRIERFSYNC && ::fcntl(fd, F_FULLFSYNC) >= 0) return 0;
        return errno;
#else
        return ::fsync(fd) == 0 ? 0 : errno;
#endif
    }

    // Say a prefix of the file is finished with, so the cache may drop it. A
    // hint in the strict sense: ignoring it entirely costs memory, never
    // correctness, which is why the Darwin branch is allowed to do nothing.
    inline void release_prefix(int fd, std::size_t length) noexcept {
#if defined(__APPLE__)
        // No posix_fadvise. F_NOCACHE exists but changes the policy for the
        // whole descriptor from here on, including the region still being
        // appended to, which trades a hint for a behaviour change nobody asked
        // for. Doing nothing is the honest option.
        (void)fd;
        (void)length;
#else
        ::posix_fadvise(fd, 0, (off_t)length, POSIX_FADV_DONTNEED);
#endif
    }

} // namespace storage::platform
