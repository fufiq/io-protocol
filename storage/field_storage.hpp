#pragma once
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

#include "storage.hpp"
#include "../embedded/platform.hpp"

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
    template <typename word>
    inline word load_relaxed(const word& w) noexcept {
        return std::atomic_ref<word>(const_cast<word&>(w)).load(std::memory_order_relaxed);
    }
    template <typename word>
    inline void store_relaxed(word& w, word v) noexcept {
        std::atomic_ref<word>(w).store(v, std::memory_order_relaxed);
    }

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
            if (base) ::munmap(base, spec::capacity);
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
        void compact(std::byte* scratch_base, int scratch_fd = -1) {
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
            if (base && base != scratch_base) ::munmap(base, spec::capacity);
            if (fd >= 0 && fd != scratch_fd) ::close(fd);
            fd = scratch_fd;
            base = scratch_base;
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
