// =============================================================================
// field_storage.hpp - GENERATED, single-file build of the storage engine.
//
//     tools/amalgamate.py
//
// Do not edit. Edit the sources and regenerate; `ctest` fails if this file and
// the sources disagree, so a stale copy cannot reach a release.
//
// Contents, in dependency order:
//   embedded/platform.hpp
//   storage/storage.hpp
//   storage/field_storage.hpp
//   storage/prefetch_gather.hpp
//   storage/blob.hpp
//   storage/indexes.hpp
//
// Needs the standard library and POSIX, and nothing else. To use it, copy this
// one file into your project:
//
//     #include "field_storage.hpp"
//
//     using spec = storage::field_spec<"data.log", 8ull << 30,
//                                      "data.idx", 1'000'000, 200'000>;
//     storage::field_storage<spec> table;
//
// Paths can also be given at run time, which is what a sandboxed process needs:
//
//     storage::field_storage<spec> table(storage::storage_paths::under<spec>(dir));
//
// The specification is FIELD_STORAGE.md in the source repository.
// =============================================================================

#pragma once

// ---- begin embedded/platform.hpp ---------------------------------------
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
// ---- end embedded/platform.hpp

// ---- begin storage/storage.hpp -----------------------------------------
// storage.hpp - a single append-only storage over mmap that stores any number of
// types without upfront registration. One id space, one id->byte* index,
// one write offset. T is only named at the call site (create<T>/get<T>/
// for_each<T>) - the storage itself carries no type information and performs
// no dispatch.
//
// Strictly single-threaded, with one explicit exception: background_compact
// (Variant 2) permits exactly two threads on a storage - writer and compactor -
// both synchronized through the same mutex.
//
// Compaction, two variants:
//   compact(scratch_base) - blocking (Variant 1). A storage always owns
//                              exactly one file, so there is no need to
//                              rebind a base across multiple tables.
//   background_compact(...) - non-blocking (Variant 2). The snapshot is an
//                              arena (one mapped buffer plus one mapped
//                              slot per id) - no per-record allocation, no
//                              dependency on a fixed T, and nothing that
//                              grows on the heap with the dataset.
//
// Update model: a WAL diff (offset + len + payload) plus an in-place copy
// for fast get() without replay. Recovery always rebuilds state from the log
// - the in-place bytes are never trusted for that - so the WAL stays strictly
// append-only (only the tail can tear), and "stop at the first checksum
// mismatch" remains sound with no caveats. Changing a value's shape or size
// (a different T on the same id) goes through recreate(), a distinct
// operation from update().
//
// EVERY record's checksum covers its header AND its payload, with no
// exceptions. Note for anyone reading an old log: CREATE records used to be
// checksummed over the header alone, so this is an on-disk FORMAT BREAK -
// logs written before it do not recover, and a replica must run the same
// build as its primary.
//
// WHERE THOSE TWO THINGS LIVE, and why it matters:
//
// A filesystem commit is per-INODE, not per-range. msync(log_tail, MS_SYNC)
// honours its range for writeback, but the commit it triggers underneath
// (msync -> vfs_fsync_range -> the fs's ->fsync) drags every dirty page of
// that inode into the transaction. So if the in-place copy lives in the same
// file as the log, every durability point pays for update()'s scattered
// random patches. Measured on btrfs, 200k random durable updates: 1010 MiB
// written and 3.0 s, against 40 MiB and 0.2 s once the two are separated
// (bench/probe_split_log_and_data.cpp).
//
// Hence, for durability::local_sync ONLY, a storage owns TWO files:
//
//   spec::path       the log     append-only WAL, the source of truth,
//                                the only thing sync() ever flushes
//   spec::data_path  the data    materialized record payloads, what
//                                index[id] points at and what update()
//                                patches in place
//
// The data file is a CACHE, not a second source of truth: it is never
// synced, its post-crash contents are never read, and recover() rebuilds it
// from the log every time. It is a file rather than anonymous memory purely
// so its pages stay evictable - anonymous pages can only go to swap, which
// caps the dataset at RAM; file-backed pages get written back, go clean, and
// are reclaimed and faulted in again on demand.
//
// durability::shared and durability::replicated are unchanged: one
// caller-provided region, the index aliases the log payload directly,
// no copy. Only local_sync splits.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <concepts>
#include <type_traits>
#include <span>
#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <nmmintrin.h>   // _mm_crc32_u8/_mm_crc32_u64 - hardware CRC32C (SSE4.2)
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>    // __crc32cb/__crc32cd - the same function, ARM's spelling
#endif
#include <string>
#include <unordered_map>
#include <mutex>

namespace storage {

    // =====================================================================
    // tag - a free-form field in the record header. The engine never
    // dereferences or dispatches on it - it just stores and returns it (see
    // for_each_raw). Values are assigned by hand, typically from one
    // central enum per project:
    //
    //   enum class kind : storage::tag_t { user = 1, order = 2 };
    //
    // and each type declares its own tag (the engine keeps no type registry):
    //
    //   struct user {
    //       static constexpr storage::tag_t tag = (storage::tag_t)kind::user;
    //       uint64_t balance;
    //   };
    //
    // Discipline the language does not enforce: never reuse or renumber
    // values across releases (as with protobuf/thrift field numbers) -
    // otherwise old on-disk records get silently reinterpreted as a
    // different type.
    // =====================================================================
    using tag_t = uint16_t;

    template <typename T>
    concept aggregate =
        std::is_trivially_copyable_v<T> &&
        std::is_standard_layout_v<T> &&
        std::has_unique_object_representations_v<T> &&
        (!std::is_pointer_v<T>) &&
        (!std::is_polymorphic_v<T>);

    template <typename T>
    concept tagged_aggregate = aggregate<T> && requires {
        { T::tag } -> std::convertible_to<tag_t>;
    };

    template <typename T>
    concept field = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

    using record_id = uint64_t;

    // =====================================================================
    // A record id that carries what it points AT.
    //
    // record_id is a uint64_t, so nothing distinguishes a reference to an order
    // from one to a product. bench/app_layer.hpp has both in one struct, told
    // apart by a comment:
    //
    //     record_id order_id;      // FK -> order
    //     record_id product_id;    // FK -> product
    //
    // Swap those at a call site and it compiles. Hand one to get<product>() and
    // it compiles. ref<T> makes both an error, and makes get() able to work out
    // the type on its own.
    //
    // ON DISK IT IS THE SAME EIGHT BYTES. sizeof and every field offset are
    // unchanged, so a log written before this reads back after it - the whole
    // type is a compile-time claim about a number that was already there.
    //
    // WHAT IT DOES NOT FIX, and the distinction matters: two instances of one
    // spec both issue ids from 1, so a ref<customer> from one is
    // indistinguishable from a ref<customer> from another (see §1 on sharding).
    // Type safety within an instance is what this buys; across instances the id
    // space has to carry the instance, which is a different change.
    // =====================================================================
    template <typename T>
    class ref {
    public:
        using target = T;

        constexpr ref() noexcept = default;
        constexpr explicit ref(record_id value) noexcept : id_(value) {}

        constexpr record_id id() const noexcept { return id_; }
        // 0 is never a live record, so this is "does it point at anything".
        constexpr explicit operator bool() const noexcept { return id_ != 0; }

        friend constexpr bool operator==(ref, ref) noexcept = default;
        friend constexpr auto operator<=>(ref, ref) noexcept = default;

    private:
        record_id id_ = 0;
    };

    template <typename T> struct is_ref : std::false_type {};
    template <typename T> struct is_ref<ref<T>> : std::true_type {};
    template <typename T> inline constexpr bool is_ref_v = is_ref<T>::value;

    inline std::size_t pad8(std::size_t n) noexcept { return (n + 7) & ~std::size_t(7); }

    // CRC32C (Castagnoli, reflected, polynomial 0x82F63B78), hardware where the
    // target has an instruction for it and software where it does not.
    //
    // ALL THREE PATHS MUST AGREE BYTE FOR BYTE, and that is not a nicety: a
    // checksum written by one machine is verified by another. Replication ships
    // raw records between hosts, and a log written on x86 can be opened on ARM.
    // A divergence here would not corrupt anything - it would make every record
    // look corrupt, on the other architecture only, which is a far worse bug to
    // be handed. tests/test_storage.cpp checks the paths against each other and
    // against the standard vector.
    //
    // memcpy for the 8-byte reads, not a direct (const uint64_t*) cast: every
    // existing call site happens to pass a pointer derived from base + a
    // pad8()-aligned offset, so the cast was never actually observed to
    // fault - but it's still UB for an unaligned pointer per the C++ object
    // model (confirmed by UBSan: apply_wal_range(), added for replication,
    // is the first caller that can legitimately pass an arbitrary,
    // non-8-byte-aligned buffer - a network-received payload has no
    // alignment guarantee at all). memcpy compiles down to the identical
    // single unaligned-load instruction here, so this costs nothing.
    //
    // The byte-at-a-time software loop is deliberately not table-driven. It is
    // the fallback for architectures with no CRC instruction at all, where the
    // engine is not going to be fast regardless, and a 1 KB lookup table in a
    // header that a phone includes is a worse trade than the shifts.

    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                  "the record format stores integers by memcpy, so a big-endian host would "
                  "read another host's log as garbage - and crc32c's 8-byte step assumes it too");

    inline uint32_t crc32c_software(const void* data, std::size_t len, uint32_t seed) noexcept {
        const uint8_t* p = (const uint8_t*)data;
        uint32_t crc = seed;
        while (len--) {
            crc ^= *p++;
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)-(int32_t)(crc & 1));
        }
        return crc;
    }

    // __x86_64__ and not also __i386__: _mm_crc32_u64 does not exist on 32-bit
    // x86, only _mm_crc32_u32 does. The wider guard compiled here and failed on
    // an Android i686 build, which is an ABI gradle still emits by default.
    // 32-bit x86 falls through to the software path, which is correct and only
    // slower, and is a dead ABI besides.
#if defined(__x86_64__)
    __attribute__((target("sse4.2")))
    inline uint32_t crc32c(const void* data, std::size_t len, uint32_t seed = 0xFFFFFFFFu) noexcept {
        const uint8_t* p = (const uint8_t*)data;
        uint32_t crc = seed;
        while (len >= 8) {
            uint64_t chunk;
            std::memcpy(&chunk, p, sizeof(chunk));
            crc = (uint32_t)_mm_crc32_u64(crc, chunk);
            p += 8; len -= 8;
        }
        while (len) { crc = _mm_crc32_u8(crc, *p); ++p; --len; }
        return crc;
    }
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    // Mandatory from ARMv8.1 and present on every Apple silicon and essentially
    // every 64-bit Android device, but optional in ARMv8.0 - hence the feature
    // macro rather than the architecture alone. Without it this falls through to
    // the software path below, which is correct and merely slower.
    inline uint32_t crc32c(const void* data, std::size_t len, uint32_t seed = 0xFFFFFFFFu) noexcept {
        const uint8_t* p = (const uint8_t*)data;
        uint32_t crc = seed;
        while (len >= 8) {
            uint64_t chunk;
            std::memcpy(&chunk, p, sizeof(chunk));
            crc = __crc32cd(crc, chunk);
            p += 8; len -= 8;
        }
        while (len) { crc = __crc32cb(crc, *p); ++p; --len; }
        return crc;
    }
#else
    inline uint32_t crc32c(const void* data, std::size_t len, uint32_t seed = 0xFFFFFFFFu) noexcept {
        return crc32c_software(data, len, seed);
    }
#endif

    // pointer-to-member -> its class, needed by update<member>(id, v):
    // storage<spec> is not templated on any T, so the field's owning class
    // is deduced from the member pointer itself, not from a class template
    // parameter.
    template <typename M>
    struct member_traits;
    template <typename C, typename V>
    struct member_traits<V C::*> { using class_type = C; using value_type = V; };

    template <auto member>
    using member_class_t = typename member_traits<decltype(member)>::class_type;
    template <auto member>
    using member_value_t = typename member_traits<decltype(member)>::value_type;

    // destroy, not delete_: "delete" is a reserved keyword, but rather than
    // dodge it with a trailing-underscore hack, this names the enumerator
    // after the public API method that produces it (storage::destroy()) -
    // clearer at every call site than an arbitrary keyword-avoidance suffix
    // would have been.
    enum class record_op : uint8_t { create = 0, update = 1, destroy = 2 };

    // Where a record came from, and therefore how much of it still has to be
    // questioned. `local` - this process just built it from a typed call, so
    // its fields are as trustworthy as the call site. `external` - the bytes
    // arrived from a file or a network peer, so nothing about them is known
    // beyond their checksum matching, which only proves they are the bytes
    // that were sent, not that the numbers in them are sane.
    //
    // external is the DEFAULT, so a new call site gets the checks by omission
    // rather than by remembering to ask for them.
    enum class provenance : uint8_t { local, external };

    // =====================================================================
    // durability is now a property of the WHOLE storage, not of a single type
    // - a storage can hold any number of types, all sharing one durability
    // domain. Need different levels for different data? Use a second storage
    // - that's a split by durability, not by data type.
    // =====================================================================
    enum class durability : uint8_t { shared, local_sync, replicated };

    template <std::size_t N>
    struct fixed_string {
        char value[N]{};
        consteval fixed_string(const char (&s)[N]) noexcept { std::copy_n(s, N, value); }
    };
    template <std::size_t N> fixed_string(const char (&)[N]) -> fixed_string<N>;

    // "<path>.data" at compile time. The data file's name is DERIVED rather
    // than required, so that every spec written before the log/data split
    // keeps compiling and keeps meaning the same thing - naming a second
    // path is opt-in (data_path_v below), not a new obligation.
    template <fixed_string log_path>
    consteval auto with_data_suffix() {
        constexpr char suffix[] = ".data";
        constexpr std::size_t n = sizeof(log_path.value);   // includes the NUL
        constexpr std::size_t m = sizeof(suffix) - 1;
        std::array<char, n + m> out{};
        for (std::size_t i = 0; i + 1 < n; ++i) out[i] = log_path.value[i];
        for (std::size_t i = 0; i < m; ++i) out[n - 1 + i] = suffix[i];
        out[n + m - 1] = '\0';
        return out;
    }

    // level_v/path_v/capacity_v (not level/path/capacity): each is exposed
    // again as a same-named static member below (storage_spec::level, etc)
    // - the "_v" suffix marks the template parameter as the more-local side
    // of that pairing, leaving the public-facing static member with the
    // plain, natural name.
    //
    // capacity is per FILE, not the total: local_sync reserves it once for
    // the log and once for the data file. data_offset can never exceed
    // write_offset (every live materialization has a CREATE behind it in the
    // log), so one bound genuinely covers both - at the price of reserving
    // 2 * capacity of disk.
    template <durability level_v = durability::shared, fixed_string path_v = fixed_string(""), std::size_t capacity_v = 16ull * 1024 * 1024,
              fixed_string data_path_v = fixed_string(""), fixed_string index_path_v = fixed_string(""),
              fixed_string checkpoint_path_v = fixed_string(""), std::size_t index_entries_v = 0>
    struct storage_spec {
        static constexpr durability level = level_v;
        static constexpr auto path = path_v;
        static constexpr std::size_t capacity = capacity_v;

        // Empty (the default) leaves the index on the heap, exactly as
        // before. Naming a path moves it into a file instead - see
        // index_array below for why that is worth having.
        static constexpr const char* index_path = index_path_v.value;
        static constexpr bool index_on_disk = index_path_v.value[0] != '\0';

        // One slot per id ever issued. A record occupies at least
        // pad8(sizeof(record_header) + 1) == 32 bytes of log, so a
        // capacity-byte log can never mint more than capacity/32 ids - that
        // is a real bound, not a guess, and it costs capacity/4 bytes of
        // index file. recreate() with a hand-picked id beyond it is refused
        // rather than served, which is the point: an arbitrary id can no
        // longer turn into an arbitrary allocation.
        // capacity/32 is the true upper bound - a record occupies at least
        // pad8(sizeof(record_header) + 1) == 32 bytes of log - but it is the
        // bound for the SMALLEST possible record. A storage holding kilobyte
        // records will never mint anywhere near that many ids, and reserving
        // capacity/4 bytes of index file for them is pure waste: at a 28 GB
        // capacity that is 7 GB of index for perhaps 200 MB of real slots.
        // index_entries_v overrides it with what the caller actually expects.
        static constexpr std::size_t index_entries = index_entries_v ? index_entries_v : capacity_v / 32;

        // Held as a member so the array outlives every use of data_path;
        // an explicit data_path_v wins over the derived name.
        static constexpr auto derived_data_path = with_data_suffix<path_v>();
        static constexpr const char* data_path = data_path_v.value[0] ? data_path_v.value : derived_data_path.data();

        // Empty (the default) means recover() always replays the log, as it
        // always has. Naming a path turns on the fast-restart path - see
        // storage::checkpoint().
        //
        // It requires index_path, because a heap index cannot survive the
        // process and there would be nothing to restart from.
        static constexpr const char* checkpoint_path = checkpoint_path_v.value;
        static constexpr bool fast_restart = checkpoint_path_v.value[0] != '\0';
        static_assert(!fast_restart || index_path_v.value[0] != '\0',
                      "a checkpoint needs the index on disk too - give the spec an index_path");
        static_assert(!fast_restart || level_v == durability::local_sync,
                      "a checkpoint only means anything for durability::local_sync, which owns its files");
    };

    // What a checkpoint records: the exact position at which the log, the
    // materialized records and the index were all made durable together.
    // Written last and atomically, so it can only exist once everything it
    // describes is already on disk.
    struct checkpoint_record {
        uint64_t magic;             // 8, offset 0
        uint32_t version;           // 4, offset 8
        uint32_t reserved;          // 4, offset 12
        uint64_t write_offset;      // 8, offset 16 - the log's end
        uint64_t data_offset;       // 8, offset 24
        uint64_t live_byte_count;   // 8, offset 32
        uint64_t next_id;           // 8, offset 40
        uint64_t index_count;       // 8, offset 48
        uint32_t tail_crc;          // 4, offset 56 - of the log bytes just before write_offset
        uint32_t checksum;          // 4, offset 60 - over everything above
    };
    static_assert(sizeof(checkpoint_record) == 64);
    inline constexpr uint64_t checkpoint_magic = 0x4b50435f45524f54ull;
    inline constexpr uint32_t checkpoint_version = 1;
    inline constexpr std::size_t checkpoint_tail_bytes = 4096;

    // =====================================================================
    // record_header sits in the file right before its payload. tag is
    // opaque to the engine - only user code interprets it, at the read
    // boundary. 24 bytes, unchanged from before: the 2-byte tag simply
    // occupies what used to be dead padding (1-byte type_id + 2 bytes of
    // padding became 1 byte of padding + a 2-byte tag - same total).
    // =====================================================================
    struct alignas(8) record_header {
        record_id id;            // 8, offset 0 - hdr.id==0 marks a sparse file tail; depends on offset 0
        uint32_t  offset;        // 4, offset 8
        uint32_t  len;           // 4, offset 12
        record_op op;             // 1, offset 16
        // Eight bits that belong to whatever is layered on top. The engine
        // writes them as zero and never reads them - see
        // set_last_record_flags() for why they are worth having: a layer that
        // needs to mark a boundary (the end of a transaction, say) can do it
        // in a header that was going to be written anyway, instead of
        // appending a record of its own.
        uint8_t   flags = 0;      // 1, offset 17
        tag_t     tag;             // 2, offset 18
        uint32_t  checksum = 0;     // 4, offset 20
    };
    static_assert(sizeof(record_header) == 24, "header layout drifted");
    static_assert(alignof(record_header) == 8);
    static_assert(offsetof(record_header, id) == 0, "sparse-tail check (hdr.id==0) requires offset 0");
    static_assert(offsetof(record_header, tag) % alignof(tag_t) == 0, "tag misaligned");
    static_assert(offsetof(record_header, checksum) == 20, "write_record/recover depend on this offset");
    static_assert(offsetof(record_header, checksum) % alignof(uint32_t) == 0);

    inline uint32_t checksum_of(const record_header& h, const void* data, uint32_t data_len) noexcept {
        uint32_t crc = crc32c(&h, offsetof(record_header, checksum));
        crc = crc32c(data, data_len, crc);
        return crc ^ 0xFFFFFFFFu;
    }

    // =====================================================================
    // An observer is told about every record the storage applies - from a
    // live create/update/destroy, from recover(), and from replication's
    // apply_wal_range(), all through the SAME call. That sameness is the
    // point: a secondary index maintained here is maintained identically
    // when the log is replayed at startup or shipped to a replica, so it
    // cannot drift from the data it indexes.
    //
    // It runs BEFORE the record is applied, so the storage still holds the
    // PREVIOUS state: get<T>(hdr.id) yields the value about to be replaced
    // or removed, which is exactly what an index needs in order to retract
    // its old entry. The new value is in `payload`, already checksum-verified.
    // The record is guaranteed to be applied after the call returns - every
    // reason to refuse one is checked first.
    //
    // The default sees everything and does nothing; [[no_unique_address]]
    // means it costs no space and the call inlines to nothing.
    // =====================================================================
    struct null_observer {
        void on_record(auto&, const record_header&, const std::byte*) noexcept {}
    };

    // =====================================================================
    // storage<spec> - the single storage. No tuple<storage<T>...>, no
    // compile-time type list, no type_id dispatch table. For local_sync it
    // opens and maps its own file (as region<spec> used to); for
    // shared/replicated, base is supplied by the caller - the storage neither
    // opens nor owns it.
    // =====================================================================
    template <typename spec = storage_spec<>, typename observer = null_observer>
    class storage {
    public:
        // An OFFSET from data_base, not a pointer: a pointer cannot be
        // stored in a file (the next mmap lands somewhere else), and the
        // index is a candidate for exactly that - see index_array. 0 means
        // "no record": a live payload always sits at least one
        // record_header past the start of its region, so offset 0 is not a
        // value any live slot can hold.
        struct index_entry { std::size_t offset = 0; };

        // One slot per id EVER created - dead slots are never reclaimed
        // (see reserve). That makes the index the one structure whose size
        // tracks lifetime churn rather than live data, and as a std::vector
        // it is anonymous memory, which the kernel can only swap, never
        // evict. spec::index_path backs it with a file instead, so its pages
        // get written back, go clean, and become reclaimable like every
        // other region here.
        //
        // It is NOT a checkpoint. recover() rebuilds the index from the log
        // every time and never reads what a previous run left behind; the
        // file is opened truncated for exactly that reason.
        class index_array {
        public:
            index_array() = default;
            ~index_array() { close_file(); }
            index_array(const index_array&) = delete;
            index_array& operator=(const index_array&) = delete;

            // keep_contents is what a checkpoint needs: the index on disk IS
            // the state being restarted from, so truncating it would throw
            // away the very thing being preserved. Without a checkpoint the
            // previous run's contents are meaningless, and dropping them is
            // what guarantees every slot starts dead.
            void map_file(const char* path, std::size_t entries, bool keep_contents = false) {
                close_file();
                int opened = ::open(path, O_RDWR | O_CREAT, 0644);
                if (opened < 0) return; // stays on the heap - degraded, not broken
                const std::size_t bytes = entries * sizeof(index_entry);
                // Reserved blocks read back as zeros, so every slot starts
                // dead without touching (and therefore faulting in) a page.
                if (!keep_contents && ::ftruncate(opened, 0) != 0) { ::close(opened); return; }
                if (::posix_fallocate(opened, 0, bytes) != 0) { ::close(opened); return; }
                void* m = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, opened, 0);
                if (m == MAP_FAILED) { ::close(opened); return; }
                slots = (index_entry*)m;
                fd = opened;
                file_backed = true;
                capacity = entries;
                mapped_bytes = bytes;
                count = 0;
            }

            std::size_t size() const noexcept { return count; }
            index_entry& operator[](std::size_t i) noexcept { return slots[i]; }
            const index_entry& operator[](std::size_t i) const noexcept { return slots[i]; }

            // Makes slot i addressable, or nullptr if it cannot be. The heap
            // form grows geometrically as it always did; the file form is
            // capped by its mapping and says no instead of growing.
            index_entry* reserve_slot(std::size_t i) {
                if (file_backed) {
                    if (i >= capacity) return nullptr;
                } else if (i >= heap.size()) {
                    heap.resize(std::max<std::size_t>(i + 1, heap.size() * 2 + 1));
                    slots = heap.data();
                    capacity = heap.size();
                }
                if (i + 1 > count) count = i + 1;
                return slots + i;
            }

            void clear() noexcept {
                if (file_backed) { if (count) std::memset(slots, 0, count * sizeof(index_entry)); }
                else { heap.clear(); slots = nullptr; capacity = 0; }
                count = 0;
            }

            // Only the slots in use - the mapping covers the whole capacity,
            // and flushing pages nothing was ever written to would be pure
            // waste at any realistic index size.
            void sync() noexcept {
                if (!file_backed || !count) return;
                ::msync(slots, (count * sizeof(index_entry) + 4095) & ~std::size_t(4095), MS_SYNC);
            }

            bool on_disk() const noexcept { return file_backed; }

            // Adopts a count from a checkpoint: the slots below it are already
            // in the mapping, so nothing has to be replayed to reach them.
            bool restore_count(std::size_t n) noexcept {
                if (!file_backed || n > capacity) return false;
                count = n;
                return true;
            }

        private:
            void close_file() noexcept {
                if (!file_backed) return;
                ::munmap(slots, mapped_bytes);
                ::close(fd);
                slots = nullptr; fd = -1; file_backed = false; capacity = 0; mapped_bytes = 0; count = 0;
            }
            index_entry* slots = nullptr;
            std::size_t count = 0, capacity = 0, mapped_bytes = 0;
            std::vector<index_entry> heap;
            int fd = -1;
            bool file_backed = false;
        };

        // A type-erased view of a record - for generic iteration without
        // knowing T (rebuilding user-space secondary indexes after
        // recovery, dumping/debugging). tag hints at how to interpret it;
        // the tag->handler table lives entirely in user code, the engine
        // holds no such table.
        struct raw_view { record_id id; tag_t tag; std::span<const std::byte> bytes; };

        // True when the log and the materialized records live in separate
        // files. Named once here because it gates every place the two models
        // differ, and reads better at those sites than repeating the
        // durability comparison.
        static constexpr bool split_regions = (spec::level == durability::local_sync);

        explicit storage(std::byte* shared_base = nullptr) {
            if constexpr (split_regions) {
                fd = ::open(spec::path.value, O_RDWR | O_CREAT, 0644);
                ::posix_fallocate(fd, 0, spec::capacity);
                base = (std::byte*)(::mmap(nullptr, spec::capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));

                // Same flags as the log, deliberately: this mapping is
                // written to constantly (every update() patches it) and is
                // never synced. MAP_SHARED is what keeps those pages
                // reclaimable - writeback cleans them and the kernel can
                // then evict them, which is the entire reason this is a file
                // and not MAP_ANONYMOUS.
                data_fd = ::open(spec::data_path, O_RDWR | O_CREAT, 0644);
                ::posix_fallocate(data_fd, 0, spec::capacity);
                data_base = (std::byte*)(::mmap(nullptr, spec::capacity, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0));

                // NOT madvise(MADV_RANDOM), which looks obviously right and is
                // not. The access pattern IS random - index[id] -> data_base +
                // offset says nothing about locality - so disabling the
                // kernel's readahead should stop it wasting bandwidth on pages
                // nobody asked for. Measured on 26 GB with 24 GB of RAM
                // (bench/big_dataset.cpp), it made everything worse:
                //
                //   random read     3262 -> 2516 ops/s
                //   random update  10148 -> 6180 ops/s
                //   major faults    72k  -> 144k on reads, 14k -> 146k on updates
                //   fill          228k  -> 150k rec/s
                //
                // Two reasons, both obvious in hindsight. The region is
                // written SEQUENTIALLY during fill and recovery, where
                // readahead is exactly right, and one advice cannot serve both
                // phases. And a 128 KB readahead that turns out to be useful
                // even a fraction of the time beats 32 separate 4 KB faults,
                // because the device is far better at one large read than at
                // many small ones.
            } else {
                base = shared_base;
                data_base = shared_base;
            }
            if constexpr (spec::index_on_disk) index.map_file(spec::index_path, spec::index_entries, spec::fast_restart);
        }

        ~storage() {
            if constexpr (split_regions) {
                if (base) ::munmap(base, spec::capacity);
                if (data_base) ::munmap(data_base, spec::capacity);
                if (fd >= 0) ::close(fd);
                if (data_fd >= 0) ::close(data_fd);
            }
        }

        storage(const storage&) = delete;
        storage& operator=(const storage&) = delete;

        template <tagged_aggregate T>
        record_id create(const T& value) {
            record_id id = next_id++;
            // Reserved up front so a refusal costs nothing: apply() would
            // decline the record anyway, but only after it had been appended
            // to the log.
            if (!reserve(id)) return 0; // index full - 0 is never a valid record id
            std::byte* logged = write_record<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        // Replaces a value's shape or size (variable length, a different T
        // on the same id) with a fresh CREATE over the existing id. The old
        // bytes become garbage, reclaimed on the next compaction.
        template <tagged_aggregate T>
        record_id recreate(record_id id, const T& value) {
            if (!reserve(id)) return 0;
            std::byte* logged = write_record<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        template <auto member>
        requires field<member_value_t<member>>
        void update(record_id id, member_value_t<member> value) {
            if (!alive(id)) return;
            const std::size_t off = member_offset<member>();
            std::byte* logged = write_record<record_op::update>(id, (uint32_t)(off), tag_t{0}, std::as_bytes(std::span{&value, 1}));
            apply_local<record_op::update, sizeof(member_value_t<member>)>(*header_of(logged), logged);
        }

        void destroy(record_id id) {
            if (!alive(id)) return;
            std::byte* logged = write_record<record_op::destroy>(id, 0, tag_t{0}, {});
            apply_local<record_op::destroy>(*header_of(logged), logged);
        }

        template <tagged_aggregate T>
        [[nodiscard]] const T* get(record_id id) const noexcept {
            return alive(id) ? (const T*)(record_ptr(id)) : nullptr;
        }

        // Claims the next id without writing anything. For a caller that must
        // NAME a record before it is ready to append it - a transaction that
        // stages its writes and only flushes them at commit needs ids up front
        // so that records staged later can refer to records staged earlier.
        // Ids are monotonic and never reused, so one claimed by a transaction
        // that then aborts is simply skipped, which costs nothing but a gap.
        [[nodiscard]] record_id reserve_id() noexcept { return next_id++; }

        // Type-erased counterpart of recreate<T>: a CREATE for `id` carrying
        // `tag` and `bytes`, for a caller holding a record's bytes without
        // naming its C++ type - a staged transaction flushing its batch, a
        // migration, any tool driven by tag_of() rather than by a type.
        record_id create_raw(record_id id, tag_t tag, std::span<const std::byte> bytes) {
            if (!reserve(id)) return 0;
            std::byte* logged = write_record<record_op::create>(id, 0, tag, bytes);
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        static constexpr std::size_t no_record = (std::size_t)-1;

        // Sets bits in record_header::flags on the most recently appended
        // record, refreshing its checksum in place. Returns false only if
        // nothing has been appended yet.
        //
        // This is what makes a transaction boundary free. A layer above would
        // otherwise have to append a marker record to say "everything up to
        // here is committed" - 40 bytes and a whole extra pass through
        // apply() and the observer, against a bit in a header that was being
        // written regardless. Recovery then looks for the flag instead of the
        // marker (see bench/app_txn.hpp).
        //
        // It writes in place, but never to an old page: the record was
        // appended since the last sync(), so its page is already dirty and
        // already inside the range the next msync will cover. Nothing is
        // added to what a durability point has to flush.
        bool set_last_record_flags(uint8_t bits) noexcept {
            if (last_record_offset == no_record) return false;
            record_header* h = (record_header*)(base + last_record_offset);
            h->flags |= bits;
            h->checksum = checksum_of(*h, base + last_record_offset + sizeof(record_header), h->len);
            return true;
        }

        // The tag a record was created with, or 0 if the id is not live.
        // UPDATE and DESTROY records carry no tag of their own - they name an
        // id and a byte range, nothing more - so this is how an observer
        // learns what KIND of record a diff is patching, and therefore which
        // of its indexes the diff belongs to.
        [[nodiscard]] tag_t tag_of(record_id id) const noexcept {
            return alive(id) ? header_of(record_ptr(id))->tag : tag_t{0};
        }

        // Run-time-offset counterpart of update<member>: patches `bytes` at
        // `offset` inside a record whose type the call site does not name.
        // What update<member> cannot express - an undo log restoring bytes it
        // saved earlier, a migration, any generic tool that works off tag_of()
        // rather than a C++ type.
        //
        // The offset is a run-time value here, so unlike update<member> this
        // takes the checked path: a patch that would reach past the end of the
        // record is refused, not applied.
        void patch(record_id id, uint32_t offset, std::span<const std::byte> bytes) {
            if (!alive(id)) return;
            if ((std::size_t)offset + bytes.size() > header_of(record_ptr(id))->len) return;
            std::byte* logged = write_record<record_op::update>(id, offset, tag_t{0}, bytes);
            // local: the range was already checked above, so the external
            // path would only check it a second time.
            apply_local<record_op::update>(*header_of(logged), logged);
        }

        void for_each_raw(auto&& fn) const requires std::invocable<decltype(fn), raw_view> {
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].offset) continue;
                std::byte* rec = record_ptr(id);
                const record_header* h = header_of(rec);
                fn(raw_view{ id, h->tag, std::span<const std::byte>{rec, h->len} });
            }
        }

        template <tagged_aggregate T>
        void for_each(auto&& fn) const requires std::invocable<decltype(fn), record_id, const T&> {
            for (record_id id = 1; id < index.size(); ++id) {
                if (index[id].offset) fn(id, *(const T*)(record_ptr(id)));
            }
        }

        // THE single point where a record becomes state. Live writes reach it
        // by appending to the log and handing back the record they just wrote;
        // recover() and apply_wal_range() reach it by verifying bytes off the
        // disk or the wire. Never interprets the payload (tag is ignored) - it
        // only moves bytes by offset/len, so one method covers every type.
        // hdr BY VALUE, deliberately. A reference would point into the log
        // mapping, and every store below goes through a std::byte*, which is
        // allowed to alias anything - so the compiler would have to reload
        // hdr.id/op/offset/len from memory after each one. Measured on random
        // updates: 9.2M ops/s with a reference, 32.2M with a copy of the same
        // 24 bytes.
        //
        // TWO ENTRY POINTS, ONE IMPLEMENTATION PER OP. Bytes off a disk or a
        // wire carry their op in the header, so they dispatch at run time. A
        // record this process just wrote does not: update<member> always writes
        // an update whose payload is exactly sizeof the member. Saying so in
        // the type lets the switch and the length dispatch vanish, and the
        // three op bodies are still the only implementations there are - what
        // is specialised is which one is reached, not what it does.
        //
        // Not micro-tuning for its own sake. With the op left at run time,
        // clang emitted 70% more instructions than gcc on the identical update
        // loop (160.8M against 94.4M, IPC 0.93 against 0.84 - more work, not
        // more stalls) and ran 1.8x slower, because it declined to inline a
        // function carrying all three paths.
        template <provenance from = provenance::external>
        void apply(record_header hdr, const std::byte* data) {
            switch (hdr.op) {
                case record_op::create:  apply_create(hdr, data); break;
                case record_op::update:  apply_update<from, 0>(hdr, data); break;
                case record_op::destroy: apply_destroy(hdr); break;
            }
        }

        // payload_len is the payload's size when the caller knows it, and 0
        // when it does not - patch(), whose length is a run-time argument.
        template <record_op op, std::size_t payload_len = 0>
        void apply_local(record_header hdr, const std::byte* data) {
            if constexpr (op == record_op::create) apply_create(hdr, data);
            else if constexpr (op == record_op::update) apply_update<provenance::local, payload_len>(hdr, data);
            else apply_destroy(hdr);
        }

        // Walks the verified records of [from, to) and hands each to fn
        // WITHOUT applying anything - the sibling of for_each_raw for a
        // delta rather than a whole state. This is what a layer above uses
        // to catch its own persisted index up to the storage ("my index
        // reflects the log through offset Y, the storage is at X, give me
        // what is in between") instead of rebuilding it from scratch on
        // every start.
        //
        // fn may return void, or bool to stop early. Returns the offset it
        // stopped at, which for a clean walk is `to` rounded down to the
        // last whole record.
        std::size_t for_each_record(std::size_t from, std::size_t to, auto&& fn) const {
            return scan(base, from, to, [&](const record_header& hdr, const std::byte* payload) {
                if constexpr (std::is_void_v<decltype(fn(hdr, std::span<const std::byte>{payload, hdr.len}))>) {
                    fn(hdr, std::span<const std::byte>{payload, hdr.len});
                    return true;
                } else {
                    return fn(hdr, std::span<const std::byte>{payload, hdr.len});
                }
            });
        }
        std::size_t for_each_record(std::size_t from, auto&& fn) const { return for_each_record(from, write_offset, fn); }

        // Strictly sequential scan, stops at the first checksum mismatch -
        // nothing past that point is read or applied. For local_sync,
        // file_size is usually obtained via ::stat() on the compile-time
        // known spec::path.
        // =================================================================
        // Fast restart.
        //
        // recover() replays the log because the log is the only thing on disk
        // that is trusted. checkpoint() makes the other two trusted as well:
        // it flushes the log, the materialized records and the index, and only
        // THEN writes a marker naming the offset at which all three agree.
        //
        // The ordering is the whole correctness argument. Everything the
        // marker describes is durable before the marker exists, and the marker
        // is written to a temp file, fsynced, and renamed - so a crash at any
        // point leaves either no marker at all (recover() replays, exactly as
        // before) or a marker whose contents are already on disk.
        //
        // Enabled by spec::checkpoint_path. Without it this is a no-op and
        // nothing changes.
        // =================================================================
        bool checkpoint() noexcept requires (spec::fast_restart) {
            if (!index.on_disk()) return false;

            // 1. The log. 2. The materialized records - the one place they are
            // ever flushed; every other day of their life they are a cache
            // that recover() would rebuild. 3. The index.
            sync();
            if (data_offset) ::msync(data_base, (data_offset + page_size - 1) & ~(page_size - 1), MS_SYNC);
            index.sync();

            checkpoint_record rec{};
            rec.magic = checkpoint_magic;
            rec.version = checkpoint_version;
            rec.write_offset = write_offset;
            rec.data_offset = data_offset;
            rec.live_byte_count = live_byte_count;
            rec.next_id = next_id;
            rec.index_count = index.size();
            rec.tail_crc = log_tail_crc(write_offset);
            rec.checksum = crc32c(&rec, offsetof(checkpoint_record, checksum)) ^ 0xFFFFFFFFu;

            // 4. And only now the marker, atomically.
            std::string tmp = std::string(spec::checkpoint_path) + ".tmp";
            int fd_tmp = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd_tmp < 0) return false;
            const bool written = ::pwrite(fd_tmp, &rec, sizeof(rec), 0) == (ssize_t)sizeof(rec) && ::fsync(fd_tmp) == 0;
            ::close(fd_tmp);
            if (!written) { ::remove(tmp.c_str()); return false; }
            if (::rename(tmp.c_str(), spec::checkpoint_path) != 0) { ::remove(tmp.c_str()); return false; }

            // The rename itself is only durable once the directory entry is,
            // which is the step almost everyone forgets.
            sync_parent_directory(spec::checkpoint_path);
            return true;
        }

        // Adopts a checkpoint if there is a consistent one, replaying nothing;
        // otherwise falls back to recover(). Returns the offset now in effect.
        //
        // A checkpoint is accepted ONLY when the log ends exactly where the
        // marker says. That is deliberately strict: it means nothing was
        // written after the checkpoint, so the data region and the index on
        // disk cannot have drifted past it, and there is no delta to replay.
        // Anything else - a crash after further writes, a compacted log, a
        // truncated file - falls back to a full replay, which is always
        // correct.
        std::size_t restart(std::size_t file_size) requires (spec::fast_restart) {
            checkpoint_record rec{};
            if (!read_checkpoint(rec)) return recover(file_size);
            if (rec.write_offset > file_size) return recover(file_size);

            // The log must end here: the next header must read as a sparse
            // tail, and the bytes just before must be the ones checkpointed.
            if (rec.write_offset + sizeof(record_header) <= file_size) {
                record_header next{};
                std::memcpy(&next, base + rec.write_offset, sizeof(next));
                if (next.id != 0) return recover(file_size);
            }
            if (log_tail_crc(rec.write_offset) != rec.tail_crc) return recover(file_size);
            if (!index.restore_count((std::size_t)rec.index_count)) return recover(file_size);

            write_offset = (std::size_t)rec.write_offset;
            last_synced = write_offset;
            data_offset = (std::size_t)rec.data_offset;
            live_byte_count = (std::size_t)rec.live_byte_count;
            next_id = (record_id)rec.next_id;
            last_record_offset = no_record; // the record it would point at was another run's
            restarted_from_checkpoint = true;
            return write_offset;
        }

        // Whether the last restart() adopted a checkpoint or fell back to
        // replaying. Worth exposing: "my start was fast" is a fact a caller
        // may need, e.g. to decide whether its own derived state is still
        // valid or has to be rebuilt.
        bool started_from_checkpoint() const noexcept { return restarted_from_checkpoint; }

        // Invalidates any checkpoint on disk. Compaction rewrites the log, so
        // every offset a checkpoint records becomes meaningless - and the
        // tail_crc check would catch it anyway, but removing the file says so
        // outright instead of relying on a mismatch.
        void discard_checkpoint() noexcept requires (spec::fast_restart) { ::remove(spec::checkpoint_path); }

        // file_size is the LOG's size. The data file is not inspected at all
        // - whatever a crash left in it is stale by definition, and this
        // scan rewrites it from offset 0 as each CREATE is replayed.
        std::size_t recover(std::size_t file_size) {
            data_offset = 0;
            std::size_t pos = scan(base, 0, file_size, [this](record_header hdr, const std::byte* payload) {
                apply(hdr, payload);
                return true;
            });
            write_offset = pos;
            last_synced = pos; // reading an existing file - treat it as already synced
            return pos;
        }

        // Applies WAL bytes from an external source (replication - see
        // replication.hpp) into this storage's own region, at this
        // storage's own current write_offset, exactly as if they had been
        // written locally: same checksum-verify-then-apply loop as
        // recover(), just fed by network arrival instead of a single
        // startup scan over a whole file. The caller (replication layer)
        // must ensure `bytes` is exactly the next contiguous range this
        // storage is missing - out-of-order or overlapping ranges are not
        // this method's problem to detect; see replication.hpp's own
        // msg_id-ordered reorder buffer, which is what guarantees that by
        // the time anything reaches here.
        //
        // The replica's file shares the primary's exact OFFSET numbering -
        // that's deliberate: it's what makes "resume from offset X after a
        // reconnect" trivially meaningful on both sides, rather than needing
        // a translated/independent numbering scheme. The bytes are not
        // identical, and never were: a replica is unsplit, so its index
        // aliases its own log and update() patches CREATE payloads there
        // (and now refreshes their checksums too). Only lengths and
        // positions have to agree, and they do - pad8() of the same header
        // and length gives the same stride on both sides.
        //
        // Returns how many bytes were actually verified and applied - less
        // than bytes.size() if a checksum failed or the range was
        // truncated partway through a record, mirroring recover()'s own
        // "stop at the first bad record" behavior. The caller should treat
        // anything past the returned count as not yet applied and not
        // advance its own bookkeeping past it.
        std::size_t apply_wal_range(const std::byte* bytes, std::size_t len) {
            std::memcpy(base + write_offset, bytes, len);

            // Everything below reads the just-copied destination, never
            // `bytes` directly. Two reasons: `bytes` is a caller-provided
            // buffer (a network payload, for replication's actual use) with
            // no alignment guarantee at all, unlike every other caller of
            // checksum_of/crc32c in this file; and index entries must point
            // into this storage's own mapped memory, not into the caller's
            // temporary buffer.
            const std::size_t end = scan(base, write_offset, write_offset + len,
                                         [this](record_header hdr, const std::byte* payload) {
                apply(hdr, payload);
                return true;
            });
            std::size_t consumed = end - write_offset;
            write_offset = end;
            return consumed;
        }

        // Flushes the LOG, and only the log - the delta since the last
        // sync(), never the whole file. When it returns, every record
        // appended so far is durable; that is the whole guarantee, and it is
        // enough, because the log is the source of truth and the data file
        // is a rebuildable cache (see the file header).
        //
        // The msync range is honoured for writeback, but the filesystem
        // commit it triggers underneath (msync -> vfs_fsync_range -> the fs's
        // ->fsync, a log/transaction commit on btrfs) is per-INODE and cannot
        // be scoped to a range. That is exactly why the data file is a
        // separate inode: the log's inode now has no dirty pages except the
        // tail being flushed, so "commit the whole inode" and "commit the
        // tail" have become the same thing.
        //
        // Measured, btrfs, 200k random-key durable updates
        // (bench/probe_split_log_and_data.cpp), same workload, same
        // guarantee, only the layout differs:
        //     one file (log and data together)    3.0 s   ~1010 MiB written
        //     log and data in separate files      0.2 s     ~40 MiB written
        //
        // What did NOT go away: the data file's dirty pages still reach the
        // disk eventually, via background writeback. Splitting moves that
        // work off the critical path; it does not delete it.
        void sync() noexcept {
            if (write_offset <= last_synced) return;
            std::size_t aligned_start = last_synced & ~(page_size - 1);
            ::msync(base + aligned_start, write_offset - aligned_start, MS_SYNC);
            last_synced = write_offset;
        }

        // The observer instance, so the layer above can reach whatever state
        // it maintains (its index, its counters) through the storage that
        // feeds it.
        observer& observer_state() noexcept { return obs; }
        const observer& observer_state() const noexcept { return obs; }

        std::size_t live_bytes() const noexcept { return live_byte_count; }

        // Live data as a fraction of the LOG, so this measures what
        // compaction actually reclaims: not just dead records but every
        // update() and destroy() ever appended. It is therefore the right
        // trigger for a second reason - the log's length is also the restart
        // cost, since recover() replays all of it.
        double garbage_ratio() const noexcept {
            return write_offset == 0 ? 0.0 : 1.0 - (double)(live_byte_count) / write_offset;
        }
        std::size_t current_offset() const noexcept { return write_offset; }

        // How much of the data region the materialized records occupy. Above
        // live_bytes() by whatever recreate()/destroy() left behind there
        // since the last compaction. Always 0 for shared/replicated, which
        // have no separate data region to account for.
        std::size_t data_region_offset() const noexcept { return data_offset; }

        // Non-disruptive snapshot export: the same "every live record,
        // freshly encoded as a CREATE" walk compact() uses internally, but
        // purely read-only - this storage's own base/write_offset/index
        // are completely untouched. Used by replication's snapshot
        // bootstrap (see replication.hpp) to give a new or badly-behind
        // replica a fresh baseline without forcing an actual compaction
        // cycle on the primary just because some replica happened to join
        // or fall behind. Two passes (size first, then write) so the
        // returned buffer is exactly right-sized - no caller-provided
        // scratch buffer to size correctly by hand.
        std::vector<std::byte> export_snapshot() const {
            std::size_t needed = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].offset) continue;
                needed += pad8(sizeof(record_header) + header_of(record_ptr(id))->len);
            }
            std::vector<std::byte> scratch(needed);
            std::size_t scratch_offset = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].offset) continue;
                std::byte* rec = record_ptr(id);
                const record_header* old_hdr = header_of(rec);
                write_record_at<record_op::create>(scratch.data(), scratch_offset, id, 0, old_hdr->tag,
                                                    std::span<const std::byte>{rec, old_hdr->len});
            }
            return scratch;
        }

        // Resets this storage back to a fresh, empty state - write_offset,
        // live_byte_count, next_id, and the index are all cleared, as if
        // this were a brand-new storage over the same (still-mapped)
        // region. Used when replication needs to discard a replica's
        // state entirely and rebuild from a fresh snapshot (see
        // replication.hpp) - a full snapshot is only ever meaningful
        // applied on top of nothing, never appended after whatever a
        // (possibly desynced) replica already had.
        void reset() {
            write_offset = 0;
            last_synced = 0;
            last_record_offset = no_record;
            data_offset = 0;
            live_byte_count = 0;
            next_id = 1;
            index.clear();
        }

        // =================================================================
        // Compaction, Variant 1: blocking. Halts all use of the storage for
        // the duration (single-threaded logic, no new synchronization).
        // scratch_base is a region the caller has already opened/mapped.
        // Each live record reads its own len/tag straight from its header
        // (header_of) - T is never needed, so this works across any mix of
        // types in the storage at once.
        // =================================================================
        // Two overloads rather than one with a defaulted second scratch: a
        // split storage that compacted into a single region would silently
        // point its index back into the log and quietly undo the split. A
        // `requires` makes that a compile error at the call site instead.
        void compact(std::byte* scratch_base, int scratch_fd = -1) requires (!split_regions) {
            compact_into(scratch_base, nullptr, scratch_fd, -1);
        }

        // scratch_data_base must be a DIFFERENT region from the current data
        // region: records are read from the old one while being written to
        // the new one, and their relative order is not guaranteed to make an
        // in-place shuffle safe.
        void compact(std::byte* scratch_base, std::byte* scratch_data_base,
                     int scratch_fd = -1, int scratch_data_fd = -1) requires (split_regions) {
            compact_into(scratch_base, scratch_data_base, scratch_fd, scratch_data_fd);
        }

        // --- Below: methods for background_compact (Variant 2) only, not
        // for general use. Public because background_compact is a free
        // function, not a friend - by convention, call these only through
        // storage::background_compact(), never directly.

        std::byte* raw_base() const noexcept { return base; }

        // Where one live record sits inside the snapshot arena. Owns
        // nothing - the same principle the storage itself uses (bump an
        // offset, never free individually). offset is size_t, not uint32_t:
        // it is the CUMULATIVE position across every record snapshotted,
        // unlike record_header::len (one record's own length, genuinely
        // bounded by uint32_t). len == 0 means "no record with this id",
        // which is unambiguous because a tagged_aggregate always has
        // sizeof >= 1.
        struct arena_ref { std::size_t offset; uint32_t len; tag_t tag; };

        // A stable copy of every live record, taken under the lock in phase 1
        // and caught up on the WAL without the lock in phase 2.
        //
        // BOTH halves are file-backed, for the same reason the data region
        // is. This used to be a std::vector plus a std::unordered_map - an
        // arena the size of the live set plus ~64 bytes per record, all of it
        // anonymous memory, which the kernel can only swap and never reclaim,
        // allocated while phase 1 holds the writer's mutex. Measured at 400k
        // records of 64 bytes it was 49.7 MB against 25 MB of live data, and
        // it was the last allocation in the engine that grew with the dataset
        // and could not be evicted (bench/probe_snapshot_cost.cpp).
        //
        // Both files are unlinked the moment they are mapped: they live
        // exactly as long as the mapping, are never reopened, and cannot be
        // left behind by a crash.
        class snapshot {
        public:
            static constexpr std::size_t npos = (std::size_t)-1;

            snapshot() = default;
            ~snapshot() { close(); }
            snapshot(const snapshot&) = delete;
            snapshot& operator=(const snapshot&) = delete;

            bool open(const char* arena_path, std::size_t arena_bytes, const char* refs_path, std::size_t entries) {
                close();
                arena = map_unlinked(arena_path, arena_bytes, arena_fd);
                refs = (arena_ref*)map_unlinked(refs_path, entries * sizeof(arena_ref), refs_fd);
                if (!arena || !refs) { close(); return false; }
                arena_capacity = arena_bytes;
                refs_bytes = entries * sizeof(arena_ref);
                ref_capacity = entries;
                arena_used = 0;
                overflow = false;
                return true;
            }

            // Copies len bytes in and returns where they landed, or npos if
            // the arena is full. A full arena is not recoverable mid-way, so
            // it also latches `overflow` - background_compact checks that
            // before switching anything over, and abandons the compaction
            // rather than switch to a snapshot that silently lost records.
            std::size_t append(const std::byte* src, uint32_t len) noexcept {
                if (arena_used + len > arena_capacity) { overflow = true; return npos; }
                std::memcpy(arena + arena_used, src, len);
                std::size_t at = arena_used;
                arena_used += len;
                return at;
            }

            bool set(record_id id, tag_t tag, std::size_t offset, uint32_t len) noexcept {
                if (id >= ref_capacity) { overflow = true; return false; }
                refs[id] = arena_ref{ offset, len, tag };
                return true;
            }

            arena_ref* find(record_id id) const noexcept {
                return (id < ref_capacity && refs[id].len != 0) ? refs + id : nullptr;
            }
            void erase(record_id id) noexcept { if (id < ref_capacity) refs[id].len = 0; }

            std::byte* bytes() const noexcept { return arena; }
            std::size_t used() const noexcept { return arena_used; }
            bool overflowed() const noexcept { return overflow; }

        private:
            static std::byte* map_unlinked(const char* path, std::size_t bytes, int& out_fd) noexcept {
                int opened = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
                if (opened < 0) return nullptr;
                // Reserved blocks read back as zeros, so every ref starts
                // with len == 0, i.e. absent, without touching a page.
                if (::posix_fallocate(opened, 0, bytes) != 0) { ::close(opened); return nullptr; }
                void* m = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, opened, 0);
                if (m == MAP_FAILED) { ::close(opened); return nullptr; }
                ::unlink(path); // the mapping keeps it alive; nothing is left on disk
                out_fd = opened;
                return (std::byte*)m;
            }
            void close() noexcept {
                if (arena) { ::munmap(arena, arena_capacity); arena = nullptr; }
                if (refs) { ::munmap(refs, refs_bytes); refs = nullptr; }
                if (arena_fd >= 0) { ::close(arena_fd); arena_fd = -1; }
                if (refs_fd >= 0) { ::close(refs_fd); refs_fd = -1; }
                arena_capacity = arena_used = refs_bytes = ref_capacity = 0;
            }
            std::byte* arena = nullptr;
            arena_ref* refs = nullptr;
            std::size_t arena_used = 0, arena_capacity = 0, refs_bytes = 0, ref_capacity = 0;
            int arena_fd = -1, refs_fd = -1;
            bool overflow = false;
        };

        void snapshot_into(snapshot& out) const {
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].offset) continue;
                std::byte* rec = record_ptr(id);
                const record_header* h = header_of(rec);
                std::size_t off = out.append(rec, h->len);
                if (off == snapshot::npos) return; // overflow latched; the caller aborts
                out.set(id, h->tag, off, h->len);
            }
        }

        // Replays the WAL range [from, to) on top of an EXTERNAL snapshot -
        // never touches this storage's index, so it's safe to call WITHOUT
        // the lock (the WAL is immutable in this range: bytes before `to`
        // are never mutated again, only appended past it).
        std::size_t replay_into(std::byte* wal_base, std::size_t from, std::size_t to, snapshot& snap) const {
            return scan(wal_base, from, to, [&](record_header hdr, const std::byte* payload) {
                switch (hdr.op) {
                    case record_op::create: {
                        // recreate() of the same id: the old range in the
                        // arena simply becomes dead weight, not reclaimed
                        // (the whole arena is discarded after
                        // finish_compaction - not worth the complexity of
                        // reusing fragments).
                        std::size_t off = snap.append(payload, hdr.len);
                        if (off == snapshot::npos) return false; // overflow latched
                        snap.set(hdr.id, hdr.tag, off, hdr.len);
                        break;
                    }
                    case record_op::update: {
                        arena_ref* ref = snap.find(hdr.id);
                        if (ref && (std::size_t)(hdr.offset) + hdr.len <= ref->len)
                            std::memcpy(snap.bytes() + ref->offset + hdr.offset, payload, hdr.len);
                        break;
                    }
                    case record_op::destroy:
                        snap.erase(hdr.id);
                        break;
                }
                return true;
            });
        }

        // Final step: writes the entire snapshot into scratch as fresh
        // CREATEs, then switches index/base/write_offset/last_synced
        // to the new region. Call only while holding the lock (see
        // background_compact).
        void finish_compaction(std::byte* scratch, snapshot& snap, int scratch_fd = -1) requires (!split_regions) {
            finish_compaction_into(scratch, nullptr, snap, scratch_fd, -1);
        }
        void finish_compaction(std::byte* scratch, std::byte* scratch_data, snapshot& snap,
                               int scratch_fd = -1, int scratch_data_fd = -1) requires (split_regions) {
            finish_compaction_into(scratch, scratch_data, snap, scratch_fd, scratch_data_fd);
        }

    private:
        // The three op bodies. Every path into the storage - a live write, a
        // replayed record, a replicated one - ends in exactly one of these.
        void apply_create(const record_header& hdr, const std::byte* data) {
            index_entry* slot = reserve(hdr.id);
            if (!slot) return; // id beyond a file-backed index's capacity
            obs.on_record(*this, hdr, data);
            if (slot->offset) live_byte_count -= header_of(data_base + slot->offset)->len;
            slot->offset = offset_in_data(materialize(hdr.id, hdr.tag, data, hdr.len));
            live_byte_count += hdr.len;
        }

        template <provenance from, std::size_t payload_len>
        void apply_update(const record_header& hdr, const std::byte* data) {
            if (!alive(hdr.id)) return;
            std::byte* rec = record_ptr(hdr.id);
            // A diff has to land inside the record it claims to patch. For a
            // local update the offset came from a pointer-to-member and there
            // is nothing to re-derive; for a record off the disk or a wire it
            // is just a number that happened to checksum.
            if constexpr (from == provenance::external) {
                if ((std::size_t)hdr.offset + hdr.len > header_of(rec)->len) return;
            }
            obs.on_record(*this, hdr, data);
            // A known length is one load and one store. An unknown one has to
            // dispatch on it, which is what copy_field exists for.
            if constexpr (payload_len != 0) std::memcpy(rec + hdr.offset, data, payload_len);
            else copy_field(rec + hdr.offset, data, hdr.len);
            refresh_after_patch(hdr.id);
        }

        void apply_destroy(const record_header& hdr) {
            if (!alive(hdr.id)) return;
            std::byte* rec = record_ptr(hdr.id);
            obs.on_record(*this, hdr, nullptr);
            live_byte_count -= header_of(rec)->len;
            index[hdr.id].offset = 0;
        }

        static record_header* header_of(std::byte* data) noexcept {
            return (record_header*)(data - sizeof(record_header));
        }
        static const record_header* header_of(const std::byte* data) noexcept {
            return (const record_header*)(data - sizeof(record_header));
        }

        // An UPDATE's payload is one `field`, so its length is almost always
        // 1, 2, 4 or 8 - but hdr.len is read from the record at run time, and
        // a memcpy with a run-time length compiles to the general libc call
        // with its size-class dispatch. That call is not a rounding error at
        // this granularity: routing update() through apply() with a plain
        // memcpy doubled the benchmark's instruction count (102M -> 202M) and
        // cost 4x on random updates (37.5M -> 8.7M ops/s), for copying eight
        // bytes. Naming the common sizes lets each one inline to a single
        // load and store, and the branch is perfectly predicted by any
        // workload that touches one field repeatedly.
        static void copy_field(std::byte* dst, const std::byte* src, uint32_t len) noexcept {
            switch (len) {
                case 8: std::memcpy(dst, src, 8); return;
                case 4: std::memcpy(dst, src, 4); return;
                case 2: std::memcpy(dst, src, 2); return;
                case 1: *dst = *src; return;
                default: std::memcpy(dst, src, len); return;
            }
        }

        // THE verified walk over a log range, shared by recover(),
        // apply_wal_range(), replay_into() and for_each_record(). Each of
        // those used to carry its own copy of this loop - four places that
        // had to agree about the sparse-tail check, the truncation check,
        // the checksum and the pad8 stride, and that had already drifted
        // once. fn returns false to stop; the returned offset is where the
        // walk stopped, and the record that failed a check (or that fn
        // rejected) is never counted as consumed.
        static std::size_t scan(const std::byte* region, std::size_t from, std::size_t to, auto&& fn) {
            std::size_t pos = from;
            while (pos + sizeof(record_header) <= to) {
                record_header hdr;
                std::memcpy(&hdr, region + pos, sizeof(hdr));
                if (hdr.id == 0) break;                          // sparse tail, not a real record
                if (pos + sizeof(hdr) + hdr.len > to) break;     // truncated tail
                if (checksum_of(hdr, region + pos + sizeof(hdr), hdr.len) != hdr.checksum) break;
                if (!fn(hdr, region + pos + sizeof(hdr))) break;
                pos += pad8(sizeof(hdr) + hdr.len);
            }
            return pos;
        }

        // Where index[id].data should point for a CREATE whose payload has
        // just been appended to the log at `payload`.
        //
        // Unsplit (shared/replicated): nowhere new - the index aliases the
        // log payload directly, exactly as it always did. No copy, no second
        // region, byte-for-byte the same behaviour as before the split.
        //
        // Split (local_sync): a second copy, header and all, in the data
        // region. The header comes along because header_of() is how len and
        // tag are read back everywhere (get, for_each_raw, destroy,
        // compaction), and keeping it beside the payload leaves all of that
        // untouched. The payload really is written twice for a create -
        // that is the price of the split, and it buys a durability point
        // that no longer scales with how scattered update()'s patches are.
        std::byte* materialize(record_id id, tag_t tag, const std::byte* payload, uint32_t len) {
            if constexpr (!split_regions) return (std::byte*)payload;
            else return write_record_at<record_op::create>(data_base, data_offset, id, 0, tag,
                                                           std::span<const std::byte>{payload, len});
        }

        // Shared body of both compact() overloads. scratch_data_base is
        // nullptr exactly when the storage is unsplit.
        void compact_into(std::byte* scratch_base, std::byte* scratch_data_base,
                          int scratch_fd = -1, int scratch_data_fd = -1) {
            // Slots are rewritten as the loop goes, so the new offsets must be
            // measured against the region they will end up in, not against
            // data_base - which is still the OLD region every record is being
            // read from, and only becomes the new one after the loop.
            std::byte* const target_data = split_regions ? scratch_data_base : scratch_base;
            std::size_t scratch_offset = 0, scratch_data_offset = 0;
            live_byte_count = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].offset) continue; // dead record - not carried over
                std::byte* old_rec = record_ptr(id);
                const record_header* old_hdr = header_of(old_rec);
                std::span<const std::byte> payload{old_rec, old_hdr->len};
                std::byte* logged = write_record_at<record_op::create>(scratch_base, scratch_offset, id, 0, old_hdr->tag, payload);
                std::byte* placed = logged;
                if constexpr (split_regions)
                    placed = write_record_at<record_op::create>(scratch_data_base, scratch_data_offset, id, 0, old_hdr->tag, payload);
                index[id].offset = (std::size_t)(placed - target_data);
                live_byte_count += old_hdr->len;
            }
            adopt_region(base, fd, scratch_base, scratch_fd);
            write_offset = scratch_offset;
            last_record_offset = no_record; // the record it pointed at is gone
            // last_synced MUST be reset: scratch has never been synced, and
            // the old value (from the previous, larger region) could exceed
            // the new write_offset - sync() would then conclude there's
            // nothing to flush and silently skip the whole compacted region.
            last_synced = 0;
            if constexpr (split_regions) { adopt_region(data_base, data_fd, scratch_data_base, scratch_data_fd); data_offset = scratch_data_offset; }
            else data_base = scratch_base;
        }

        void finish_compaction_into(std::byte* scratch, std::byte* scratch_data, snapshot& snap,
                                    int scratch_fd = -1, int scratch_data_fd = -1) {
            std::byte* const target_data = split_regions ? scratch_data : scratch;
            std::size_t scratch_offset = 0, scratch_data_offset = 0;
            live_byte_count = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                const arena_ref* ref_ptr = snap.find(id);
                if (!ref_ptr) { index[id].offset = 0; continue; } // died during catch-up
                const arena_ref& ref = *ref_ptr;
                std::span<const std::byte> payload{snap.bytes() + ref.offset, ref.len};
                std::byte* logged = write_record_at<record_op::create>(scratch, scratch_offset, id, 0, ref.tag, payload);
                std::byte* placed = logged;
                if constexpr (split_regions)
                    placed = write_record_at<record_op::create>(scratch_data, scratch_data_offset, id, 0, ref.tag, payload);
                index[id].offset = (std::size_t)(placed - target_data);
                live_byte_count += ref.len;
            }
            adopt_region(base, fd, scratch, scratch_fd);
            write_offset = scratch_offset;
            last_record_offset = no_record;
            last_synced = 0;
            if constexpr (split_regions) { adopt_region(data_base, data_fd, scratch_data, scratch_data_fd); data_offset = scratch_data_offset; }
            else data_base = scratch;
        }

        bool alive(record_id id) const noexcept {
            return id < index.size() && index[id].offset != 0;
        }

        std::byte* record_ptr(record_id id) const noexcept { return data_base + index[id].offset; }

        // CRC of the last few KB before `end`, which is what binds a
        // checkpoint to the log it was taken from: a compacted or rewritten
        // log has different bytes there even at the same offset.
        uint32_t log_tail_crc(std::size_t end) const noexcept {
            const std::size_t len = end < checkpoint_tail_bytes ? end : checkpoint_tail_bytes;
            return crc32c(base + end - len, len) ^ 0xFFFFFFFFu;
        }

        bool read_checkpoint(checkpoint_record& out) const noexcept {
            int cf = ::open(spec::checkpoint_path, O_RDONLY);
            if (cf < 0) return false;
            const bool got = ::pread(cf, &out, sizeof(out), 0) == (ssize_t)sizeof(out);
            ::close(cf);
            if (!got || out.magic != checkpoint_magic || out.version != checkpoint_version) return false;
            return (crc32c(&out, offsetof(checkpoint_record, checksum)) ^ 0xFFFFFFFFu) == out.checksum;
        }

        // A rename is not durable until the directory entry naming it is.
        // Skipping this is the classic way to lose a file that fsync() said
        // was safe.
        static void sync_parent_directory(const char* path) noexcept {
            std::string dir(path);
            const std::size_t slash = dir.find_last_of('/');
            dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash ? slash : 1);
            int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
            if (dfd < 0) return;
            ::fsync(dfd);
            ::close(dfd);
        }

        // An unsplit storage's index aliases the log, so patching a field
        // rewrites bytes its CREATE's checksum covers - leave it stale and
        // the next recover() stops dead at that record. A split storage
        // patches the data region, which no checksum in the log describes,
        // so there is nothing to refresh and this compiles away.
        void refresh_after_patch(record_id id) noexcept {
            if constexpr (!split_regions) {
                std::byte* rec = record_ptr(id);
                record_header* h = header_of(rec);
                h->checksum = checksum_of(*h, rec, h->len);
            }
        }
        std::size_t offset_in_data(const std::byte* p) const noexcept { return (std::size_t)(p - data_base); }

        // Compaction builds a fresh region and hands it over; this takes it
        // and releases whatever the storage was using. Before it existed,
        // base was simply reassigned, so every compaction leaked a mapping
        // of spec::capacity and its file descriptor - unbounded, once per
        // compaction, for the life of the process.
        //
        // Only a storage that OWNS its regions has anything to release. An
        // unsplit (shared/replicated) storage never did: the caller passed
        // the region in and keeps it, so this only rebinds the pointer.
        //
        // The incoming region is adopted, not borrowed - ~storage() will
        // unmap it. The caller may close its file descriptor afterwards (the
        // mapping keeps the inode alive), but must not unmap it itself.
        // Takes over BOTH the mapping and the file behind it. The fd used to
        // be closed and left at -1, which lost the storage its own file: after
        // a compaction log_fd() returned -1, so uring_gather and direct_gather
        // read from a closed descriptor, release_log()'s posix_fadvise silently
        // did nothing, and checkpoint() named an offset in a file it could no
        // longer fsync. The mapping was adopted; the file was dropped.
        //
        // fresh_fd < 0 keeps the old behaviour and is correct for a caller
        // whose scratch is not a file at all - a vector, or an anonymous
        // mapping, as several tests use.
        void adopt_region(std::byte*& region, int& region_fd, std::byte* fresh, int fresh_fd) noexcept {
            if constexpr (split_regions) {
                if (region && region != fresh) ::munmap(region, spec::capacity);
                if (region_fd >= 0 && region_fd != fresh_fd) ::close(region_fd);
            }
            region = fresh;
            region_fd = fresh_fd;
        }

        // Grows with the total number of ids ever issued (next_id only
        // ever increases), not with the current live-record count - a
        // destroyed record's index slot is never reclaimed, just left
        // null. 8 bytes per id-ever-created; fine for the monotonic
        // single-writer id space this storage assumes, worth knowing
        // before assuming index memory tracks occupancy rather than
        // lifetime churn.
        // nullptr when the slot cannot be had - only possible for a
        // file-backed index, whose capacity is fixed at open (see
        // index_array). Callers refuse the record rather than proceed.
        index_entry* reserve(record_id id) {
            index_entry* slot = index.reserve_slot(id);
            if (!slot) return nullptr;
            next_id = std::max(next_id, id + 1);
            return slot;
        }

        // Pointer-to-member-through-a-null-base offset trick: the same
        // idiom pre-C++11 codebases used to hand-roll offsetof() for
        // non-aggregate access. Dereferencing through a null pointer is
        // technically UB per the letter of the standard, but for a
        // standard-layout `field` type (required by the `field` concept
        // above) every mainstream ABI gives a fixed, predictable member
        // offset independent of the object's actual address - this works
        // everywhere in practice, which is why the trick is still common.
        // Not something to build on for a non-standard-layout T; the
        // `field` constraint on `member_value_t<member>` is what makes it
        // safe here.
        template <auto member>
        static std::size_t member_offset() noexcept {
            return (std::size_t)(&((member_class_t<member> const volatile*)(0)->*member));
        }

        // op is now an NTTP, not a runtime parameter: at every real call
        // site (create/recreate/compact -> create; update -> update;
        // destroy -> destroy) it is already known statically - it was just
        // being threaded through as an ordinary argument before. The
        // op == create ternary becomes if constexpr: three specializations
        // instead of one runtime branch on the write hot path. apply()/
        // recover() deliberately do NOT get this treatment - there, hdr.op
        // comes from the file: it's data, not a compile-time constant, and
        // can only be branched on with a switch.
        //
        // target_base/target_offset need not be this storage's own
        // base/write_offset: compaction writes into a scratch region
        // through the same path, so the checksum/pad8 formula isn't
        // duplicated.
        template <record_op op>
        static std::byte* write_record_at(std::byte* target_base, std::size_t& target_offset,
                                          record_id id, uint32_t offset, tag_t tag,
                                          std::span<const std::byte> bytes) {
            record_header hdr{ id, offset, (uint32_t)(bytes.size()), op, 0, tag }; // flags start clear
            // ONE rule for every op: the checksum covers the header and the
            // payload together. CREATE used to be excepted, because its
            // payload was the record and update() patched it in place, so a
            // payload-covering checksum would have gone stale on the first
            // update. The consequence was that a CREATE's payload was
            // unprotected - a torn write that landed the header's sector but
            // not the payload's, or later bit rot, passed recovery silently
            // and was applied as a real value.
            //
            // Split storages made the exception unnecessary: their log is
            // never patched. Unsplit ones keep patching it and now refresh
            // this checksum when they do (see refresh_after_patch), which is
            // cheaper than leaving every created record undefended.
            hdr.checksum = checksum_of(hdr, bytes.data(), (uint32_t)bytes.size());
            std::byte* pos = target_base + target_offset;
            std::memcpy(pos, &hdr, sizeof(hdr));
            if (!bytes.empty()) std::memcpy(pos + sizeof(hdr), bytes.data(), bytes.size());
            target_offset += pad8(sizeof(hdr) + bytes.size());
            return pos + sizeof(hdr);
        }

        template <record_op op>
        std::byte* write_record(record_id id, uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            last_record_offset = write_offset;
            return write_record_at<op>(base, write_offset, id, offset, tag, bytes);
        }

        static constexpr std::size_t page_size = 4096; // de facto standard on x86_64 Linux

        std::byte* base = nullptr;          // the log - append-only, the source of truth
        std::size_t write_offset = 0;
        std::size_t last_synced = 0;
        std::size_t last_record_offset = no_record; // start of the newest record in the log
        bool restarted_from_checkpoint = false;

        // The materialized records. A separate file only when split_regions;
        // otherwise it aliases base and data_offset stays 0, because nothing
        // is ever written through it.
        std::byte* data_base = nullptr;
        std::size_t data_offset = 0;

        [[no_unique_address]] observer obs{};
        std::size_t live_byte_count = 0;
        record_id next_id = 1;
        int fd = -1;
        int data_fd = -1;
        index_array index;
    };

    // =====================================================================
    // batch_commit - unchanged in substance: table is just a storage<spec>&,
    // its API (sync()) is the same as before. Group commit: drains the
    // queue in batches (by count OR by time, whichever comes first),
    // applies each item, and issues ONE sync() per batch.
    // =====================================================================
    void batch_commit(auto& table, auto& queue, std::atomic<bool>& running, auto&& apply,
                      std::size_t max_batch = 1000,
                      std::chrono::milliseconds max_window = std::chrono::milliseconds(5)) {
        while (running.load(std::memory_order_relaxed)) {
            auto first = queue.pop_blocking();
            if (!first) continue;

            apply(table, *first);
            std::size_t batch_count = 1;
            auto deadline = std::chrono::steady_clock::now() + max_window;
            while (batch_count < max_batch && std::chrono::steady_clock::now() < deadline) {
                auto item = queue.pop();
                if (!item) break;
                apply(table, *item);
                ++batch_count;
            }
            table.sync();
        }
    }

    // "Compaction-ready" variant - same logic, but holds mu for the
    // duration of each batch and calls post_batch afterward. Needed ONLY
    // when background_compact() is running concurrently in another thread
    // - it uses the same mu to safely interleave between batches.
    void batch_commit(auto& table, auto& queue, std::atomic<bool>& running, std::mutex& mu, auto&& apply, auto&& post_batch,
                      std::size_t max_batch = 1000,
                      std::chrono::milliseconds max_window = std::chrono::milliseconds(5)) {
        while (running.load(std::memory_order_relaxed)) {
            auto first = queue.pop_blocking(); // without the lock - otherwise the compactor never gets a turn
            if (!first) continue;

            {
                std::lock_guard<std::mutex> lock(mu); // held only for the duration of the batch
                apply(table, *first);
                std::size_t batch_count = 1;
                auto deadline = std::chrono::steady_clock::now() + max_window;
                while (batch_count < max_batch && std::chrono::steady_clock::now() < deadline) {
                    auto item = queue.pop();
                    if (!item) break;
                    apply(table, *item);
                    ++batch_count;
                }
                table.sync();
            }
            post_batch(table); // e.g. check garbage_ratio() and trigger compaction
        }
    }

    // =====================================================================
    // background_compact - Variant 2: non-blocking compaction. Two SHORT
    // locked sections (initial snapshot, final catch-up + switchover); the
    // bulk of the work runs lock-free. REQUIRES the writer to also take mu
    // on every batch (see batch_commit above) - otherwise the lock
    // accomplishes nothing.
    //
    // storage<spec> is not a template parameter of the function itself -
    // spec is deduced from table; the snapshot is an arena (offset+len
    // into a shared buffer), never mallocs per record, and never names T,
    // so this works for any storage holding any mix of types.
    //
    // max_catchup_rounds/catchup_threshold_bytes tune phase 2's lock-free
    // catch-up loop below - defaults match what was previously hardcoded.
    //
    // tmp_data_path/live_data_path are required for a split (local_sync)
    // storage and ignored otherwise: compaction has to produce a fresh data
    // region alongside the fresh log, and it cannot reuse the live one
    // (records are read from it while the new layout is written). The data
    // file is renamed into place like the log purely so the on-disk state
    // keeps matching spec::data_path - nothing reads it across a restart.
    // =====================================================================
    template <typename spec, typename observer>
    void background_compact(storage<spec, observer>& table, std::mutex& mu,
                            const char* tmp_path, const char* live_path,
                            std::size_t scratch_capacity,
                            const char* tmp_data_path = nullptr, const char* live_data_path = nullptr,
                            int max_catchup_rounds = 5, std::size_t catchup_threshold_bytes = 4096) {
        typename storage<spec, observer>::snapshot snapshot;
        // The snapshot's two files are derived from tmp_path and unlinked as
        // soon as they are mapped, so they need no cleanup and cannot be left
        // behind. Sizing the arena at scratch_capacity is the same bound the
        // caller already asserted for the compacted log.
        const std::string arena_path = std::string(tmp_path) + ".arena";
        const std::string refs_path = std::string(tmp_path) + ".refs";
        if (!snapshot.open(arena_path.c_str(), scratch_capacity, refs_path.c_str(), spec::index_entries)) return;

        std::size_t caught_up_to;
        std::byte* wal_base;

        { // --- PHASE 1 (short lock): snapshot the current values ---
            std::lock_guard<std::mutex> lock(mu);
            table.snapshot_into(snapshot);
            caught_up_to = table.current_offset();
            wal_base = table.raw_base();
        }
        if (snapshot.overflowed()) return; // storage untouched - nothing has been switched yet

        // --- PHASE 2 (lock-free): catch up on the WAL in a few passes,
        // each one getting closer to "now". Gives up after
        // max_catchup_rounds regardless of how far behind it still is -
        // under a sustained write rate that never drops below
        // catchup_threshold_bytes per round, phase 3 below absorbs
        // whatever's left over. Still correct, but phase 3's lock is no
        // longer "short" in that case: raise max_catchup_rounds (or lower
        // catchup_threshold_bytes) if that's ever measured to matter under
        // your real write rate.
        for (int round = 0; round < max_catchup_rounds; ++round) {
            std::size_t current;
            { std::lock_guard<std::mutex> lock(mu); current = table.current_offset(); }
            if (current - caught_up_to < catchup_threshold_bytes) break; // close enough - move to the final phase
            caught_up_to = table.replay_into(wal_base, caught_up_to, current, snapshot);
        }

        int tmp_fd = ::open(tmp_path, O_RDWR | O_CREAT, 0644);
        ::posix_fallocate(tmp_fd, 0, scratch_capacity);
        std::byte* scratch = (std::byte*)(::mmap(nullptr, scratch_capacity, PROT_READ | PROT_WRITE, MAP_SHARED, tmp_fd, 0));

        bool switched_over = false;
        int tmp_data_fd = -1;
        std::byte* scratch_data = nullptr;
        if constexpr (storage<spec, observer>::split_regions) {
            tmp_data_fd = ::open(tmp_data_path, O_RDWR | O_CREAT, 0644);
            ::posix_fallocate(tmp_data_fd, 0, scratch_capacity);
            scratch_data = (std::byte*)(::mmap(nullptr, scratch_capacity, PROT_READ | PROT_WRITE, MAP_SHARED, tmp_data_fd, 0));
        }

        { // --- PHASE 3 (short lock): final catch-up, write, switchover,
          // and sync. sync() MUST happen inside this lock, not after it
          // releases: write_offset/last_synced are the same fields the
          // writer thread mutates on every batch_commit call (see
          // storage::sync()) - calling sync() unlocked here would race
          // with that, reading and writing those fields from two threads
          // with no synchronization between them.
            std::lock_guard<std::mutex> lock(mu);
            std::size_t final_offset = table.current_offset();
            table.replay_into(wal_base, caught_up_to, final_offset, snapshot);
            // Checked before anything is switched over: a snapshot that ran
            // out of arena or ran past its last ref slot is MISSING records,
            // and compacting to it would delete them. Abandoning the
            // compaction leaves the storage exactly as it was.
            if (snapshot.overflowed()) { switched_over = false; }
            else {
                if constexpr (storage<spec, observer>::split_regions) table.finish_compaction(scratch, scratch_data, snapshot);
                else table.finish_compaction(scratch, snapshot);
                table.sync();
                switched_over = true;
            }
        }

        ::close(tmp_fd);
        if constexpr (storage<spec, observer>::split_regions) ::close(tmp_data_fd);

        if (!switched_over) {
            // Nothing adopted the scratch regions, so this function still
            // owns them, and the temp files are not the live ones.
            ::munmap(scratch, scratch_capacity);
            ::remove(tmp_path);
            if constexpr (storage<spec, observer>::split_regions) { ::munmap(scratch_data, scratch_capacity); ::remove(tmp_data_path); }
            return;
        }

        ::rename(tmp_path, live_path);
        if constexpr (storage<spec, observer>::split_regions) ::rename(tmp_data_path, live_data_path);
    }

} // namespace storage
// ---- end storage/storage.hpp

// ---- begin storage/field_storage.hpp -----------------------------------
// field_storage.hpp - the same log, indexed per FIELD instead of per record.
//
// A sibling of storage.hpp, not a mode of it. They share the record format,
// the checksums, the concepts and the tag discipline - a log written by one is
// readable by the other's scanner - but they differ in the one thing that
// cannot be a runtime flag: what an index entry means, and therefore what a
// read returns.
//
//   storage.hpp        index[id] -> the record's bytes, kept contiguous by
//                      patching them in place. get<T>() hands back a pointer.
//                      Two regions: the log, and the materialization.
//
//   this file          index[id] -> the base CREATE, plus a chain saying where
//                      the newest version of each overridden FIELD lives in
//                      the log. Nothing is ever patched. get<T>() assembles
//                      into a caller's object, because the record is not
//                      contiguous anywhere. ONE region: the log.
//
// It is Bitcask with the granularity moved from the record to the field, which
// matters because update<member> is already field-granular: changing eight
// bytes of a kilobyte record writes eight bytes, where a B+tree or a heap
// table rewrites the row.
//
// WHAT IT BUYS, measured at 26 GB on a 31 GB machine (bench/big_dataset.cpp).
// Three runs of each, ALTERNATED, with writeback drained between them - which
// matters more than it sounds: an earlier attempt ran them back to back and
// saw a 5x spread that was the previous run's dirty pages, not the engine.
// Alternating and settling brings the spread to 1.1-1.6x.
//
//                          storage.hpp      this      ratio
//   fill (rec/sec)             185662     321555      1.7x
//   random read                  3168       7987      2.5x
//   random update                8120      11340      1.4x
//   read hot, before            10694      26400      2.5x
//   hot update x5                5419       8583      1.6x
//   read hot, after          31800000    3830000     0.12x   <- the cost
//   read hot, collapsed             -    9940000
//   major faults / read         0.397      0.233      1.7x
//   major faults / update       0.126     0.0016       78x
//   RSS                        6.1 GB    19.3 GB      3.2x worse
//   on disk                   56.2 GB    28.5 GB      2.0x
//
// UPDATES take 78x fewer major faults, and in two runs of three that was
// literally ONE fault per 200000 updates. An update appends the field and
// moves a pointer; it touches no page of the record, so there is nothing to
// fault on. Patching in place must reach a random page of a 26 GB region.
//
// READS get 2.5x faster, and the reason is not the index at all: half the
// footprint means twice as much of it fits in the page cache. 0.233 faults per
// read against 0.397. (An earlier version of this file retracted this claim
// after seeing read faults scatter across 27057, 75523 and 23535. That scatter
// was the measurement, not the engine - settled and alternated, three runs
// give 40969, 42292 and 56437 against 72541, 84295 and 81216.)
//
// WHAT IT COSTS, and this is now visible cleanly: reading a record that has
// five overridden fields is 8.3x slower than reading the same record from
// storage.hpp - 3.83M against 31.8M ops/sec, both entirely in cache, so it is
// pure CPU. The gather assembles from six places and copies a kilobyte into
// the caller's object where storage.hpp returns a pointer. Collapsing recovers
// most of it (9.94M) but not all, because the copy remains.
//
// So the two designs trade places, and the axis is whether the workload is
// I/O-bound: out of RAM this one wins nearly everything, in RAM (500k records,
// bench/kv_comparison.cpp) it loses reads 1.7x and updates 1.6x, and on a
// hot record with a deep chain it loses badly either way. spec::max_depth is
// the dial between those.
//
// The other cost is index memory: a slot per id (16 bytes) plus a node per
// overridden field (24 bytes), against storage.hpp's 8 bytes per id.


#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace storage {

    // =====================================================================
    // Where the newest version of one field lives. An update to a field that
    // is already overridden REPLACES its node rather than adding one, so a
    // chain is bounded by distinct fields touched, never by update count.
    // =====================================================================
    struct override_node {
        uint64_t log_offset;    // 8 - where the new bytes are in the log
        uint32_t field_offset;  // 4 - where they belong inside the record
        uint32_t len;           // 4
        uint32_t next;          // 4
        uint32_t reserved;      // 4
    };
    static_assert(sizeof(override_node) == 24);

    // The index words a reader may see while a writer is changing them, read
    // and written as single indivisible loads and stores.
    //
    // This is the price of admitting readers during a commit, and it is paid in
    // notation rather than in instructions: a relaxed atomic load or store of an
    // aligned word compiles to the same mov as the plain access on every
    // architecture this targets. What it buys is that the compiler may not
    // split, fuse or re-read the access - which is what makes a bounds check on
    // a possibly-torn value mean anything at all - and that ThreadSanitizer can
    // tell an intended optimistic read from a genuine bug.
    //
    // Both sides must use them. A relaxed load racing a plain store is still a
    // data race by the standard, so the writer's stores below are converted too.
    //
    // std::atomic_ref is the spelling; the compiler builtins are the fallback,
    // and the fallback is not hypothetical - libc++ only grew atomic_ref in
    // LLVM 19, and the Android NDK ships 18 as of r27, so the engine did not
    // compile for Android at all until this existed. The two are the same
    // instructions: atomic_ref lowers to exactly these builtins for a
    // lock-free type, and ThreadSanitizer instruments __atomic_* the same way,
    // so nothing above is weakened by taking the second path.
    //
    // STORAGE_FORCE_ATOMIC_BUILTINS exists so the fallback is a TESTED path and
    // not merely a compiled one. Without it the branch is dead code on any
    // machine whose library has atomic_ref - which is every machine this is
    // developed on - and dead code that only runs on a phone is how a release
    // ships something nobody ever executed. ctest builds and runs the smoke
    // test both ways.
#if defined(__cpp_lib_atomic_ref) && !defined(STORAGE_FORCE_ATOMIC_BUILTINS)
    template <typename word>
    inline word load_relaxed(const word& w) noexcept {
        return std::atomic_ref<word>(const_cast<word&>(w)).load(std::memory_order_relaxed);
    }
    template <typename word>
    inline void store_relaxed(word& w, word v) noexcept {
        std::atomic_ref<word>(w).store(v, std::memory_order_relaxed);
    }
#else
    template <typename word>
    inline word load_relaxed(const word& w) noexcept {
        return __atomic_load_n(&w, __ATOMIC_RELAXED);
    }
    template <typename word>
    inline void store_relaxed(word& w, word v) noexcept {
        __atomic_store_n(&w, v, __ATOMIC_RELAXED);
    }
#endif

    struct field_slot {
        uint64_t base;          // 8 - log offset of the CREATE's payload; 0 = no record

        // head AND depth in one word: 24 bits of node index, 8 bits of depth.
        // A node index is bounded by max_overrides and a depth by max_depth,
        // and neither is anywhere near its field - so spending a full uint32 on
        // each was spending 4 bytes to store a number below 16 million and 4
        // more to store one below 256.
        //
        // Worth the shifts because the index is the ONE structure that has to
        // stay resident: at 24 bytes a 27M-record index is 648 MB, at 16 it is
        // 432 MB, and the difference decides whether it survives memory
        // pressure. When it did not survive - measured under a 320 MB cap - all
        // six read paths collapsed to ~1000 ops/sec and became
        // indistinguishable, because every one of them needs the slot before it
        // can do anything at all.
        uint32_t chain;         // 4

        static constexpr uint32_t head_bits = 0x00FFFFFFu;
        uint32_t head() const noexcept { return load_relaxed(chain) & head_bits; }
        uint32_t depth() const noexcept { return load_relaxed(chain) >> 24; }
        void set_head(uint32_t h) noexcept { store_relaxed(chain, (load_relaxed(chain) & ~head_bits) | (h & head_bits)); }
        void set_depth(uint32_t d) noexcept { store_relaxed(chain, (load_relaxed(chain) & head_bits) | (d << 24)); }
        // Saturating, so a record that somehow outruns max_depth records 255
        // rather than wrapping to 0 and never collapsing again.
        void bump_depth() noexcept { if (depth() < 255) set_depth(depth() + 1); }

        // The record's LENGTH AND TAG, kept here rather than read back from the
        // header that precedes the payload in the log.
        //
        // This is not a cache, it is the difference between a read path that
        // can avoid the log and one that cannot. record_len() used to be
        // header_of(base + slots[id].base)->len - a dereference into the log
        // MAPPING, so merely learning how many bytes to read cost a page touch
        // per record. Measured on 512 MB under a 320 MB cap, that made every
        // read path identical to within 30% and left O_DIRECT taking 7272 major
        // faults per 10000 reads while reading its data into an aligned pool it
        // had specifically to avoid the page cache. It bypassed the cache for
        // the data and walked straight into it for the metadata.
        //
        // The 16 bytes come from the old inline_* fields, which were measured
        // and did not pay (variant H: 9.2M upd/s against the baseline's 12.5M
        // for the same 4.08M reads). The slot gets SMALLER, 24 bytes not 32.
        uint16_t len;           // 2
        tag_t    tag;           // 2
    };
    static_assert(sizeof(field_slot) == 16);

    inline constexpr uint32_t no_override = 0x00FFFFFFu; // fits field_slot::chain's 24 bits

    // The index file's header. Everything a restart needs that is not in the
    // arrays themselves.
    struct field_index_header {
        uint64_t magic;
        uint32_t version;
        uint32_t reserved;
        uint64_t slot_count;      // ids reserved so far
        uint64_t node_high_water; // first never-used node
        uint64_t node_free_head;  // head of the free list, or no_override
        uint64_t next_id;
        uint64_t live_byte_count;
        uint64_t write_offset;    // the log position these arrays describe
    };
    inline constexpr uint64_t field_index_magic = 0x584449444c454946ull; // "FIELDIDX"
    inline constexpr uint32_t field_index_version = 3; // 3: 16-byte slot, head+depth packed

    // =====================================================================
    // level is always local_sync here: a field storage owns its files by
    // construction - there is nowhere for a caller-provided region to fit,
    // because the index is not a simple array the caller could supply.
    // =====================================================================
    template <fixed_string path_v, std::size_t capacity_v,
              fixed_string index_path_v, std::size_t max_ids_v, std::size_t max_overrides_v,
              std::size_t max_depth_v = 8, std::size_t max_record_bytes_v = 4096,
              fixed_string checkpoint_path_v = fixed_string(""),
              std::size_t max_span_v = 0>
    struct field_spec {
        static constexpr auto path = path_v;
        static constexpr std::size_t capacity = capacity_v;
        static constexpr const char* index_path = index_path_v.value;
        static constexpr std::size_t max_ids = max_ids_v;
        static constexpr std::size_t max_overrides = max_overrides_v;

        // How many fields a record may have overridden before an update
        // collapses it back into one contiguous CREATE. This is the knob that
        // decides how far a read can degrade: a gather is at most 1 + max_depth
        // accesses, ever.
        static constexpr std::size_t max_depth = max_depth_v;

        // The widest record this storage will hold, which is the size of the
        // scratch buffer a collapse assembles into.
        static constexpr std::size_t max_record_bytes = max_record_bytes_v;

        // Collapse when the chain's overrides are spread further apart in the
        // log than this, rather than when there are more than max_depth of
        // them. Depth is only a proxy for what a gather costs; the spread is
        // the thing itself - two overrides in one page cost one fault, two
        // 20 GB apart cost two. 0 disables it.
        static constexpr std::size_t max_span = max_span_v;

        static constexpr const char* checkpoint_path = checkpoint_path_v.value;
        static constexpr bool fast_restart = checkpoint_path_v.value[0] != '\0';

        static_assert(index_path_v.value[0] != '\0', "a field storage keeps its index in a file - give the spec an index_path");
        static_assert(max_depth_v >= 1, "max_depth 0 would collapse on every update, which is just storage.hpp with extra steps");
        static_assert(max_depth_v < 255, "depth is 8 bits in field_slot::chain");
        static_assert(max_overrides_v < 0x00FFFFFFu, "a node index is 24 bits in field_slot::chain");
        static_assert(max_record_bytes_v <= 0xFFFFu, "field_slot::len is 16 bits");
        static constexpr std::size_t index_bytes =
            sizeof(field_index_header) + max_ids_v * sizeof(field_slot) + max_overrides_v * sizeof(override_node);
    };

    // WHERE THE FILES GO, decided at run time instead of in the spec.
    //
    // A spec names its files as compile-time strings, which is right when the
    // paths are known when the code is written and wrong in two situations that
    // both matter: a sandboxed process learns its directory only at startup
    // (every mobile platform), and running N instances side by side needs N
    // sets of names that a template parameter cannot produce from a loop.
    //
    // An empty field means "use the spec's". So the default constructor is
    // exactly what it always was, every existing spec keeps working untouched,
    // and this is opt-in per file.
    //
    // What this does NOT do is change whether a checkpoint exists.
    // spec::fast_restart is still what enables checkpoint()/restart(), because
    // that decides which members exist. Naming a checkpoint here relocates one;
    // it cannot bring one into being.
    struct storage_paths {
        std::string log;
        std::string index;
        std::string checkpoint;

        // Every file the spec names, moved under `directory` and keeping its
        // own basename. The common case, and the one a sandbox wants.
        template <typename spec>
        static storage_paths under(std::string_view directory) {
            const auto join = [&](const char* name) {
                std::string_view base = name;
                if (const auto cut = base.find_last_of('/'); cut != std::string_view::npos)
                    base.remove_prefix(cut + 1);
                if (base.empty()) return std::string{};
                std::string out(directory);
                if (!out.empty() && out.back() != '/') out += '/';
                out += base;
                return out;
            };
            return { join(spec::path.value), join(spec::index_path), join(spec::checkpoint_path) };
        }
    };

    // =====================================================================
    template <typename spec, typename observer = null_observer>
    class field_storage {
    public:
        // The spec this instance was built from, so a helper written against
        // `auto& table` can ask about its bounds - blob.hpp needs
        // max_record_bytes to refuse an over-long payload rather than truncate.
        using spec_type = spec;

        struct raw_view { record_id id; tag_t tag; std::span<const std::byte> bytes; };

        field_storage() : field_storage(storage_paths{}) {}

        explicit field_storage(storage_paths where)
            : log_path(where.log.empty() ? spec::path.value : std::move(where.log)),
              index_path(where.index.empty() ? spec::index_path : std::move(where.index)),
              checkpoint_path(where.checkpoint.empty() ? spec::checkpoint_path : std::move(where.checkpoint)) {
            fd = ::open(log_path.c_str(), O_RDWR | O_CREAT, 0644);
            if (fd < 0) return;
            if (platform::preallocate(fd, spec::capacity) != 0) return;
            base = (std::byte*)::mmap(nullptr, spec::capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) { base = nullptr; return; }

            index_fd = ::open(index_path.c_str(), O_RDWR | O_CREAT, 0644);
            if (index_fd < 0) return;
            // Preserved across runs only when a checkpoint can vouch for it;
            // otherwise the previous run's contents are meaningless and
            // dropping them is what guarantees every slot starts empty.
            if constexpr (!spec::fast_restart) { if (::ftruncate(index_fd, 0) != 0) return; }
            if (platform::preallocate(index_fd, spec::index_bytes) != 0) return;
            void* m = ::mmap(nullptr, spec::index_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, index_fd, 0);
            if (m == MAP_FAILED) return;

            head = (field_index_header*)m;
            slots = (field_slot*)((std::byte*)m + sizeof(field_index_header));
            nodes = (override_node*)((std::byte*)slots + spec::max_ids * sizeof(field_slot));
            if (head->magic != field_index_magic || head->version != field_index_version) reset_header();
        }

        ~field_storage() {
            if (head && index_pinned) ::munlock(head, spec::index_bytes);
            if (head) ::munmap(head, spec::index_bytes);
            // Only if it IS a mapping. compact() can hand the engine a region
            // it did not map - see the base_is_mapped comment there.
            if (base && base_is_mapped) ::munmap(base, spec::capacity);
            if (index_fd >= 0) ::close(index_fd);
            if (fd >= 0) ::close(fd);
        }

        field_storage(const field_storage&) = delete;
        field_storage& operator=(const field_storage&) = delete;

        bool ok() const noexcept { return base && head; }

        observer& observer_state() noexcept { return obs; }
        const observer& observer_state() const noexcept { return obs; }

        // ---- writes -----------------------------------------------------

        template <tagged_aggregate T>
        record_id create(const T& value) {
            static_assert(sizeof(T) <= spec::max_record_bytes, "raise field_spec::max_record_bytes for this type");
            const record_id id = head->next_id++;
            if (id >= spec::max_ids) return 0;
            std::byte* logged = append<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            if (!logged) return 0;
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        // Claims an id without writing, for a caller that must name a record
        // before it is ready to append it - a staged transaction assigning ids
        // to records that later staged records refer to.
        [[nodiscard]] record_id reserve_id() noexcept { return head->next_id++; }

        // Type-erased recreate, for a caller holding a record's bytes and its
        // tag without naming its C++ type.
        record_id create_raw(record_id id, tag_t tag, std::span<const std::byte> bytes) {
            if (id >= spec::max_ids || bytes.size() > spec::max_record_bytes) return 0;
            std::byte* logged = append<record_op::create>(id, 0, tag, bytes);
            if (!logged) return 0;
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        // Sets bits in record_header::flags on the most recently appended
        // record and refreshes its checksum, so a layer above can mark a
        // boundary - the end of a transaction, say - without spending a
        // record on it. Same argument as storage.hpp's: the record was
        // appended since the last sync(), so its page is already dirty and
        // already inside the range the next msync covers.
        bool set_last_record_flags(uint8_t bits) noexcept {
            if (last_record_offset == no_record_yet) return false;
            record_header* h = (record_header*)(base + last_record_offset);
            h->flags |= bits;
            h->checksum = checksum_of(*h, base + last_record_offset + sizeof(record_header), h->len);
            return true;
        }

        template <tagged_aggregate T>
        record_id recreate(record_id id, const T& value) {
            static_assert(sizeof(T) <= spec::max_record_bytes, "raise field_spec::max_record_bytes for this type");
            if (id >= spec::max_ids) return 0;
            std::byte* logged = append<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            if (!logged) return 0;
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        template <auto member>
        requires field<member_value_t<member>>
        void update(record_id id, member_value_t<member> value) {
            if (!alive(id)) return;
            const std::size_t off = (std::size_t)&(((member_class_t<member> const volatile*)0)->*member);
            std::byte* logged = append<record_op::update>(id, (uint32_t)off, tag_t{0}, std::as_bytes(std::span{&value, 1}));
            if (!logged) return;
            apply_local<record_op::update>(*header_of(logged), logged);
        }

        // Run-time-offset counterpart of update<member>, for a caller holding
        // bytes without naming a type. Checked, since the offset is data.
        void patch(record_id id, uint32_t offset, std::span<const std::byte> bytes) {
            if (!alive(id)) return;
            if ((std::size_t)offset + bytes.size() > record_len(id)) return;
            std::byte* logged = append<record_op::update>(id, offset, tag_t{0}, bytes);
            if (!logged) return;
            apply_local<record_op::update>(*header_of(logged), logged);
        }

        void destroy(record_id id) {
            if (!alive(id)) return;
            std::byte* logged = append<record_op::destroy>(id, 0, tag_t{0}, {});
            if (!logged) return;
            apply_local<record_op::destroy>(*header_of(logged), logged);
        }

        // ---- reads ------------------------------------------------------
        //
        // By reference, not by pointer, and that is the API consequence of the
        // whole design: the record is the base CREATE plus its overrides, and
        // those live wherever the log happened to be when each was written.
        // There is no address at which the current record exists.

        // The by-value counterpart, so a caller written against storage.hpp's
        // pointer-returning get() compiles here too: `auto v = t.get<T>(id)`
        // and then `v->field` or `*v` mean the same thing for a pointer and
        // for an optional. That is the whole compatibility story - the layers
        // above this engine had to stop naming `const T*` and say `auto`.
        template <tagged_aggregate T>
        [[nodiscard]] std::optional<T> get(record_id id) const {
            T value;
            if (!get(id, value)) return std::nullopt;
            return value;
        }

        template <tagged_aggregate T>
        [[nodiscard]] bool get(record_id id, T& out) const {
            if (!alive(id) || record_len(id) != sizeof(T)) return false;
            assemble(id, (std::byte*)&out, sizeof(T));
            return true;
        }

        // The same reads, told what to return by the reference itself. A
        // ref<customer> can only produce a customer, so the type stops being
        // something the call site repeats and can therefore get wrong.
        template <tagged_aggregate T>
        [[nodiscard]] std::optional<T> get(ref<T> r) const { return get<T>(r.id()); }

        template <tagged_aggregate T>
        [[nodiscard]] bool get(ref<T> r, T& out) const { return get<T>(r.id(), out); }

        template <typename T>
        bool alive(ref<T> r) const noexcept { return alive(r.id()); }

        // A typed id for a record this instance is about to write, so a schema
        // can hold ref<T> fields without ever converting by hand.
        template <typename T>
        [[nodiscard]] ref<T> reserve_ref() noexcept { return ref<T>(reserve_id()); }

        template <tagged_aggregate T>
        ref<T> create_ref(const T& value) { return ref<T>(create(value)); }

        [[nodiscard]] bool read_raw(record_id id, std::span<std::byte> out) const {
            if (!alive(id)) return false;
            const uint32_t len = record_len(id);
            if (out.size() < len) return false;
            assemble(id, out.data(), len);
            return true;
        }

        [[nodiscard]] tag_t tag_of(record_id id) const noexcept {
            return alive(id) ? slots[id].tag : tag_t{0};
        }

        bool alive(record_id id) const noexcept { return id < head->slot_count && slots[id].base != 0; }
        uint32_t depth(record_id id) const noexcept { return alive(id) ? slots[id].depth() : 0; }

        template <tagged_aggregate T>
        void for_each(auto&& fn) const requires std::invocable<decltype(fn), record_id, const T&> {
            T value;
            for (record_id id = 1; id < head->slot_count; ++id) {
                if (!alive(id) || record_len(id) != sizeof(T)) continue;
                assemble(id, (std::byte*)&value, sizeof(T));
                fn(id, (const T&)value);
            }
        }

        void for_each_raw(auto&& fn) const requires std::invocable<decltype(fn), raw_view> {
            alignas(8) std::byte scratch[spec::max_record_bytes];
            for (record_id id = 1; id < head->slot_count; ++id) {
                if (!alive(id)) continue;
                const field_slot& s = slots[id];
                assemble(id, scratch, s.len);
                fn(raw_view{ id, s.tag, std::span<const std::byte>{ scratch, s.len } });
            }
        }

        // ---- the single application point --------------------------------

        // TWO ENTRY POINTS, ONE IMPLEMENTATION PER OP - the same split
        // storage.hpp makes, for the same measured reason. Bytes off a disk or
        // a wire carry their op in the header and dispatch at run time; a
        // record this process just wrote does not, and saying so in the type
        // is what lets the switch disappear. Left at run time it cost clang
        // 1.27x on the update path against gcc, purely from a function it
        // declined to inline because it carried all three cases.
        template <provenance from = provenance::external>
        void apply(record_header hdr, const std::byte* data) {
            if (hdr.id >= spec::max_ids) return;
            switch (hdr.op) {
                case record_op::create:  apply_create(hdr, data); break;
                case record_op::update:  apply_update<from>(hdr, data); break;
                case record_op::destroy: apply_destroy(hdr); break;
            }
        }

        template <record_op op>
        void apply_local(record_header hdr, const std::byte* data) {
            if (hdr.id >= spec::max_ids) return;
            if constexpr (op == record_op::create) apply_create(hdr, data);
            else if constexpr (op == record_op::update) apply_update<provenance::local>(hdr, data);
            else apply_destroy(hdr);
        }

        // ---- depth ------------------------------------------------------

        // Rewrites the record as one contiguous CREATE and drops its chain, so
        // the next read is a single access again. Called automatically once a
        // record passes spec::max_depth; exposed because a caller may know
        // better than the threshold does.
        void collapse(record_id id) {
            if (!alive(id) || slots[id].head() == no_override) return;
            const uint32_t len = slots[id].len;
            const tag_t tag = slots[id].tag;
            alignas(8) std::byte scratch[spec::max_record_bytes];
            assemble(id, scratch, len);
            std::byte* logged = append<record_op::create>(id, 0, tag, std::span<const std::byte>{ scratch, len });
            if (!logged) return;   // the log is full; the chain stays, which is correct, only slower
            free_chain(id);
            store_relaxed(slots[id].base, (uint64_t)(logged - base));
            ++collapses;
        }

        // Collapse driven by READS rather than by writes. The threshold on
        // update bounds the gather for everyone; this bounds it only for
        // records somebody actually looks at, which is a different trade: a
        // write-only record never pays to become contiguous, and a hot one
        // pays once. Call it after a get() when the caller knows the record
        // is worth flattening.
        void collapse_if_deeper_than(record_id id, uint32_t depth_limit) {
            if (alive(id) && slots[id].depth() > depth_limit) collapse(id);
        }

        // VARIANT B: re-append the record's overrides back to back at the log
        // tail and repoint the chain at the copies. Depth is unchanged, so the
        // gather still does the same number of memcpys - but they now come
        // from ONE place instead of k scattered ones, which is what a gather
        // actually pays for once the log stops fitting in memory.
        //
        // Costs the changed bytes, not the record: three uint64 fields of a
        // kilobyte record is 24 bytes against 1024. And it needs no format
        // change, because what it writes is ordinary update records that
        // recovery already knows how to replay.
        void compact_chain(record_id id) {
            if (!alive(id) || slots[id].head() == no_override) return;
            field_slot& s = slots[id];
            for (uint32_t n = s.head(); n != no_override; n = nodes[n].next) {
                override_node& o = nodes[n];
                std::byte* moved = append<record_op::update>(id, o.field_offset, tag_t{0},
                    std::span<const std::byte>{ base + o.log_offset, o.len });
                if (!moved) return;
                o.log_offset = (std::size_t)(moved - base);
            }
            ++chain_compactions;
        }

        // VARIANT A: collapse everything deeper than a threshold in ONE pass,
        // SORTED BY ADDRESS, so the reads feeding the gathers walk the log in
        // order instead of jumping. Meant to be driven by a background thread
        // or a maintenance tick - the point is that neither a read nor a write
        // pays for it on the critical path.
        std::size_t collapse_deep(uint32_t depth_limit) {
            sweep.clear();
            for (record_id id = 1; id < head->slot_count; ++id)
                if (alive(id) && slots[id].depth() > depth_limit) sweep.push_back({ slots[id].base, id });
            std::sort(sweep.begin(), sweep.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [addr, id] : sweep) collapse(id);
            return sweep.size();
        }

        std::size_t chain_compaction_count() const noexcept { return chain_compactions; }
        std::size_t refused_append_count() const noexcept { return refused_appends; }
        std::size_t collapse_count() const noexcept { return collapses; }

        // ---- durability --------------------------------------------------

        void sync() noexcept {
            if (write_offset <= last_synced) return;
            const std::size_t start = last_synced & ~(platform::page_size() - 1);
            open_read_window();
            platform::flush_mapping(base + start, write_offset - start, fd);
            close_read_window();
            last_synced = write_offset;
        }

        // ---- the sync window ---------------------------------------------
        //
        // A durable commit is time in which the writer holds the CPU and mutates
        // NOTHING: it is stopped inside msync waiting on the device. Measured on
        // the VPS matrix, that is 0.61 s of iowait out of 1.35 s wall for
        // 204800 inserts across 5451 barriers - 45% of the run in which the
        // index is provably still.
        //
        // Readers can have that time. Not by locking, which would put a
        // contended atomic on the append path where there is none today, but by
        // publishing a GENERATION that is odd exactly while a commit is in
        // flight. A reader that sees the same odd value before and after its
        // gather knows the whole gather happened inside one still window.
        //
        // The writer never waits for a reader. That is the point, and it is the
        // difference from the obvious design: a gate the writer has to drain
        // hands a reader that takes a 275 us major fault the power to stall the
        // commit path. Here a reader that overruns simply loses its read and
        // says so - try_get returns false and the caller retries or defers.
        //
        // WHERE THIS PAYS, precisely: readers that are CPU-bound while the
        // writer is device-bound. In RAM that is the whole workload, and threads
        // measured 231147 -> 560802 ops/sec at 4 of them. Out of RAM readers
        // want the same device the commit is waiting on and there is nothing to
        // win; see the prefetch note in FIELD_STORAGE.md.
        bool read_window_open() const noexcept { return (gate_generation.load(std::memory_order_acquire) & 1) != 0; }
        std::size_t read_window_count() const noexcept { return gate_generation.load(std::memory_order_relaxed) / 2; }

        // The reader's entry point, and the only const method on this class that
        // is safe to call from another thread while this one is writing.
        //
        // Returns false - having written nothing to `out` it promises - when the
        // window was shut, shut mid-gather, or the record does not survive the
        // bounds checks the optimistic path applies. False is not an error and
        // carries no information about the record; it means "not now".
        template <tagged_aggregate T>
        [[nodiscard]] bool try_get(record_id id, T& out) const noexcept {
            return try_read_raw(id, std::span<std::byte>{ (std::byte*)&out, sizeof(T) }) == sizeof(T);
        }

        // Returns the number of bytes assembled, or 0.
        [[nodiscard]] uint32_t try_read_raw(record_id id, std::span<std::byte> out) const noexcept {
            for (int attempt = 0; attempt < window_read_attempts; ++attempt) {
                const uint64_t before = gate_generation.load(std::memory_order_acquire);
                if ((before & 1) == 0) return 0;                    // no commit in flight

                // Snapshots taken while the window was shut, published by the
                // release on gate_generation above. The writer's own
                // write_offset and slot_count are plain members it stores to on
                // every append; reading them from here would be a data race in
                // the language whatever the hardware does.
                const std::size_t extent = gate_extent.load(std::memory_order_relaxed);
                const uint64_t slot_limit = gate_slots.load(std::memory_order_relaxed);

                if (id == 0 || id >= slot_limit) return 0;
                const uint16_t len = load_relaxed(slots[id].len);
                if (len == 0 || len > out.size()) return 0;
                if (!assemble<true>(id, out.data(), len, extent)) return 0;

                // The gather is done. If the generation is still the odd value
                // it started at, the writer was inside the same msync the whole
                // time and touched neither slot nor node.
                std::atomic_thread_fence(std::memory_order_acquire);
                if (gate_generation.load(std::memory_order_relaxed) == before) return len;
            }
            return 0;
        }

        // Tells the kernel we are done with the log below `upto`: first drop
        // our page-table entries for it, then ask for the page-cache pages
        // themselves. The data is not lost - this mapping is MAP_SHARED over a
        // file, so the next access re-faults it in.
        //
        // Only CLEAN pages actually go. A page still dirty is skipped and left
        // to writeback, which is why this deliberately does NOT force an msync
        // first: paying a filesystem commit to free memory would trade the
        // exact thing the log/data split was built to avoid.
        //
        // Meant for a bulk load, where the pages behind the write cursor are
        // pure exhaust and are evicting the pages that reads actually want.
        void release_log(std::size_t upto) noexcept {
            if (!base || upto < platform::page_size()) return;
            const std::size_t end = (upto < write_offset ? upto : write_offset) & ~(platform::page_size() - 1);
            if (end == 0) return;
            ::madvise(base, end, MADV_DONTNEED);
            platform::release_prefix(fd, end);
        }

        std::size_t current_offset() const noexcept { return write_offset; }
        std::size_t live_bytes() const noexcept { return head->live_byte_count; }
        std::size_t overrides_used() const noexcept { return head->node_high_water; }
        double garbage_ratio() const noexcept {
            return write_offset == 0 ? 0.0 : 1.0 - (double)head->live_byte_count / write_offset;
        }

        // A PREDICATE, not a background task, and the difference is structural:
        // compaction needs a fresh region, and the engine has no way to obtain
        // one - the caller supplies it, along with its file. So the engine can
        // only say whether compacting is worth doing; the caller decides when
        // it can afford to.
        //
        // Two bounds, both of which must hold. The ratio is the point: below it
        // the rewrite moves more live bytes than it reclaims. The floor keeps a
        // young log from tripping it - a log holding one record and one update
        // is 50% garbage by ratio and worth nothing to compact, and without a
        // floor a caller polling this would compact continuously from startup.
        bool compaction_due(double min_garbage = 0.5, std::size_t min_bytes = 64ull << 20) const noexcept {
            return write_offset >= min_bytes && garbage_ratio() >= min_garbage;
        }

        // What a compaction would leave behind, so a caller can size the region
        // it is about to hand over. Live bytes plus one header each, rounded up
        // - the same arithmetic compact() performs as it writes.
        std::size_t compacted_size() const noexcept {
            std::size_t n = 0;
            for (record_id id = 1; id < head->slot_count; ++id)
                if (alive(id)) n += pad8(sizeof(record_header) + slots[id].len);
            return n;
        }

        // Replays the log. Identical in shape to storage.hpp's: verify, then
        // hand each record to the same apply() a live write goes through, so
        // an observer maintained above cannot drift from the data.
        std::size_t recover(std::size_t file_size) {
            reset_header();
            last_record_offset = no_record_yet;
            const std::size_t pos = scan(base, 0, file_size, [this](record_header hdr, const std::byte* payload) {
                apply(hdr, payload);
                return true;
            });
            write_offset = pos;
            last_synced = pos;
            head->write_offset = pos;
            return pos;
        }

        // ---- replication --------------------------------------------------

        // Back to empty, keeping the files. A snapshot is only meaningful
        // applied on top of nothing, so a replica adopting one discards what it
        // had first; this is that discard.
        //
        // Everything goes, including the node arena and next_id, because a
        // snapshot names its own ids and a stale high-water mark would hand out
        // nodes that the incoming state is about to claim. The log's bytes are
        // left where they are - write_offset returning to zero is what makes
        // them unreachable, and the next append overwrites them.
        void reset() noexcept {
            write_offset = 0;
            last_synced = 0;
            last_record_offset = no_record_yet;
            from_checkpoint = false;
            reset_header();
        }



        // Applies a run of raw WAL bytes - records exactly as they sit in
        // another storage's log - by APPENDING THEM VERBATIM to this one and
        // then replaying from the local copy.
        //
        // Verbatim matters twice over. The bytes are already complete records
        // with their own checksums, so copying them keeps this log
        // byte-identical to the one it is following, and therefore keeps the
        // two agreeing on offsets - which is what lets a replica resume by
        // naming one.
        //
        // And the replay must run off the LOCAL copy, never off the caller's
        // buffer. A slot's base is an offset into this mapping; applying
        // straight from a network payload would record an offset into memory
        // that is about to be reused, and the record would read as garbage the
        // moment the buffer went away. The caller's buffer also carries no
        // alignment guarantee, unlike everything else that reaches
        // checksum_of() here.
        //
        // Returns the number of bytes actually applied, which is less than len
        // if the run is truncated or a checksum fails - the same rule recovery
        // uses, and the same conclusion: stop at the first record that cannot
        // be trusted.
        std::size_t apply_wal_range(const std::byte* bytes, std::size_t len) {
            if (!base || !len) return 0;
            if (write_offset + len > spec::capacity) { ++refused_appends; return 0; }
            std::memcpy(base + write_offset, bytes, len);
            const std::size_t from = write_offset;
            const std::size_t end = scan(base, from, from + len, [this](record_header hdr, const std::byte* payload) {
                apply(hdr, payload);
                return true;
            });
            write_offset = end;
            head->write_offset = end;
            if (end > from) last_record_offset = from;   // approximate, and only used for flags
            return end - from;
        }

        // Every live record re-encoded as one contiguous CREATE, for seeding a
        // replica from scratch or after a desync.
        //
        // Assembling is what makes this a snapshot rather than a copy: a record
        // with a chain is written as the single CREATE it currently amounts to,
        // so the receiver gets a state, not a history. It also means the result
        // is independent of how the log got that way, which is exactly what a
        // replica that has fallen out of step needs.
        //
        // Does NOT disturb this storage: nothing is appended, no chain is
        // freed, no offset moves. Unlike compact(), which does all three.
        std::vector<std::byte> export_snapshot() const {
            std::size_t needed = 0;
            for (record_id id = 1; id < head->slot_count; ++id)
                if (alive(id)) needed += pad8(sizeof(record_header) + slots[id].len);

            std::vector<std::byte> out(needed);
            alignas(8) std::byte scratch[spec::max_record_bytes];
            std::size_t at = 0;
            for (record_id id = 1; id < head->slot_count; ++id) {
                if (!alive(id)) continue;
                const uint32_t len = slots[id].len;
                assemble(id, scratch, len);
                write_record_at<record_op::create>(out.data(), at, id, 0, slots[id].tag,
                                                   std::span<const std::byte>{ scratch, len });
            }
            return out;
        }

        std::size_t for_each_record(std::size_t from, std::size_t to, auto&& fn) const {
            return scan(base, from, to, [&](record_header hdr, const std::byte* payload) {
                if constexpr (std::is_void_v<decltype(fn(hdr, std::span<const std::byte>{payload, hdr.len}))>) {
                    fn(hdr, std::span<const std::byte>{payload, hdr.len});
                    return true;
                } else {
                    return fn(hdr, std::span<const std::byte>{payload, hdr.len});
                }
            });
        }
        std::size_t for_each_record(std::size_t from, auto&& fn) const { return for_each_record(from, write_offset, fn); }

        // Flushes the log and the index together, then names the offset they
        // agree at. Same ordering argument as storage.hpp: everything the
        // marker describes is durable before the marker exists.
        bool checkpoint() noexcept requires (spec::fast_restart) {
            sync();
            head->write_offset = write_offset;
            // The index flush is the LONGER of the two windows - it is the whole
            // arena, not the log's tail - and the writer is just as idle in it.
            open_read_window();
            platform::flush_mapping(head, spec::index_bytes, index_fd);
            close_read_window();

            checkpoint_record rec{};
            rec.magic = checkpoint_magic;
            rec.version = checkpoint_version;
            rec.write_offset = write_offset;
            rec.data_offset = 0;
            rec.live_byte_count = head->live_byte_count;
            rec.next_id = head->next_id;
            rec.index_count = head->slot_count;
            rec.tail_crc = log_tail_crc(write_offset);
            rec.checksum = crc32c(&rec, offsetof(checkpoint_record, checksum)) ^ 0xFFFFFFFFu;

            const std::string tmp = checkpoint_path + ".tmp";
            int cf = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (cf < 0) return false;
            const bool written = ::pwrite(cf, &rec, sizeof(rec), 0) == (ssize_t)sizeof(rec) && platform::flush_file(cf) == 0;
            ::close(cf);
            if (!written) { ::remove(tmp.c_str()); return false; }
            if (::rename(tmp.c_str(), checkpoint_path.c_str()) != 0) { ::remove(tmp.c_str()); return false; }
            sync_parent_directory(checkpoint_path.c_str());
            return true;
        }

        std::size_t restart(std::size_t file_size) requires (spec::fast_restart) {
            checkpoint_record rec{};
            if (!read_checkpoint(rec)) return recover(file_size);
            if (rec.write_offset > file_size) return recover(file_size);
            if (head->magic != field_index_magic || head->write_offset != rec.write_offset) return recover(file_size);
            if (rec.write_offset + sizeof(record_header) <= file_size) {
                record_header next{};
                std::memcpy(&next, base + rec.write_offset, sizeof(next));
                if (next.id != 0) return recover(file_size);
            }
            if (log_tail_crc(rec.write_offset) != rec.tail_crc) return recover(file_size);

            write_offset = (std::size_t)rec.write_offset;
            last_synced = write_offset;
            from_checkpoint = true;
            return write_offset;
        }

        bool started_from_checkpoint() const noexcept { return from_checkpoint; }
        void discard_checkpoint() noexcept requires (spec::fast_restart) { ::remove(checkpoint_path.c_str()); }

        // ---- compaction --------------------------------------------------

        // Rewrites every live record as one contiguous CREATE into a fresh
        // region, which drops every chain at once. Compaction here reclaims
        // more than dead records: the whole update history goes with it.
        // scratch_fd is the file behind scratch_base, and passing it is what
        // keeps this storage owning a file after a compaction. It used to close
        // its fd and leave it at -1: the mapping was adopted, the file was
        // dropped, and every path that needs a descriptor degraded silently -
        // log_fd() handed a closed one to uring_gather and direct_gather,
        // release_log()'s posix_fadvise did nothing, and checkpoint() named an
        // offset in a file it could no longer fsync.
        //
        // -1 keeps the old behaviour, which is right for scratch that is not a
        // file: a vector, or an anonymous mapping, as the tests use.
        //
        // scratch_is_mapped is what the destructor needs to know, and getting it
        // wrong crashed on Android before it crashed anywhere else. compact()
        // ADOPTS scratch_base as the engine's base, and the destructor used to
        // munmap that unconditionally - so handing it a std::vector meant
        // munmap() on the heap. glibc tolerated it well enough that the test
        // passed for months; bionic on a 16 KB-page device turned it into a
        // SIGSEGV, which is the honest reaction.
        //
        // It defaults to false because a vector is the common case in this
        // repo's own tests, and because the wrong answer in that direction
        // leaks a mapping while the wrong answer in the other corrupts the
        // heap. A caller passing a real mmap should say so; passing a file
        // descriptor implies it.
        void compact(std::byte* scratch_base, int scratch_fd = -1, bool scratch_is_mapped = false) {
            std::size_t scratch_offset = 0;
            alignas(8) std::byte scratch[spec::max_record_bytes];
            std::size_t live = 0;
            for (record_id id = 1; id < head->slot_count; ++id) {
                if (!alive(id)) continue;
                const uint32_t len = slots[id].len;
                const tag_t tag = slots[id].tag;
                assemble(id, scratch, len);
                std::byte* placed = write_record_at<record_op::create>(scratch_base, scratch_offset, id, 0, tag,
                                                                       std::span<const std::byte>{ scratch, len });
                free_chain(id);
                slots[id].base = (std::size_t)(placed - scratch_base);
                live += len;
            }
            if (base && base_is_mapped && base != scratch_base) ::munmap(base, spec::capacity);
            if (fd >= 0 && fd != scratch_fd) ::close(fd);
            fd = scratch_fd;
            base = scratch_base;
            base_is_mapped = scratch_is_mapped || scratch_fd >= 0;
            write_offset = scratch_offset;
            last_synced = 0;
            head->live_byte_count = live;
            head->write_offset = scratch_offset;

            // Every chain is provably gone - each record was just rewritten as
            // one contiguous CREATE - so the arena resets rather than being
            // left as a free list under a high-water mark that never comes
            // back down.
            head->node_high_water = 0;
            head->node_free_head = no_override;
        }

        std::byte* raw_base() const noexcept { return base; }

        // The log's file descriptor, and the record's bytes described as FILE
        // OFFSETS rather than as addresses in the mapping. A pointer-returning
        // API hides where the data is; an io_uring read needs to be told.
        int log_fd() const noexcept { return fd; }

        // Pulls the slot toward the core before anything asks for it. Every
        // read begins by loading slots[id], and at 16 bytes with random ids
        // that is one cache line per lookup that nothing can predict - the
        // access is id -> slot, and an id says nothing about the previous one.
        // Issued a batch ahead, the miss overlaps with work instead of stalling
        // in front of it.
        void prefetch(record_id id) const noexcept {
            if (id < spec::max_ids) __builtin_prefetch(&slots[id], 0, 3);
        }

        // Makes the index's residency a decision instead of an outcome.
        //
        // Not a tuning knob. Under a 320 MB cap the index was evicted along
        // with everything else, and because every read path needs the slot
        // before it can issue anything, all six of them - mmap, pread, two ring
        // depths, two O_DIRECT depths - landed within 30% of each other at
        // ~1000 ops/sec. The bottleneck was not the path, it was that the 2.3%
        // of bytes that gate every access were competing with the 97.7% that do
        // not.
        //
        // mlock is the real thing and needs RLIMIT_MEMLOCK; MADV_WILLNEED only
        // asks. Returns true only for the former, so a caller can tell which it
        // got rather than assume.
        bool pin_index() noexcept {
            if (!head) return false;
            if (::mlock(head, spec::index_bytes) == 0) { index_pinned = true; return true; }
            ::madvise(head, spec::index_bytes, MADV_WILLNEED);
            return false;
        }
        bool index_is_pinned() const noexcept { return index_pinned; }

        // Emits the record as DISJOINT pieces that together cover [0, len)
        // exactly once: fn(file_offset, offset_within_record, length).
        //
        // Disjoint is the whole point and it is not an optimisation. assemble()
        // gets away with reading the whole base and then overwriting fields on
        // top because it runs in order. Reads issued to a ring complete in
        // whatever order the device likes, so a base read landing after an
        // override read would silently restore the stale bytes. Splitting the
        // base into the gaps BETWEEN the overridden fields removes the overlap,
        // and with it the ordering requirement - every piece can then be in
        // flight at once.
        //
        // Returns false when two overrides overlap, which only run-time patch()
        // calls can produce (update<member> cannot): there the result depends
        // on chain order, so the caller must use the ordered path instead.
        bool for_each_piece(record_id id, auto&& fn) const {
            if (!alive(id)) return false;
            const field_slot& s = slots[id];
            const uint32_t len = record_len(id);

            struct piece { uint32_t at; uint32_t len; std::size_t from; };
            piece parts[spec::max_depth + 2];
            std::size_t n = 0;
            for (uint32_t k = s.head(); k != no_override; k = nodes[k].next) {
                if ((std::size_t)nodes[k].field_offset + nodes[k].len > len) continue;
                if (n >= spec::max_depth + 2) return false;
                parts[n++] = { nodes[k].field_offset, nodes[k].len, nodes[k].log_offset };
            }
            std::sort(parts, parts + n, [](const piece& a, const piece& b) { return a.at < b.at; });

            // Validated BEFORE anything is emitted. Returning false halfway
            // would leave the caller having queued reads for a record it is
            // then told to redo, and those reads would land in the buffer
            // after the redo finished.
            uint32_t cursor = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (parts[i].at < cursor) return false;          // overlapping overrides
                cursor = parts[i].at + parts[i].len;
            }

            cursor = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (parts[i].at > cursor) fn(s.base + cursor, cursor, parts[i].at - cursor);
                fn(parts[i].from, parts[i].at, parts[i].len);
                cursor = parts[i].at + parts[i].len;
            }
            if (cursor < len) fn(s.base + cursor, cursor, len - cursor);
            return true;
        }

        // The pieces in ASSEMBLE ORDER instead: the whole base first, then each
        // override on top. They overlap, so a caller using this must apply them
        // in the order given - which a reader that copies out of its own pool
        // controls, and a reader writing straight into the caller's buffer does
        // not.
        //
        // Worth having because the disjoint form splits the base into up to d+1
        // fragments, and under O_DIRECT a fragment costs a whole block however
        // few bytes it is: measured 18.5x read amplification against 1 KB
        // records. This form is d+1 pieces with the base contiguous.
        void for_each_piece_ordered(record_id id, auto&& fn) const {
            if (!alive(id)) return;
            const field_slot& s = slots[id];
            const uint32_t len = record_len(id);
            fn(s.base, (uint32_t)0, len);
            for (uint32_t n = s.head(); n != no_override; n = nodes[n].next)
                if ((std::size_t)nodes[n].field_offset + nodes[n].len <= len)
                    fn(nodes[n].log_offset, nodes[n].field_offset, nodes[n].len);
        }

        uint32_t length_of(record_id id) const noexcept { return alive(id) ? record_len(id) : 0; }

    private:
        // The three op bodies. Every path into the storage - a live write, a
        // replayed record, a replicated one - ends in exactly one of these.
        void apply_create(const record_header& hdr, const std::byte* data) {
            if (hdr.len > spec::max_record_bytes) return;
            if (hdr.id >= head->slot_count) head->slot_count = hdr.id + 1;
            if (head->next_id <= hdr.id) head->next_id = hdr.id + 1;
            field_slot& s = slots[hdr.id];
            if (s.base) head->live_byte_count -= s.len;
            obs.on_record(*this, hdr, data);
            free_chain(hdr.id);
            store_relaxed(s.len, (uint16_t)hdr.len);
            s.tag = hdr.tag;
            store_relaxed(s.base, (uint64_t)(data - base));   // published last: base != 0 is what alive() tests
            head->live_byte_count += hdr.len;
        }

        template <provenance from>
        void apply_update(const record_header& hdr, const std::byte* data) {
            if (!alive(hdr.id)) return;
            // A diff has to land inside the record it claims to patch. For a
            // local update the offset came from a pointer-to-member; for a
            // record off the disk or a wire it is a number that checksummed.
            if constexpr (from == provenance::external) {
                if ((std::size_t)hdr.offset + hdr.len > record_len(hdr.id)) return;
            }
            obs.on_record(*this, hdr, data);
            set_override(hdr.id, hdr.offset, hdr.len, (std::size_t)(data - base));
            maybe_collapse(hdr.id);
        }

        void apply_destroy(const record_header& hdr) {
            if (!alive(hdr.id)) return;
            obs.on_record(*this, hdr, nullptr);
            head->live_byte_count -= record_len(hdr.id);
            free_chain(hdr.id);
            store_relaxed(slots[hdr.id].base, (uint64_t)0);
        }

        static record_header* header_of(std::byte* data) noexcept { return (record_header*)(data - sizeof(record_header)); }
        static const record_header* header_of(const std::byte* data) noexcept {
            return (const record_header*)(data - sizeof(record_header));
        }

        uint32_t record_len(record_id id) const noexcept { return slots[id].len; }

        // The gather. Base record first, then every overridden field on top -
        // newest chain node first, and since an update to an already-overridden
        // field replaces its node, every node here is the current value.
        //
        // `guarded` is the optimistic-read variant and it exists because a slot
        // read concurrently with set_override can be TORN - head and depth share
        // one 32-bit word, and a chain rewritten under a reader can hand it a
        // node index that was never valid. Unguarded, `nodes[n]` with a stray n
        // is a read off the end of the index arena, and a stray `next` is a
        // cycle. Neither is theoretical: it is exactly the race ThreadSanitizer
        // pointed at on field_slot::head().
        //
        // The checks cost one compare per chain node on a path whose body is a
        // memcpy, so they are free where they are needed. `if constexpr` keeps
        // them out of the exclusive path entirely rather than trusting the
        // branch predictor with them.
        // Every index word the optimistic path reads goes through this, and it
        // is not decoration. A plain read of a word a writer may be changing is
        // a data race, which C++ calls undefined and ThreadSanitizer calls a
        // race - but the concrete hazard is worse than either label. The
        // compiler is entitled to RELOAD a non-atomic value between the bounds
        // check and its use, so
        //
        //     if (o.log_offset + o.len > extent) return false;
        //     memcpy(dst, base + o.log_offset, o.len);
        //
        // may check one value and copy with another, and the check that exists
        // to keep the reader inside the mapping stops meaning anything. A
        // relaxed atomic load is one load that cannot be split or repeated. On
        // x86-64 it emits the same instruction as the plain read.
        template <bool guarded = false>
        bool assemble(record_id id, std::byte* out, uint32_t len,
                      std::size_t extent = 0) const noexcept {
            const field_slot& s = slots[id];
            if constexpr (guarded) {
                const uint64_t at = load_relaxed(s.base);
                if (at == 0 || at + len > extent) return false;
                std::memcpy(out, base + at, len);
            } else {
                std::memcpy(out, base + s.base, len);
            }
            uint32_t steps = 0;
            uint32_t n = guarded ? (load_relaxed(s.chain) & field_slot::head_bits) : s.head();
            for (; n != no_override; ) {
                if constexpr (guarded) {
                    // The index bound stops the out-of-arena read; the step
                    // bound stops a torn `next` that closed a loop. A real chain
                    // never exceeds max_depth, which is 8 bits wide.
                    if (n >= spec::max_overrides || ++steps > 256) return false;
                    const override_node& o = nodes[n];
                    const uint64_t at = load_relaxed(o.log_offset);
                    const uint32_t off = load_relaxed(o.field_offset), plen = load_relaxed(o.len);
                    if (at + plen > extent) return false;
                    if ((std::size_t)off + plen <= len)
                        std::memcpy(out + off, base + at, plen);
                    n = load_relaxed(o.next);
                } else {
                    const override_node& o = nodes[n];
                    if ((std::size_t)o.field_offset + o.len <= len)
                        std::memcpy(out + o.field_offset, base + o.log_offset, o.len);
                    n = o.next;
                }
            }
            return true;
        }

        // Publishing order matters and is the whole correctness argument: the
        // snapshots are stored first and the generation last, with release, so a
        // reader that acquires an odd generation is guaranteed to see the extent
        // and slot count that went with it.
        void open_read_window() noexcept {
            gate_extent.store(write_offset, std::memory_order_relaxed);
            gate_slots.store(head->slot_count, std::memory_order_relaxed);
            gate_generation.fetch_add(1, std::memory_order_release);
        }

        // Closing needs release too, so that a reader which is about to compare
        // generations cannot see the even value while still reading index words
        // the writer is now free to change.
        void close_read_window() noexcept {
            gate_generation.fetch_add(1, std::memory_order_release);
        }

        void set_override(record_id id, uint32_t offset, uint32_t len, std::size_t at) noexcept {
            field_slot& s = slots[id];
            // The walk that finds an existing override for this field also
            // measures how far apart the chain is, which costs nothing extra
            // because the nodes are being touched anyway.
            std::size_t lo = at, hi = at;
            for (uint32_t n = s.head(); n != no_override; n = nodes[n].next) {
                if (nodes[n].field_offset == offset && nodes[n].len == len) { store_relaxed(nodes[n].log_offset, (uint64_t)at); return; }
                lo = nodes[n].log_offset < lo ? nodes[n].log_offset : lo;
                hi = nodes[n].log_offset > hi ? nodes[n].log_offset : hi;
            }
            const uint32_t n = alloc_node();
            if (n == no_override) return; // out of nodes: the record keeps its previous value rather than a wrong one
            // The node is filled in BEFORE the slot points at it, so a reader
            // that follows the new head never sees an uninitialised node.
            override_node& o = nodes[n];
            store_relaxed(o.log_offset, (uint64_t)at);
            store_relaxed(o.field_offset, offset);
            store_relaxed(o.len, len);
            store_relaxed(o.next, s.head());
            o.reserved = 0;
            s.set_head(n);
            s.bump_depth();
            if constexpr (spec::max_span != 0) {
                if (hi - lo > spec::max_span) collapse(id);
            }
        }

        // The bound on how far a read can degrade. Without it a chain grows
        // with the number of DISTINCT fields a record has ever had touched,
        // and a gather grows with it.
        void maybe_collapse(record_id id) {
            if (slots[id].depth() > spec::max_depth) collapse(id);
        }

        uint32_t alloc_node() noexcept {
            if (head->node_free_head != no_override) {
                const uint32_t n = (uint32_t)head->node_free_head;
                head->node_free_head = nodes[n].next;
                return n;
            }
            if (head->node_high_water >= spec::max_overrides) return no_override;
            return (uint32_t)head->node_high_water++;
        }

        void free_chain_only(record_id id) noexcept {
            uint32_t n = slots[id].head();
            while (n != no_override) {
                const uint32_t next = load_relaxed(nodes[n].next);
                store_relaxed(nodes[n].next, (uint32_t)head->node_free_head);
                head->node_free_head = n;
                n = next;
            }
            slots[id].set_head(no_override);
        }

        void free_chain(record_id id) noexcept {
            free_chain_only(id);
            slots[id].set_depth(0);
        }

        // The capacity check the log did not have. Without it a workload that
        // collapses often - a low max_depth over wide records rewrites the
        // whole record on nearly every update - walks straight off the end of
        // the mapping and segfaults. Found by exactly that: max_depth 1 over
        // 32 fields wrote 8.8 GB into a 4 GB log.
        template <record_op op>
        std::byte* append(record_id id, uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            const std::size_t need = pad8(sizeof(record_header) + bytes.size());
            if (write_offset + need > spec::capacity) { ++refused_appends; return nullptr; }
            last_record_offset = write_offset;
            return write_record_at<op>(base, write_offset, id, offset, tag, bytes);
        }

        // Same verified walk as storage.hpp's, for the same reason: one place
        // that knows about sparse tails, truncation, checksums and the stride.
        static std::size_t scan(const std::byte* region, std::size_t from, std::size_t to, auto&& fn) {
            std::size_t pos = from;
            while (pos + sizeof(record_header) <= to) {
                record_header hdr;
                std::memcpy(&hdr, region + pos, sizeof(hdr));
                if (hdr.id == 0) break;
                if (pos + sizeof(hdr) + hdr.len > to) break;
                if (checksum_of(hdr, region + pos + sizeof(hdr), hdr.len) != hdr.checksum) break;
                if (!fn(hdr, region + pos + sizeof(hdr))) break;
                pos += pad8(sizeof(hdr) + hdr.len);
            }
            return pos;
        }

        template <record_op op>
        static std::byte* write_record_at(std::byte* target, std::size_t& target_offset, record_id id,
                                          uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            record_header hdr{ id, offset, (uint32_t)bytes.size(), op, 0, tag };
            hdr.checksum = checksum_of(hdr, bytes.data(), (uint32_t)bytes.size());
            std::byte* pos = target + target_offset;
            std::memcpy(pos, &hdr, sizeof(hdr));
            if (!bytes.empty()) std::memcpy(pos + sizeof(hdr), bytes.data(), bytes.size());
            target_offset += pad8(sizeof(hdr) + bytes.size());
            return pos + sizeof(hdr);
        }

        uint32_t log_tail_crc(std::size_t end) const noexcept {
            const std::size_t len = end < checkpoint_tail_bytes ? end : checkpoint_tail_bytes;
            return crc32c(base + end - len, len) ^ 0xFFFFFFFFu;
        }

        bool read_checkpoint(checkpoint_record& out) const noexcept {
            int cf = ::open(checkpoint_path.c_str(), O_RDONLY);
            if (cf < 0) return false;
            const bool got = ::pread(cf, &out, sizeof(out), 0) == (ssize_t)sizeof(out);
            ::close(cf);
            if (!got || out.magic != checkpoint_magic || out.version != checkpoint_version) return false;
            return (crc32c(&out, offsetof(checkpoint_record, checksum)) ^ 0xFFFFFFFFu) == out.checksum;
        }

        static void sync_parent_directory(const char* path) noexcept {
            std::string dir(path);
            const std::size_t slash = dir.find_last_of('/');
            dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash ? slash : 1);
            int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
            if (dfd < 0) return;
            ::fsync(dfd);
            ::close(dfd);
        }

        void reset_header() noexcept {
            std::memset(head, 0, sizeof(field_index_header));
            head->magic = field_index_magic;
            head->version = field_index_version;
            head->next_id = 1;
            head->slot_count = 1;      // id 0 is never a record
            head->node_free_head = no_override;
            std::memset(slots, 0, spec::max_ids * sizeof(field_slot));
            for (std::size_t i = 0; i < spec::max_ids; ++i) slots[i].chain = no_override; // head = none, depth = 0
        }

        std::byte* base = nullptr;
        std::size_t write_offset = 0;
        std::size_t last_synced = 0;
        static constexpr std::size_t no_record_yet = (std::size_t)-1;
        std::size_t last_record_offset = no_record_yet;
        std::size_t collapses = 0, chain_compactions = 0, refused_appends = 0;
        std::vector<std::pair<std::size_t, record_id>> sweep;
        bool from_checkpoint = false;
        bool index_pinned = false;
        // The constructor maps `base`; compact() may replace it with a region
        // the caller owns and the engine must not unmap.
        bool base_is_mapped = true;
        int fd = -1, index_fd = -1;

        // Resolved once in the constructor, from storage_paths or from the
        // spec. Held as strings rather than pointers because a caller building
        // a sandbox path has nowhere to keep it alive: the obvious call site is
        // field_storage(storage_paths::under<spec>(app_dir())), whose temporary
        // is gone before the first append.
        std::string log_path, index_path, checkpoint_path;

        // Odd while a commit is in flight. Even - the steady state - means the
        // writer owns the index and try_read_raw refuses. Bumped twice per
        // sync() and never on the append path, so the cost to a writer that
        // never has a concurrent reader is two uncontended atomic increments per
        // BARRIER, against an msync measured at 87 us.
        static constexpr int window_read_attempts = 4;
        alignas(64) std::atomic<uint64_t> gate_generation{0};
        std::atomic<std::size_t> gate_extent{0};
        std::atomic<uint64_t> gate_slots{0};
        char gate_padding[64 - 3 * sizeof(uint64_t)] = {};

        field_index_header* head = nullptr;
        field_slot* slots = nullptr;
        override_node* nodes = nullptr;
        [[no_unique_address]] observer obs{};
    };

} // namespace storage
// ---- end storage/field_storage.hpp

// ---- begin storage/prefetch_gather.hpp ---------------------------------
// prefetch_gather.hpp - queue depth without threads, without pread, without
// io_uring. Portable POSIX, and the read path itself does not change.
//
// THE PROBLEM, in the numbers it was measured with. A single-threaded reader
// over a mapping that does not fit in memory ran at 5731 records/sec while the
// same device, probed directly, sustained 239000 IOPS at queue depth 512. That
// is three multipliers, not one:
//
//   0.42 faults per read     8459 major faults over 20000 reads - not every
//                            record is cold, so the read rate and the fault
//                            rate are different numbers
//   queue depth 1            a page fault is one thread stopped on one
//                            request; the device needs hundreds in flight to
//                            reach 239000, and gives ~12000 with one
//   275 us per fault         against ~100 us of device: the trap, the page
//                            allocation, and - under memory pressure - having
//                            to evict something before there is a page to
//                            read into
//
// This file attacks the middle one only, and says so up front. The fault path
// and the eviction are the price of going through the page cache at all; only
// O_DIRECT avoids them, and O_DIRECT is not portable and not free (measured at
// 8.2x read amplification, since a 4 KB block is the minimum unit for a piece
// of any size).
//
// HOW. Before assembling a batch, hand the kernel the pages that batch is
// about to touch. posix_madvise(POSIX_MADV_WILLNEED) starts the reads and
// returns; several are then in flight while the caller is still assembling the
// records before them. Nothing else changes - reads still go through the same
// assemble(), the same mapping, the same index.
//
// WHY THE FIELD LAYOUT GETS MORE OUT OF THIS than an in-place one. The slot
// holds the base's offset AND every override's offset, in a resident structure,
// so a record with d overridden fields yields 1+d addresses that are all known
// before any of them is touched. A contiguous record yields one. The layout
// whose reads look scattered is the layout that can say in advance where it is
// going.
//
// A NOTE ON madvise, because this project has been burnt by it. MADV_RANDOM on
// this same mapping made everything worse - reads 3262 -> 2516, updates
// 10148 -> 6180, major faults on reads 72k -> 144k. That is a different
// mechanism: a hint about the ACCESS PATTERN, which disables the readahead that
// sequential fill and recovery depend on. WILLNEED names specific pages and
// asks for them once. Related only by the name of the syscall.


#include <algorithm>
#include <span>
#include <sys/mman.h>
#include <vector>

namespace storage {

    class prefetch_gather {
    public:
        // `window` records are prefetched ahead of the ones being assembled.
        // Too small and the device drains before the next batch is issued; too
        // large and the first pages are evicted again before they are read.
        explicit prefetch_gather(std::size_t window = 64, std::size_t page = 4096)
            : lookahead(window ? window : 1), page_size(page) {
            ranges.reserve(lookahead * 8);
        }

        struct result {
            std::size_t records = 0, pieces = 0, advises = 0, advised_bytes = 0;
        };

        template <typename engine>
        result read_many(const engine& e, std::span<const record_id> ids, std::byte* out, std::size_t stride) {
            result r{};
            const std::byte* base = e.raw_base();
            if (!base) return r;

            // Prime the first window, then keep one window ahead: the device
            // works on batch k+1 while the caller assembles batch k, instead of
            // alternating between a burst of I/O and a burst of memcpy.
            advise_window(e, base, ids, 0, r);

            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (i % lookahead == 0) advise_window(e, base, ids, i + lookahead, r);
                const uint32_t len = e.length_of(ids[i]);
                if (!len) continue;
                (void)e.read_raw(ids[i], std::span<std::byte>{ out + i * stride, len });
                ++r.records;
            }
            return r;
        }

    private:
        // One advise per CONTIGUOUS run rather than per piece. Each call is a
        // syscall, and a record's base plus its overrides frequently land in
        // the same page or in neighbouring ones - overrides written in the same
        // burst are adjacent in the log by construction. Merging turns a few
        // hundred calls into a few dozen and lets the kernel issue larger reads.
        template <typename engine>
        void advise_window(const engine& e, const std::byte* base,
                           std::span<const record_id> ids, std::size_t from, result& r) {
            if (from >= ids.size()) return;
            const std::size_t to = std::min(from + lookahead, ids.size());

            ranges.clear();
            for (std::size_t i = from; i < to; ++i) {
                e.for_each_piece_ordered(ids[i], [&](std::size_t off, uint32_t, uint32_t len) {
                    const std::size_t first = off & ~(page_size - 1);
                    const std::size_t last = (off + len + page_size - 1) & ~(page_size - 1);
                    ranges.push_back({ first, last });
                    ++r.pieces;
                });
            }
            if (ranges.empty()) return;

            std::sort(ranges.begin(), ranges.end(),
                      [](const range& a, const range& b) { return a.first < b.first; });

            std::size_t open_from = ranges[0].first, open_to = ranges[0].last;
            for (std::size_t i = 1; i < ranges.size(); ++i) {
                if (ranges[i].first <= open_to) {         // touching or overlapping
                    open_to = std::max(open_to, ranges[i].last);
                    continue;
                }
                issue(base, open_from, open_to, r);
                open_from = ranges[i].first;
                open_to = ranges[i].last;
            }
            issue(base, open_from, open_to, r);
        }

        void issue(const std::byte* base, std::size_t from, std::size_t to, result& r) {
            // posix_madvise rather than madvise: same effect, and the spelling
            // that exists outside Linux.
            ::posix_madvise((void*)(base + from), to - from, POSIX_MADV_WILLNEED);
            ++r.advises;
            r.advised_bytes += to - from;
        }

        struct range { std::size_t first, last; };

        std::size_t lookahead, page_size;
        std::vector<range> ranges;
    };

} // namespace storage
// ---- end storage/prefetch_gather.hpp

// ---- begin storage/blob.hpp --------------------------------------------
// blob.hpp - text and binary of a length you do not know in advance.
//
// A record here is a POD aggregate with no padding, so a string inside one can
// only be `char name[40]`: too small for some rows, wasted on most, and a hard
// ceiling either way. That is a real limitation of the TYPED api, and it is
// easy to mistake for a limitation of the engine.
//
// It is not. The engine stores a length per record - field_slot::len, 16 bits -
// and create_raw() takes arbitrary bytes, so records of different sizes already
// coexist under one tag. Verified before this file was written: 0, 1, 5, 300
// and 4000 bytes round-trip, and patch() works on them by offset.
//
// So what was missing was never a capability. It was a convention and a type:
// the text lives as its OWN record, and the record that owns it holds a
// reference instead of an array.
//
//     struct article {
//         static constexpr storage::tag_t tag = 1;
//         storage::blob_ref<2> title;    // tag 2: where the text lives
//         storage::blob_ref<2> body;
//         uint64_t published_ms;
//     };
//
//     article a{};
//     a.title = storage::put_text<2>(table, "A very long title, no ceiling");
//     const auto id = table.create(a);
//     ...
//     std::string title = storage::load_text(table, table.get<article>(id)->title);
//
// WHAT THIS COSTS, stated rather than buried:
//
//   A second record per string. Each carries a 24-byte header, so short strings
//   are worse off than a char[40] would have been - under about 30 bytes, put
//   it in the struct. The win starts where the ceiling used to bite.
//
//   A second lookup to read one. The reference resolves through the slot array,
//   which is one indexed load, and then the bytes are assembled like any
//   record. Reading a row and its text is two gathers, not one.
//
//   No mutation in place. Text is replaced by writing a new blob and pointing
//   the owner at it, which leaves the old one for compaction to reclaim. That
//   is the same trade the whole engine makes and it is not a special case here.
//
// WHAT IS DELIBERATELY ABSENT. Interning - sharing one record between equal
// strings - is not here, because it needs a hash function chosen for the data
// and a decision about collisions, and both are the application's. It is four
// lines over a unique_index on a hash field you already control.


#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace storage {

    // A reference to a variable-length record stored under `tag_v`.
    //
    // Separate from ref<T> because ref<T> names a TYPE and a blob has none -
    // its payload is the bytes, so there is nothing to get<>() into. The tag is
    // what distinguishes one kind of blob from another, and putting it in the
    // type means a title reference cannot be handed to something expecting a
    // body reference.
    //
    // Eight bytes, trivially copyable, no padding: it satisfies the same
    // aggregate rules a record field must, which is the point of it being a
    // class rather than a typedef.
    template <tag_t tag_v>
    class blob_ref {
    public:
        static constexpr tag_t tag = tag_v;

        constexpr blob_ref() noexcept = default;
        constexpr explicit blob_ref(record_id value) noexcept : id_(value) {}

        constexpr record_id id() const noexcept { return id_; }
        constexpr explicit operator bool() const noexcept { return id_ != 0; }

        friend constexpr bool operator==(blob_ref, blob_ref) noexcept = default;
        friend constexpr auto operator<=>(blob_ref, blob_ref) noexcept = default;

    private:
        record_id id_ = 0;
    };

    // Writes `bytes` as a record of its own and returns a reference to it.
    // A default-constructed reference comes back when the payload is longer
    // than the spec allows, rather than a truncated record that would read as
    // valid - max_record_bytes is a compile-time bound on this engine and
    // silently honouring half of a string is worse than refusing it.
    template <tag_t tag_v>
    [[nodiscard]] blob_ref<tag_v> put_blob(auto& table, std::span<const std::byte> bytes) {
        using spec = typename std::remove_reference_t<decltype(table)>::spec_type;
        if (bytes.size() > spec::max_record_bytes) return blob_ref<tag_v>{};
        const record_id id = table.reserve_id();
        table.create_raw(id, tag_v, bytes);
        return blob_ref<tag_v>(id);
    }

    template <tag_t tag_v>
    [[nodiscard]] blob_ref<tag_v> put_text(auto& table, std::string_view text) {
        return put_blob<tag_v>(table, std::span<const std::byte>{ (const std::byte*)text.data(), text.size() });
    }

    // How many bytes are behind a reference, without reading them - useful for
    // sizing a buffer, and for deciding whether to read at all.
    template <tag_t tag_v>
    [[nodiscard]] std::size_t blob_size(const auto& table, blob_ref<tag_v> r) noexcept {
        return r ? table.length_of(r.id()) : 0;
    }

    // Into the caller's buffer, for a reader that has one already. Returns the
    // bytes written, 0 if the reference is empty or the buffer is too small -
    // a partial copy is never reported as success.
    template <tag_t tag_v>
    [[nodiscard]] std::size_t load_blob_into(const auto& table, blob_ref<tag_v> r, std::span<std::byte> out) {
        const std::size_t len = blob_size(table, r);
        if (!len || len > out.size()) return 0;
        return table.read_raw(r.id(), out.subspan(0, len)) ? len : 0;
    }

    template <tag_t tag_v>
    [[nodiscard]] std::string load_text(const auto& table, blob_ref<tag_v> r) {
        std::string out(blob_size(table, r), '\0');
        if (out.empty()) return out;
        if (!table.read_raw(r.id(), std::span<std::byte>{ (std::byte*)out.data(), out.size() })) out.clear();
        return out;
    }

    // Replaces the text behind a field: writes a new blob and hands back the
    // reference to store. The old record stays until compaction, exactly as an
    // overwritten field's old bytes do - this is an append-only log, and a blob
    // is not an exception to that.
    template <tag_t tag_v>
    [[nodiscard]] blob_ref<tag_v> replace_text(auto& table, blob_ref<tag_v> previous, std::string_view text) {
        (void)previous;   // named to make the caller's intent readable at the call site
        return put_text<tag_v>(table, text);
    }

    template <typename T> struct is_blob_ref : std::false_type {};
    template <tag_t tag_v> struct is_blob_ref<blob_ref<tag_v>> : std::true_type {};
    template <typename T> inline constexpr bool is_blob_ref_v = is_blob_ref<T>::value;

} // namespace storage
// ---- end storage/blob.hpp

// ---- begin storage/indexes.hpp -----------------------------------------
// indexes.hpp - secondary indexes you declare instead of write.
//
// The engine has no secondary indexes and will not grow any: it does not know
// what a "field" means to your schema, and an index that lived inside it would
// have to. What it does have is the observer, which sees every record on its
// way in - a live write, a replayed one, a replicated one - and that is the one
// place an index can be maintained without ever drifting from the data
// (FIELD_STORAGE.md §7).
//
// Writing that observer by hand is what every application ends up doing, and it
// is the same ~200 lines every time: a switch over tags, a retract before every
// add, and a resolve step for UPDATE because an update carries only the changed
// field. bench/app_layer.hpp is one worked instance. This file is that code,
// generated from a declaration:
//
//     using indexes = storage::index_set<
//         storage::unique_index<&customer::email_hash>,
//         storage::multi_index<&order::customer_id>,
//         storage::ordered_index<&product::price>>;
//
//     storage::field_storage<spec, indexes> table;
//
//     auto id = table.observer_state().find<&customer::email_hash>(h);
//     table.observer_state().each<&order::customer_id>(cid, [](record_id line) { ... });
//     auto n = table.observer_state().count_range<&product::price>(100, 500);
//
// THE THREE RULES IT ENFORCES FOR YOU, each of which is a bug someone has
// already shipped:
//
//   Retract before add. An update to an indexed field must remove the old entry
//   or the index accumulates ghosts. The observer runs BEFORE the slot changes,
//   which is what makes the old value still readable - that ordering is the
//   whole reason this works, and it is easy to lose by hand.
//
//   Resolve the new value. An UPDATE record carries only the bytes that
//   changed, so the post-update record is the current one with those bytes
//   applied on top. Indexing the payload directly indexes a fragment.
//
//   Type comes from the record, not the header. UPDATE and DESTROY carry no
//   tag - only an id and a byte range - so the type has to come from
//   tag_of(id). A switch on hdr.tag silently does nothing for updates.
//
// WHERE THE MEMORY LIVES. On the heap, in flat arrays this file owns. Flat
// rather than std::unordered_map because the structures are meant to become
// mappable later - a map is a web of pointers into an allocator and means
// nothing to the next run - and because capacity should be a number you chose
// rather than whatever the allocator does under memory pressure. An insert past
// capacity FAILS rather than growing, the same rule the engine's own index
// follows, and failed_inserts() counts it so a silent half-index is impossible.
//
// WHAT IT DOES NOT DO. Uniqueness is not enforced here - a unique_index is a
// lookup structure, and rejecting a duplicate is a constraint, which belongs
// above apply() in the transaction layer (FIELD_STORAGE.md §7 again: index
// maintenance below, constraints above, and getting that backwards produces a
// database that is correct until the first crash).


#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <vector>

namespace storage {

    // =====================================================================
    // The observer contract, spelled out. It was duck-typed - a type without a
    // matching on_record failed somewhere inside apply_create with a message
    // about none of this. Now it is a concept, and a wrong observer says so.
    // =====================================================================
    template <typename observer, typename table>
    concept observes = requires(observer& o, table& t, const record_header& h, const std::byte* p) {
        o.on_record(t, h, p);
    };

    namespace detail {

        // The byte offset of a data member, without dereferencing a null
        // object: the same trick update<member> uses, and the reason a
        // pointer-to-member is enough to name a field at compile time.
        template <auto member>
        consteval std::size_t offset_of() {
            using owner = member_class_t<member>;
            return (std::size_t)&(((owner const volatile*)nullptr)->*member);
        }

        // The record as it will be AFTER this operation. A CREATE carries the
        // whole thing; an UPDATE carries only the changed bytes, so the answer
        // is the current record with those laid on top. Called while the
        // observer still sees the pre-change state, which is what makes the
        // second case possible at all.
        template <typename T>
        T resolve(auto& table, const record_header& hdr, const std::byte* payload) {
            T value{};
            if (hdr.op == record_op::create) {
                std::memcpy(&value, payload, sizeof(T));
            } else {
                if (auto current = table.template get<T>(hdr.id)) value = *current;
                if (hdr.offset + hdr.len <= sizeof(T))
                    std::memcpy((std::byte*)&value + hdr.offset, payload, hdr.len);
            }
            return value;
        }

        // Index keys are integers, enums, or a ref - which is an id, so an
        // integer wearing a type. Anything else would mean choosing a hash
        // function on the application's behalf; hash the field yourself and
        // index the hash.
        template <typename K>
        concept index_key = std::is_integral_v<K> || std::is_enum_v<K> || is_ref_v<K>;

        // One spelling for "the number under this key", so the index kinds do
        // not each have to know that a ref is an id.
        template <typename K>
        constexpr uint64_t key_bits(K key) noexcept {
            if constexpr (is_ref_v<K>) return key.id();
            else return (uint64_t)key;
        }

        inline constexpr std::size_t round_pow2(std::size_t n) noexcept {
            std::size_t p = 1;
            while (p < n) p <<= 1;
            return p;
        }

    } // namespace detail

    // =====================================================================
    // Open-addressed hash map with linear probing, over an array this class
    // owns. Key 0 means "empty", which costs nothing for the keys an index
    // actually holds - a record id is issued from 1, and a hash colliding with
    // exactly zero is a case worth losing to keep the probe branch-free.
    //
    // Deletion shifts backward rather than leaving a tombstone. Tombstones
    // accumulate over a long-lived index and turn every lookup into a scan,
    // and an index that survives restarts is long-lived by construction.
    // =====================================================================
    class flat_hash_map {
    public:
        void reserve(std::size_t entries) {
            const std::size_t capacity = detail::round_pow2(entries < 8 ? 8 : entries * 2);
            slots.assign(capacity, slot{});
            mask = capacity - 1;
            used = 0;
        }

        bool insert(uint64_t key, uint64_t value) noexcept {
            if (used * 4 >= slots.size() * 3) return false;   // 75% and no growth
            std::size_t i = key & mask;
            for (; slots[i].key; i = (i + 1) & mask)
                if (slots[i].key == key) { slots[i].value = value; return true; }
            slots[i] = { key, value };
            ++used;
            return true;
        }

        const uint64_t* find(uint64_t key) const noexcept {
            for (std::size_t i = key & mask; slots[i].key; i = (i + 1) & mask)
                if (slots[i].key == key) return &slots[i].value;
            return nullptr;
        }

        void erase(uint64_t key) noexcept {
            std::size_t i = key & mask;
            for (; slots[i].key; i = (i + 1) & mask)
                if (slots[i].key == key) break;
            if (!slots[i].key) return;

            slots[i] = slot{};
            --used;
            // Backward shift: everything after the hole that probed past its
            // own home slot has to move up, or it becomes unreachable.
            for (std::size_t j = (i + 1) & mask; slots[j].key; j = (j + 1) & mask) {
                const std::size_t home = slots[j].key & mask;
                const bool wrapped = j < i;
                const bool home_between = wrapped ? (home <= i || home > j) : (home <= i && home > j);
                if (!home_between) continue;
                slots[i] = slots[j];
                slots[j] = slot{};
                i = j;
            }
        }

        std::size_t size() const noexcept { return used; }
        std::size_t capacity() const noexcept { return slots.size(); }

    private:
        struct slot { uint64_t key = 0; uint64_t value = 0; };
        std::vector<slot> slots;
        std::size_t mask = 0;
        std::size_t used = 0;
    };

    // =====================================================================
    // key -> many values. A hash map from key to a list head, plus one node
    // arena with a free list shared by every key.
    //
    // Not one vector per key: that is a heap allocation per distinct key, and
    // neither the map nor the vectors could ever be mapped. Not an array
    // indexed by key either - that only works when keys are dense small
    // integers, which is true for record ids and false for everything else,
    // and failing quietly on the second case is the kind of trap this file
    // exists to remove.
    // =====================================================================
    class flat_multi_map {
    public:
        void reserve(std::size_t keys, std::size_t total_entries) {
            heads.reserve(keys);
            nodes.assign(total_entries + 1, node{});   // index 0 is the end sentinel
            high_water = 1;
            free_head = 0;
        }

        bool insert(uint64_t key, uint64_t value) noexcept {
            const uint32_t n = alloc();
            if (!n) return false;
            const uint64_t* head = heads.find(key);
            nodes[n].value = value;
            nodes[n].next = head ? (uint32_t)*head : 0;
            return heads.insert(key, n);
        }

        bool erase(uint64_t key, uint64_t value) noexcept {
            const uint64_t* head = heads.find(key);
            if (!head) return false;
            uint32_t current = (uint32_t)*head, previous = 0;
            while (current) {
                if (nodes[current].value == value) {
                    const uint32_t next = nodes[current].next;
                    if (previous) nodes[previous].next = next;
                    else if (next) heads.insert(key, next);
                    else heads.erase(key);
                    nodes[current].next = free_head;
                    free_head = current;
                    return true;
                }
                previous = current;
                current = nodes[current].next;
            }
            return false;
        }

        void each(uint64_t key, auto&& fn) const {
            const uint64_t* head = heads.find(key);
            if (!head) return;
            for (uint32_t n = (uint32_t)*head; n; n = nodes[n].next) fn(nodes[n].value);
        }

        std::size_t count(uint64_t key) const noexcept {
            std::size_t n = 0;
            each(key, [&](uint64_t) { ++n; });
            return n;
        }

        bool empty(uint64_t key) const noexcept { return heads.find(key) == nullptr; }
        std::size_t distinct_keys() const noexcept { return heads.size(); }

    private:
        struct node { uint64_t value = 0; uint32_t next = 0; uint32_t reserved = 0; };

        uint32_t alloc() noexcept {
            if (free_head) {
                const uint32_t n = free_head;
                free_head = nodes[n].next;
                return n;
            }
            if (high_water >= nodes.size()) return 0;
            return (uint32_t)high_water++;
        }

        flat_hash_map heads;
        std::vector<node> nodes;
        std::size_t high_water = 1;
        uint32_t free_head = 0;
    };

    // =====================================================================
    // (key, id) pairs kept sorted, so a range is two binary searches and a
    // subtraction.
    //
    // A sorted array rather than a tree, and the cost is stated rather than
    // hidden: an insert into the middle moves the tail. A B-tree would make
    // that O(log n) at the price of a great deal of code that has to be right,
    // and the trade only pays when writes are frequent - an ordered index over
    // something like a price is queried far more often than it is written.
    // =====================================================================
    class flat_ordered_map {
    public:
        void reserve(std::size_t entries) { pairs.reserve(entries); limit = entries; }

        bool insert(int64_t key, uint64_t id) {
            if (pairs.size() >= limit) return false;
            pairs.insert(pairs.begin() + (std::ptrdiff_t)lower_bound(key), entry{ key, id });
            return true;
        }

        bool erase(int64_t key, uint64_t id) {
            for (std::size_t i = lower_bound(key); i < pairs.size() && pairs[i].key == key; ++i) {
                if (pairs[i].id != id) continue;
                pairs.erase(pairs.begin() + (std::ptrdiff_t)i);
                return true;
            }
            return false;
        }

        std::size_t count_range(int64_t lo, int64_t hi) const noexcept {
            return upper_bound(hi) - lower_bound(lo);
        }

        void each_range(int64_t lo, int64_t hi, auto&& fn) const {
            for (std::size_t i = lower_bound(lo), end = upper_bound(hi); i < end; ++i)
                fn(pairs[i].id, pairs[i].key);
        }

        std::size_t size() const noexcept { return pairs.size(); }

    private:
        struct entry { int64_t key; uint64_t id; };

        std::size_t lower_bound(int64_t key) const noexcept {
            std::size_t lo = 0, hi = pairs.size();
            while (lo < hi) { const auto mid = lo + (hi - lo) / 2; if (pairs[mid].key < key) lo = mid + 1; else hi = mid; }
            return lo;
        }
        std::size_t upper_bound(int64_t key) const noexcept {
            std::size_t lo = 0, hi = pairs.size();
            while (lo < hi) { const auto mid = lo + (hi - lo) / 2; if (pairs[mid].key <= key) lo = mid + 1; else hi = mid; }
            return lo;
        }

        std::vector<entry> pairs;
        std::size_t limit = 0;
    };

    // =====================================================================
    // The three index kinds. Each knows one field of one record type, and
    // nothing about the others - index_set does the dispatch.
    //
    // They share a shape rather than a base class: add(), remove(), and a tag
    // saying which record type they apply to. No virtuals, because index_set
    // knows every type at compile time and a dispatch per record would be paid
    // on the write path.
    // =====================================================================
    template <auto member>
    class unique_index {
    public:
        using owner = member_class_t<member>;
        using key_type = member_value_t<member>;
        static_assert(detail::index_key<key_type>,
                      "an index key must be an integer or an enum - hash the field yourself and index the hash");
        static constexpr auto field = member;
        static constexpr tag_t owner_tag = owner::tag;

        void reserve(std::size_t records) { map.reserve(records); }
        bool add(record_id id, const owner& value) { return map.insert(detail::key_bits(value.*member), id); }
        void remove(record_id, const owner& value) { map.erase(detail::key_bits(value.*member)); }

        // 0 when absent, since an id is issued from 1.
        record_id find(key_type key) const noexcept {
            const uint64_t* found = map.find(detail::key_bits(key));
            return found ? (record_id)*found : 0;
        }
        bool contains(key_type key) const noexcept { return map.find(detail::key_bits(key)) != nullptr; }
        std::size_t size() const noexcept { return map.size(); }

    private:
        flat_hash_map map;
    };

    template <auto member>
    class multi_index {
    public:
        using owner = member_class_t<member>;
        using key_type = member_value_t<member>;
        static_assert(detail::index_key<key_type>,
                      "an index key must be an integer or an enum - hash the field yourself and index the hash");
        static constexpr auto field = member;
        static constexpr tag_t owner_tag = owner::tag;

        void reserve(std::size_t records) { map.reserve(records, records); }
        bool add(record_id id, const owner& value) { return map.insert(detail::key_bits(value.*member), id); }
        void remove(record_id id, const owner& value) { map.erase(detail::key_bits(value.*member), id); }

        void each(key_type key, auto&& fn) const { map.each(detail::key_bits(key), [&](uint64_t id) { fn((record_id)id); }); }
        std::size_t count(key_type key) const noexcept { return map.count(detail::key_bits(key)); }
        bool empty(key_type key) const noexcept { return map.empty(detail::key_bits(key)); }

    private:
        flat_multi_map map;
    };

    template <auto member>
    class ordered_index {
    public:
        using owner = member_class_t<member>;
        using key_type = member_value_t<member>;
        static_assert(detail::index_key<key_type>,
                      "an ordered index key must be an integer or an enum");
        static constexpr auto field = member;
        static constexpr tag_t owner_tag = owner::tag;

        void reserve(std::size_t records) { map.reserve(records); }
        bool add(record_id id, const owner& value) { return map.insert((int64_t)(value.*member), id); }
        void remove(record_id id, const owner& value) { map.erase((int64_t)(value.*member), id); }

        std::size_t count_range(key_type lo, key_type hi) const noexcept {
            return map.count_range((int64_t)lo, (int64_t)hi);
        }
        void each_range(key_type lo, key_type hi, auto&& fn) const {
            map.each_range((int64_t)lo, (int64_t)hi, [&](uint64_t id, int64_t k) { fn((record_id)id, (key_type)k); });
        }
        std::size_t size() const noexcept { return map.size(); }

    private:
        flat_ordered_map map;
    };

    // =====================================================================
    // Every declared index, maintained by one observer.
    //
    // This is the part that is the same in every application and the part that
    // is easy to get wrong: retract before add, resolve the post-update value,
    // and take the type from the record rather than from the header.
    // =====================================================================
    template <typename... index_types>
    class index_set {
    public:
        // Capacity is fixed here rather than grown later, deliberately: an
        // index that reallocates under a write is an index whose cost is
        // unpredictable exactly when it matters, and the engine's own index
        // makes the same trade. Sized in records, since that is the number an
        // application actually knows.
        explicit index_set(std::size_t expected_records = 1024) {
            std::apply([&](auto&... ix) { (ix.reserve(expected_records), ...); }, indexes);
        }

        void on_record(auto& table, const record_header& hdr, const std::byte* payload) {
            // The type of an UPDATE or a DESTROY is not in the header - those
            // carry an id and a byte range and nothing else - so it comes from
            // the record already in place. A switch on hdr.tag would silently
            // skip every update.
            const tag_t existing = table.tag_of(hdr.id);

            // Retract first, while the old record is still readable. The
            // observer runs BEFORE the slot changes, which is the only reason
            // this is possible, and losing that ordering leaves ghosts.
            if (existing) retract(table, hdr.id, existing);
            if (hdr.op == record_op::destroy) return;

            const tag_t what = (hdr.op == record_op::create) ? hdr.tag : existing;
            if (!what) return;
            publish(table, hdr, payload, what);
        }

        // Puts an already-stored record into the indexes, for rebuilding from
        // live records rather than by replaying the log - what a restart from
        // a checkpoint needs, since the checkpoint skipped the replay that
        // would otherwise have called on_record for everything.
        void index_existing(record_id id, tag_t what, const std::byte* bytes) {
            std::apply([&](auto&... ix) { (add_if_matching(ix, id, what, bytes), ...); }, indexes);
        }

        // ---- queries, addressed by the field they index ----

        template <auto member> auto& of() noexcept {
            constexpr std::size_t i = slot_for<member>();
            static_assert(i < sizeof...(index_types),
                          "no declared index covers that field - add one to the index_set");
            return std::get<i>(indexes);
        }
        template <auto member> const auto& of() const noexcept {
            constexpr std::size_t i = slot_for<member>();
            static_assert(i < sizeof...(index_types),
                          "no declared index covers that field - add one to the index_set");
            return std::get<i>(indexes);
        }

        template <auto member> record_id find(auto key) const { return of<member>().find(key); }
        template <auto member> void each(auto key, auto&& fn) const { of<member>().each(key, fn); }
        template <auto member> std::size_t count(auto key) const { return of<member>().count(key); }
        template <auto member> std::size_t count_range(auto lo, auto hi) const { return of<member>().count_range(lo, hi); }
        template <auto member> void each_range(auto lo, auto hi, auto&& fn) const { of<member>().each_range(lo, hi, fn); }

        // Every insert that did not fit. Nonzero means an index is now a
        // partial answer, which is worse than an empty one because it looks
        // like a result - so it is counted rather than logged, and a caller
        // that cares can assert on it.
        std::size_t failed_inserts() const noexcept { return failures; }

    private:
        // Does this index cover that field? Two pointers-to-member of
        // different classes cannot be compared at all - ill-formed, not merely
        // false - so the types are checked first and the comparison is only
        // instantiated where it means something.
        //
        // remove_cv on both sides is load-bearing: decltype of a `static
        // constexpr auto field` yields the type WITH top-level const, while
        // decltype of the NTTP does not, so the two never compared equal and
        // every lookup fell off the end of the pack.
        template <auto member, typename ix>
        static consteval bool covers() {
            if constexpr (std::is_same_v<std::remove_cv_t<decltype(ix::field)>,
                                         std::remove_cv_t<decltype(member)>>)
                return ix::field == member;
            else
                return false;
        }

        // A fold rather than a recursion. The recursive form instantiated
        // tuple_element_t at the end of the pack before any guard could stop
        // it, so the out-of-range case failed inside <tuple> instead of saying
        // which field had no index.
        template <auto member>
        static consteval std::size_t slot_for() {
            constexpr bool hits[] = { covers<member, index_types>()..., false };
            for (std::size_t i = 0; i < sizeof...(index_types); ++i)
                if (hits[i]) return i;
            return sizeof...(index_types);   // of<> turns this into a readable error
        }

        void retract(auto& table, record_id id, tag_t what) {
            std::apply([&](auto&... ix) { (remove_if_matching(table, ix, id, what), ...); }, indexes);
        }

        void publish(auto& table, const record_header& hdr, const std::byte* payload, tag_t what) {
            std::apply([&](auto&... ix) { (add_resolved_if_matching(table, ix, hdr, payload, what), ...); }, indexes);
        }

        void remove_if_matching(auto& table, auto& ix, record_id id, tag_t what) {
            using owner = typename std::remove_reference_t<decltype(ix)>::owner;
            if (what != ix.owner_tag) return;
            if (auto current = table.template get<owner>(id)) ix.remove(id, *current);
        }

        void add_resolved_if_matching(auto& table, auto& ix, const record_header& hdr,
                                      const std::byte* payload, tag_t what) {
            using owner = typename std::remove_reference_t<decltype(ix)>::owner;
            if (what != ix.owner_tag) return;
            if (!ix.add(hdr.id, detail::resolve<owner>(table, hdr, payload))) ++failures;
        }

        void add_if_matching(auto& ix, record_id id, tag_t what, const std::byte* bytes) {
            using owner = typename std::remove_reference_t<decltype(ix)>::owner;
            if (what != ix.owner_tag) return;
            owner value{};
            std::memcpy(&value, bytes, sizeof(value));
            if (!ix.add(id, value)) ++failures;
        }

        std::tuple<index_types...> indexes;
        std::size_t failures = 0;
    };

} // namespace storage
// ---- end storage/indexes.hpp
