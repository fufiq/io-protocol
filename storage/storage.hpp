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
//                              arena (one vector<byte> plus a map<id,
//                              arena_ref{tag,offset,len}>) - no per-record
//                              allocation, and no dependency on a fixed T.
//
// Update model: a WAL diff (offset + len + payload, checksum over header
// and payload) plus an in-place copy for fast get() without replay.
// Recovery always rebuilds state from the log - the in-place bytes are
// never trusted for that - so the WAL stays strictly append-only (only the
// tail can tear), and "stop at the first checksum mismatch" remains sound
// with no caveats. Changing a value's shape or size (a different T on the
// same id) goes through recreate(), a distinct operation from update().
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
//                                index[id].data points at and what update()
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
// caller-provided region, index[id].data aliases the log payload directly,
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
#include <nmmintrin.h> // _mm_crc32_u8/_mm_crc32_u64 - hardware CRC32C (SSE4.2)
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

    // memcpy for the 8-byte reads, not a direct (const uint64_t*) cast: every
    // existing call site happens to pass a pointer derived from base + a
    // pad8()-aligned offset, so the cast was never actually observed to
    // fault - but it's still UB for an unaligned pointer per the C++ object
    // model (confirmed by UBSan: apply_wal_range(), added for replication,
    // is the first caller that can legitimately pass an arbitrary,
    // non-8-byte-aligned buffer - a network-received payload has no
    // alignment guarantee at all). memcpy compiles down to the identical
    // single unaligned-load instruction on x86 here, so this costs nothing.
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
              fixed_string data_path_v = fixed_string("")>
    struct storage_spec {
        static constexpr durability level = level_v;
        static constexpr auto path = path_v;
        static constexpr std::size_t capacity = capacity_v;

        // Held as a member so the array outlives every use of data_path;
        // an explicit data_path_v wins over the derived name.
        static constexpr auto derived_data_path = with_data_suffix<path_v>();
        static constexpr const char* data_path = data_path_v.value[0] ? data_path_v.value : derived_data_path.data();
    };

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
        uint8_t   pad = 0;        // 1, offset 17
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
    // storage<spec> - the single storage. No tuple<storage<T>...>, no
    // compile-time type list, no type_id dispatch table. For local_sync it
    // opens and maps its own file (as region<spec> used to); for
    // shared/replicated, base is supplied by the caller - the storage neither
    // opens nor owns it.
    // =====================================================================
    template <typename spec = storage_spec<>>
    class storage {
    public:
        struct index_entry { std::byte* data = nullptr; };

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
            } else {
                base = shared_base;
                data_base = shared_base;
            }
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
            std::byte* logged = write_record<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            reserve(id).data = materialize(id, T::tag, logged, (uint32_t)sizeof(T));
            live_byte_count += sizeof(T);
            return id;
        }

        // Replaces a value's shape or size (variable length, a different T
        // on the same id) with a fresh CREATE over the existing id. The old
        // bytes become garbage, reclaimed on the next compaction.
        template <tagged_aggregate T>
        record_id recreate(record_id id, const T& value) {
            if (alive(id)) live_byte_count -= header_of(index[id].data)->len;
            std::byte* logged = write_record<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            reserve(id).data = materialize(id, T::tag, logged, (uint32_t)sizeof(T));
            live_byte_count += sizeof(T);
            return id;
        }

        template <auto member>
        requires field<member_value_t<member>>
        void update(record_id id, member_value_t<member> value) {
            if (!alive(id)) return;
            const std::size_t off = member_offset<member>();
            write_record<record_op::update>(id, (uint32_t)(off), tag_t{0}, std::as_bytes(std::span{&value, 1}));
            // Direct cast-and-assign instead of memcpy - value is already a
            // `field` (trivially copyable, standard layout); a C-style cast
            // on the computed offset, the same idiom used throughout this
            // file (see header_of()), applied here to a typed field.
            *(member_value_t<member>*)(index[id].data + off) = value;
        }

        void destroy(record_id id) {
            if (!alive(id)) return;
            write_record<record_op::destroy>(id, 0, tag_t{0}, {});
            live_byte_count -= header_of(index[id].data)->len;
            index[id].data = nullptr;
        }

        template <tagged_aggregate T>
        [[nodiscard]] const T* get(record_id id) const noexcept {
            return alive(id) ? (const T*)(index[id].data) : nullptr;
        }

        void for_each_raw(auto&& fn) const requires std::invocable<decltype(fn), raw_view> {
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].data) continue;
                const record_header* h = header_of(index[id].data);
                fn(raw_view{ id, h->tag, std::span<const std::byte>{index[id].data, h->len} });
            }
        }

        template <tagged_aggregate T>
        void for_each(auto&& fn) const requires std::invocable<decltype(fn), record_id, const T&> {
            for (record_id id = 1; id < index.size(); ++id) {
                if (index[id].data) fn(id, *(const T*)(index[id].data));
            }
        }

        // Applies a single WAL record during replay. Never interprets the
        // payload (tag is ignored) - it only moves bytes by offset/len, so
        // one method covers every type at once, generically.
        void apply(const record_header& hdr, const std::byte* data) {
            switch (hdr.op) {
                case record_op::create:
                    if (alive(hdr.id)) live_byte_count -= header_of(index[hdr.id].data)->len;
                    reserve(hdr.id).data = materialize(hdr.id, hdr.tag, data, hdr.len);
                    live_byte_count += hdr.len;
                    break;
                case record_op::update:
                    if (alive(hdr.id)) std::memcpy(index[hdr.id].data + hdr.offset, data, hdr.len);
                    break;
                case record_op::destroy:
                    if (alive(hdr.id)) {
                        live_byte_count -= header_of(index[hdr.id].data)->len;
                        index[hdr.id].data = nullptr;
                    }
                    break;
            }
        }

        // Strictly sequential scan, stops at the first checksum mismatch -
        // nothing past that point is read or applied. For local_sync,
        // file_size is usually obtained via ::stat() on the compile-time
        // known spec::path.
        // file_size is the LOG's size. The data file is not inspected at all
        // - whatever a crash left in it is stale by definition, and this
        // scan rewrites it from offset 0 as each CREATE is replayed.
        std::size_t recover(std::size_t file_size) {
            std::size_t pos = 0;
            data_offset = 0;
            while (pos + sizeof(record_header) <= file_size) {
                record_header hdr;
                std::memcpy(&hdr, base + pos, sizeof(hdr));
                if (hdr.id == 0) break; // sparse file tail, not a real record
                if (pos + sizeof(hdr) + hdr.len > file_size) break; // truncated tail

                uint32_t expected = (hdr.op == record_op::create)
                    ? checksum_of(hdr, nullptr, 0)
                    : checksum_of(hdr, base + pos + sizeof(hdr), hdr.len);
                if (expected != hdr.checksum) break;

                apply(hdr, base + pos + sizeof(hdr));
                pos += pad8(sizeof(hdr) + hdr.len);
            }
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
        // The replica's file ends up a byte-for-byte mirror of the
        // primary's WAL, sharing its exact offset numbering - that's
        // deliberate: it's what makes "resume from offset X after a
        // reconnect" trivially meaningful on both sides, rather than
        // needing a translated/independent numbering scheme.
        //
        // Returns how many bytes were actually verified and applied - less
        // than bytes.size() if a checksum failed or the range was
        // truncated partway through a record, mirroring recover()'s own
        // "stop at the first bad record" behavior. The caller should treat
        // anything past the returned count as not yet applied and not
        // advance its own bookkeeping past it.
        std::size_t apply_wal_range(const std::byte* bytes, std::size_t len) {
            std::memcpy(base + write_offset, bytes, len);

            // Verified against base + write_offset (the just-copied,
            // guaranteed pad8()-aligned destination) from here on, not
            // against `bytes` directly - `bytes` is a caller-provided
            // buffer (a network payload, for replication's actual use)
            // with no alignment guarantee at all, unlike every other
            // caller of checksum_of/crc32c in this file. memcpy already
            // made an identical copy at the aligned destination, so
            // there's no reason to read the unaligned source a second time
            // for this.
            std::size_t pos = 0;
            while (pos + sizeof(record_header) <= len) {
                record_header hdr;
                std::memcpy(&hdr, base + write_offset + pos, sizeof(hdr));
                if (hdr.id == 0) break;
                if (pos + sizeof(hdr) + hdr.len > len) break; // truncated tail

                uint32_t expected = (hdr.op == record_op::create)
                    ? checksum_of(hdr, nullptr, 0)
                    : checksum_of(hdr, base + write_offset + pos + sizeof(hdr), hdr.len);
                if (expected != hdr.checksum) break;

                // apply() reads its payload from THIS storage's own region
                // (base + write_offset + ...), not from `bytes` directly -
                // the memcpy above already placed it there, and index_
                // entries must point into this storage's own mmap'd
                // memory, not into the caller's temporary buffer.
                apply(hdr, base + write_offset + pos + sizeof(hdr));
                pos += pad8(sizeof(hdr) + hdr.len);
            }
            write_offset += pos;
            return pos;
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
                if (!index[id].data) continue;
                needed += pad8(sizeof(record_header) + header_of(index[id].data)->len);
            }
            std::vector<std::byte> scratch(needed);
            std::size_t scratch_offset = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].data) continue;
                const record_header* old_hdr = header_of(index[id].data);
                write_record_at<record_op::create>(scratch.data(), scratch_offset, id, 0, old_hdr->tag,
                                                    std::span<const std::byte>{index[id].data, old_hdr->len});
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
        void compact(std::byte* scratch_base) requires (!split_regions) {
            compact_into(scratch_base, nullptr);
        }

        // scratch_data_base must be a DIFFERENT region from the current data
        // region: records are read from the old one while being written to
        // the new one, and their relative order is not guaranteed to make an
        // in-place shuffle safe.
        void compact(std::byte* scratch_base, std::byte* scratch_data_base) requires (split_regions) {
            compact_into(scratch_base, scratch_data_base);
        }

        // --- Below: methods for background_compact (Variant 2) only, not
        // for general use. Public because background_compact is a free
        // function, not a friend - by convention, call these only through
        // storage::background_compact(), never directly.

        std::byte* raw_base() const noexcept { return base; }

        // Snapshot of currently-live records - an ARENA, not a malloc per
        // record. arena_ref owns nothing; it's just an (offset, len) into
        // ONE shared buffer - the same principle the storage itself uses (bump
        // write_offset, never free individually), except this buffer is
        // temporary and lives for exactly one background_compact() call.
        // offset, not a pointer: the arena can grow (push_back/insert) and
        // relocate in memory - pointers handed out before that would dangle
        // silently, while offsets from the buffer's start stay valid
        // regardless. offset is size_t, not uint32_t: it's the CUMULATIVE
        // position across every live record snapshotted, unlike
        // record_header::len (a single record's own length, genuinely
        // bounded by uint32_t) - capping it at 32 bits would silently
        // truncate on any storage snapshotting more than ~4GB of combined
        // live data.
        struct arena_ref { tag_t tag; std::size_t offset; uint32_t len; };
        struct snapshot {
            std::vector<std::byte> arena;
            std::unordered_map<record_id, arena_ref> index;
        };

        void snapshot_into(snapshot& out) const {
            out.arena.reserve(out.arena.size() + live_byte_count); // live_byte_count is exact - one pass, no reallocation
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].data) continue;
                const record_header* h = header_of(index[id].data);
                std::size_t off = out.arena.size();
                out.arena.insert(out.arena.end(), index[id].data, index[id].data + h->len);
                out.index[id] = arena_ref{ h->tag, off, h->len };
            }
        }

        // Replays the WAL range [from, to) on top of an EXTERNAL snapshot -
        // never touches this storage's index, so it's safe to call WITHOUT
        // the lock (the WAL is immutable in this range: bytes before `to`
        // are never mutated again, only appended past it).
        std::size_t replay_into(std::byte* wal_base, std::size_t from, std::size_t to, snapshot& snap) const {
            std::size_t pos = from;
            while (pos + sizeof(record_header) <= to) {
                record_header hdr;
                std::memcpy(&hdr, wal_base + pos, sizeof(hdr));
                if (hdr.id == 0) break;
                if (pos + sizeof(hdr) + hdr.len > to) break;
                uint32_t expected = (hdr.op == record_op::create)
                    ? checksum_of(hdr, nullptr, 0)
                    : checksum_of(hdr, wal_base + pos + sizeof(hdr), hdr.len);
                if (expected != hdr.checksum) break;

                switch (hdr.op) {
                    case record_op::create: {
                        // recreate() of the same id: the old range in the
                        // arena simply becomes dead weight, not reclaimed
                        // (the whole arena is discarded after
                        // finish_compaction - not worth the complexity of
                        // reusing fragments).
                        std::size_t off = snap.arena.size();
                        snap.arena.insert(snap.arena.end(), wal_base + pos + sizeof(hdr), wal_base + pos + sizeof(hdr) + hdr.len);
                        snap.index[hdr.id] = arena_ref{ hdr.tag, off, hdr.len };
                        break;
                    }
                    case record_op::update: {
                        auto it = snap.index.find(hdr.id);
                        if (it != snap.index.end() && (std::size_t)(hdr.offset) + hdr.len <= it->second.len)
                            std::memcpy(snap.arena.data() + it->second.offset + hdr.offset, wal_base + pos + sizeof(hdr), hdr.len);
                        break;
                    }
                    case record_op::destroy:
                        snap.index.erase(hdr.id);
                        break;
                }
                pos += pad8(sizeof(hdr) + hdr.len);
            }
            return pos;
        }

        // Final step: writes the entire snapshot into scratch as fresh
        // CREATEs, then switches index/base/write_offset/last_synced
        // to the new region. Call only while holding the lock (see
        // background_compact).
        void finish_compaction(std::byte* scratch, snapshot& snap) requires (!split_regions) {
            finish_compaction_into(scratch, nullptr, snap);
        }
        void finish_compaction(std::byte* scratch, std::byte* scratch_data, snapshot& snap) requires (split_regions) {
            finish_compaction_into(scratch, scratch_data, snap);
        }

    private:
        static record_header* header_of(std::byte* data) noexcept {
            return (record_header*)(data - sizeof(record_header));
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
        void compact_into(std::byte* scratch_base, std::byte* scratch_data_base) {
            std::size_t scratch_offset = 0, scratch_data_offset = 0;
            live_byte_count = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                if (!index[id].data) continue; // dead record - not carried over
                const record_header* old_hdr = header_of(index[id].data);
                std::span<const std::byte> payload{index[id].data, old_hdr->len};
                std::byte* logged = write_record_at<record_op::create>(scratch_base, scratch_offset, id, 0, old_hdr->tag, payload);
                if constexpr (split_regions)
                    index[id].data = write_record_at<record_op::create>(scratch_data_base, scratch_data_offset, id, 0, old_hdr->tag, payload);
                else
                    index[id].data = logged;
                live_byte_count += old_hdr->len;
            }
            base = scratch_base;
            write_offset = scratch_offset;
            // last_synced MUST be reset: scratch has never been synced, and
            // the old value (from the previous, larger region) could exceed
            // the new write_offset - sync() would then conclude there's
            // nothing to flush and silently skip the whole compacted region.
            last_synced = 0;
            if constexpr (split_regions) { data_base = scratch_data_base; data_offset = scratch_data_offset; }
        }

        void finish_compaction_into(std::byte* scratch, std::byte* scratch_data, snapshot& snap) {
            std::size_t scratch_offset = 0, scratch_data_offset = 0;
            live_byte_count = 0;
            for (record_id id = 1; id < index.size(); ++id) {
                auto it = snap.index.find(id);
                if (it == snap.index.end()) { index[id].data = nullptr; continue; } // died during catch-up
                const arena_ref& ref = it->second;
                std::span<const std::byte> payload{snap.arena.data() + ref.offset, ref.len};
                std::byte* logged = write_record_at<record_op::create>(scratch, scratch_offset, id, 0, ref.tag, payload);
                if constexpr (split_regions)
                    index[id].data = write_record_at<record_op::create>(scratch_data, scratch_data_offset, id, 0, ref.tag, payload);
                else
                    index[id].data = logged;
                live_byte_count += ref.len;
            }
            base = scratch;
            write_offset = scratch_offset;
            last_synced = 0;
            if constexpr (split_regions) { data_base = scratch_data; data_offset = scratch_data_offset; }
        }

        bool alive(record_id id) const noexcept {
            return id < index.size() && index[id].data != nullptr;
        }

        // Grows with the total number of ids ever issued (next_id only
        // ever increases), not with the current live-record count - a
        // destroyed record's index slot is never reclaimed, just left
        // null. 8 bytes per id-ever-created; fine for the monotonic
        // single-writer id space this storage assumes, worth knowing
        // before assuming index memory tracks occupancy rather than
        // lifetime churn.
        index_entry& reserve(record_id id) {
            if (id >= index.size()) index.resize(std::max<std::size_t>(id + 1, index.size() * 2 + 1));
            next_id = std::max(next_id, id + 1);
            return index[id];
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
            record_header hdr{ id, offset, (uint32_t)(bytes.size()), op, 0, tag };
            // CREATE - checksum covers only the header (the payload is
            // legally mutated later by update(), see the file header
            // comment). UPDATE/DELETE - checksum covers header and payload
            // together; those bytes are never mutated again.
            if constexpr (op == record_op::create) {
                hdr.checksum = checksum_of(hdr, nullptr, 0);
            } else {
                hdr.checksum = checksum_of(hdr, bytes.data(), (uint32_t)bytes.size());
            }
            std::byte* pos = target_base + target_offset;
            std::memcpy(pos, &hdr, sizeof(hdr));
            if (!bytes.empty()) std::memcpy(pos + sizeof(hdr), bytes.data(), bytes.size());
            target_offset += pad8(sizeof(hdr) + bytes.size());
            return pos + sizeof(hdr);
        }

        template <record_op op>
        std::byte* write_record(record_id id, uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            return write_record_at<op>(base, write_offset, id, offset, tag, bytes);
        }

        static constexpr std::size_t page_size = 4096; // de facto standard on x86_64 Linux

        std::byte* base = nullptr;          // the log - append-only, the source of truth
        std::size_t write_offset = 0;
        std::size_t last_synced = 0;

        // The materialized records. A separate file only when split_regions;
        // otherwise it aliases base and data_offset stays 0, because nothing
        // is ever written through it.
        std::byte* data_base = nullptr;
        std::size_t data_offset = 0;

        std::size_t live_byte_count = 0;
        record_id next_id = 1;
        int fd = -1;
        int data_fd = -1;
        std::vector<index_entry> index;
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
    template <typename spec>
    void background_compact(storage<spec>& table, std::mutex& mu,
                            const char* tmp_path, const char* live_path,
                            std::size_t scratch_capacity,
                            const char* tmp_data_path = nullptr, const char* live_data_path = nullptr,
                            int max_catchup_rounds = 5, std::size_t catchup_threshold_bytes = 4096) {
        typename storage<spec>::snapshot snapshot;
        std::size_t caught_up_to;
        std::byte* wal_base;

        { // --- PHASE 1 (short lock): snapshot the current values ---
            std::lock_guard<std::mutex> lock(mu);
            table.snapshot_into(snapshot);
            caught_up_to = table.current_offset();
            wal_base = table.raw_base();
        }

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

        int tmp_data_fd = -1;
        std::byte* scratch_data = nullptr;
        if constexpr (storage<spec>::split_regions) {
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
            if constexpr (storage<spec>::split_regions) table.finish_compaction(scratch, scratch_data, snapshot);
            else table.finish_compaction(scratch, snapshot);
            table.sync();
        }

        ::close(tmp_fd);
        ::rename(tmp_path, live_path);
        if constexpr (storage<spec>::split_regions) {
            ::close(tmp_data_fd);
            ::rename(tmp_data_path, live_data_path);
        }
    }

} // namespace storage
