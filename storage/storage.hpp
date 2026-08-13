#pragma once
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
#if defined(__x86_64__) || defined(__i386__)
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

#if defined(__x86_64__) || defined(__i386__)
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
