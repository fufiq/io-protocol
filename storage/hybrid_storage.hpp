#pragma once
// hybrid_storage.hpp - one index shape that expresses BOTH previous designs,
// so the choice between them stops being a choice of engine and becomes a
// per-record property that the engine can change under load.
//
// The observation this is built on is not a design idea, it is a fact about
// the code: field_storage.hpp includes storage.hpp and reuses record_header,
// record_op, checksum_of and pad8 unchanged. The two engines write a
// BYTE-IDENTICAL log. A log produced by either is replayable by the other.
// They differ in exactly two places:
//
//     index entry     storage.hpp  { offset }                       8 bytes
//                     field        { base, head, depth, inline_* }  32 bytes
//
//     what UPDATE     storage.hpp  memcpy into the materialization
//     does            field        push an override node
//
// The second file is not a third difference. It is a consequence of the first:
// a single offset can only mean "the record is contiguous somewhere", and with
// an append-only log the only place it can be contiguous is a second region.
//
// So take the field slot, drop inline_* (variant H did not pay off: 9.2M
// upd/s against the baseline's 12.5M for the same 4.08M reads), and spend one
// bit of `base` saying WHICH region it addresses. Offsets are pad8-aligned, so
// bit 0 was always zero - the same trick deferred_storage.hpp used for its
// dirty bit. The result is 16 bytes, and:
//
//     base in the data region, no chain   -> storage.hpp. Patchable in place,
//                                            readable by pointer.
//     base in the log, chain or not       -> field storage. Updates append,
//                                            reads assemble.
//
// One slot expresses both, so a record moves between them without the index
// changing shape and without anything being rewritten.
//
// WHY THIS IS WORTH THE 8 EXTRA BYTES PER ID. Measured on the same workload
// (bench/collapse_sweep.cpp, 1 GB, 1M records of 1 KB, 262k hot):
//
//     in place (storage.hpp)   19.7M upd/s   7.5M rd/s   83M rd/s by pointer
//     field, no collapse       12.5M upd/s   3.8M rd/s   no pointer read exists
//
// and at 26 GB on a 31 GB machine (bench/big_dataset.cpp) it inverts - field
// wins updates 1.4x and reads 2.5x, and takes 78x fewer major faults per
// update, because in place has to reach a random page of a region that no
// longer fits. Neither answer is right in general. The axis is whether the
// materialization is resident, which is a runtime property, which is why it
// belongs in a runtime decision rather than in a choice of header.
//
// WHAT DECIDES. Not the data size - that is a proxy, and a bad one on a
// machine whose free memory is not the engine's to predict. The direct signal
// is major faults, which is what an in-place write costs when it misses:
// getrusage every spec::fault_probe updates, and if the window took more than
// spec::fault_permille faults per thousand updates, in-place stops.
//
// The switch is ONE-WAY until a compaction. Letting it flip back makes the
// engine hunt the boundary and pay a fresh fault storm on every probe, and
// "the region stopped fitting" does not un-happen by itself.
//
// WHAT IS NOT IN THE FORMAT. Nothing above is on disk. The log does not record
// which mode a record was in, because the log is the same log either way - a
// CREATE and the UPDATEs that followed it. A replay may well split records
// between the two modes differently than the run that wrote them, and the
// VALUES it reconstructs are identical regardless. Mode is derived state, like
// the index itself.

#include "storage.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace storage {

    // Where the newest version of one field lives. An update to a field that
    // is already overridden REPLACES its node, so a chain is bounded by the
    // distinct fields touched, never by the update count.
    struct hybrid_node {
        uint64_t log_offset;
        uint32_t field_offset;
        uint32_t len;
        uint32_t next;
        uint32_t reserved;
    };
    static_assert(sizeof(hybrid_node) == 24);

    // 16 bytes: field_storage's slot with inline_* removed, and one bit of
    // `base` spent on which region it addresses.
    struct hybrid_slot {
        uint64_t base;   // (offset | in_data). 0 = no record; a payload offset
                         // is never 0, it is at least sizeof(record_header).
        uint32_t head;   // first override, or no_link
        uint32_t depth;
    };
    static_assert(sizeof(hybrid_slot) == 16);

    inline constexpr uint32_t no_link = 0xFFFFFFFFu;
    inline constexpr uint64_t in_data_bit = 1;

    struct hybrid_index_header {
        uint64_t magic;
        uint32_t version;
        uint32_t reserved;
        uint64_t slot_count;
        uint64_t node_high_water;
        uint64_t node_free_head;
        uint64_t next_id;
        uint64_t live_byte_count;
        uint64_t write_offset;   // the log position these arrays describe
        uint64_t data_offset;    // the materialization's high-water mark
        uint64_t in_place;       // 1 while in-place updates are still allowed
    };
    inline constexpr uint64_t hybrid_index_magic = 0x5844494452425948ull; // "HYBRIDIX"
    inline constexpr uint32_t hybrid_index_version = 1;

    // =====================================================================
    // data_capacity 0 removes the materialization entirely, which leaves
    // exactly field_storage.hpp's behaviour - that is the degenerate case,
    // not a separate mode, and it is what a caller who wants one file asks
    // for. fault_permille 0 keeps the region but never patches it, so every
    // record is contiguous when created and appends from then on.
    // =====================================================================
    template <fixed_string path_v, std::size_t capacity_v,
              fixed_string data_path_v, std::size_t data_capacity_v,
              fixed_string index_path_v, std::size_t max_ids_v, std::size_t max_overrides_v,
              std::size_t max_depth_v = 8, std::size_t max_record_bytes_v = 4096,
              fixed_string checkpoint_path_v = fixed_string(""),
              std::size_t fault_probe_v = 4096, std::size_t fault_permille_v = 20>
    struct hybrid_spec {
        static constexpr auto path = path_v;
        static constexpr std::size_t capacity = capacity_v;
        static constexpr const char* data_path = data_path_v.value;
        static constexpr std::size_t data_capacity = data_capacity_v;
        static constexpr const char* index_path = index_path_v.value;
        static constexpr std::size_t max_ids = max_ids_v;
        static constexpr std::size_t max_overrides = max_overrides_v;
        static constexpr std::size_t max_depth = max_depth_v;
        static constexpr std::size_t max_record_bytes = max_record_bytes_v;

        // How often to sample the fault counter, and how many major faults per
        // thousand updates the in-place path is allowed before it gives up.
        static constexpr std::size_t fault_probe = fault_probe_v;
        static constexpr std::size_t fault_permille = fault_permille_v;

        static constexpr const char* checkpoint_path = checkpoint_path_v.value;
        static constexpr bool fast_restart = checkpoint_path_v.value[0] != '\0';
        static constexpr bool has_data = data_capacity_v != 0;

        static_assert(index_path_v.value[0] != '\0', "the index lives in a file - give the spec an index_path");
        static_assert(max_depth_v >= 1, "max_depth 0 collapses on every update, which is in-place with extra steps");
        static_assert(!has_data || data_path_v.value[0] != '\0', "a non-zero data_capacity needs a data_path");
        static constexpr std::size_t index_bytes =
            sizeof(hybrid_index_header) + max_ids_v * sizeof(hybrid_slot) + max_overrides_v * sizeof(hybrid_node);
    };

    // =====================================================================
    template <typename spec, typename observer = null_observer>
    class hybrid_storage {
    public:
        struct raw_view { record_id id; tag_t tag; std::span<const std::byte> bytes; };

        hybrid_storage() {
            fd = ::open(spec::path.value, O_RDWR | O_CREAT, 0644);
            if (fd < 0) return;
            if (::posix_fallocate(fd, 0, spec::capacity) != 0) return;
            base = (std::byte*)::mmap(nullptr, spec::capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) { base = nullptr; return; }

            if constexpr (spec::has_data) {
                data_fd = ::open(spec::data_path, O_RDWR | O_CREAT, 0644);
                if (data_fd < 0) return;
                if (::posix_fallocate(data_fd, 0, spec::data_capacity) != 0) return;
                data_base = (std::byte*)::mmap(nullptr, spec::data_capacity, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
                if (data_base == MAP_FAILED) { data_base = nullptr; return; }
            }

            index_fd = ::open(spec::index_path, O_RDWR | O_CREAT, 0644);
            if (index_fd < 0) return;
            if constexpr (!spec::fast_restart) { if (::ftruncate(index_fd, 0) != 0) return; }
            if (::posix_fallocate(index_fd, 0, spec::index_bytes) != 0) return;
            void* m = ::mmap(nullptr, spec::index_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, index_fd, 0);
            if (m == MAP_FAILED) return;

            head = (hybrid_index_header*)m;
            slots = (hybrid_slot*)((std::byte*)m + sizeof(hybrid_index_header));
            nodes = (hybrid_node*)((std::byte*)slots + spec::max_ids * sizeof(hybrid_slot));
            if (head->magic != hybrid_index_magic || head->version != hybrid_index_version) reset_header();
            else { data_offset = head->data_offset; in_place = head->in_place != 0; }
        }

        ~hybrid_storage() {
            if (head) ::munmap(head, spec::index_bytes);
            if (data_base) ::munmap(data_base, spec::data_capacity);
            if (base) ::munmap(base, spec::capacity);
            if (index_fd >= 0) ::close(index_fd);
            if (data_fd >= 0) ::close(data_fd);
            if (fd >= 0) ::close(fd);
        }

        hybrid_storage(const hybrid_storage&) = delete;
        hybrid_storage& operator=(const hybrid_storage&) = delete;

        bool ok() const noexcept { return base && head && (!spec::has_data || data_base); }

        observer& observer_state() noexcept { return obs; }
        const observer& observer_state() const noexcept { return obs; }

        // ---- writes -----------------------------------------------------

        template <tagged_aggregate T>
        record_id create(const T& value) {
            static_assert(sizeof(T) <= spec::max_record_bytes, "raise hybrid_spec::max_record_bytes for this type");
            const record_id id = head->next_id++;
            if (id >= spec::max_ids) return 0;
            std::byte* logged = append<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            if (!logged) return 0;
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        [[nodiscard]] record_id reserve_id() noexcept { return head->next_id++; }

        record_id create_raw(record_id id, tag_t tag, std::span<const std::byte> bytes) {
            if (id >= spec::max_ids || bytes.size() > spec::max_record_bytes) return 0;
            std::byte* logged = append<record_op::create>(id, 0, tag, bytes);
            if (!logged) return 0;
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        template <tagged_aggregate T>
        record_id recreate(record_id id, const T& value) {
            static_assert(sizeof(T) <= spec::max_record_bytes, "raise hybrid_spec::max_record_bytes for this type");
            if (id >= spec::max_ids) return 0;
            std::byte* logged = append<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            if (!logged) return 0;
            apply_local<record_op::create>(*header_of(logged), logged);
            return id;
        }

        // The log entry is written FIRST and unconditionally, whichever way
        // the update is then applied. That is what keeps the two modes off the
        // disk format: durability and recovery see one stream of CREATEs and
        // UPDATEs and never learn that some of them were also patched into a
        // second region.
        template <auto member>
        requires field<member_value_t<member>>
        void update(record_id id, member_value_t<member> value) {
            if (!alive(id)) return;
            const std::size_t off = (std::size_t)&(((member_class_t<member> const volatile*)0)->*member);
            std::byte* logged = append<record_op::update>(id, (uint32_t)off, tag_t{0}, std::as_bytes(std::span{&value, 1}));
            if (!logged) return;
            apply_local<record_op::update>(*header_of(logged), logged);
        }

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

        // The pointer read, which exists again - this is the whole point of
        // keeping a materialization. It returns null exactly when the record
        // is not contiguous, so a caller that wants the fast path can ask for
        // it and fall back rather than guess.
        template <tagged_aggregate T>
        [[nodiscard]] const T* peek(record_id id) const noexcept {
            if (!alive(id)) return nullptr;
            const hybrid_slot& s = slots[id];
            if (s.head != no_link) return nullptr;
            const std::byte* rec = region_of(s.base) + offset_of(s.base);
            if (header_of(rec)->len != sizeof(T)) return nullptr;
            return (const T*)rec;
        }

        template <tagged_aggregate T>
        [[nodiscard]] bool get(record_id id, T& out) const {
            if (!alive(id) || record_len(id) != sizeof(T)) return false;
            assemble(id, (std::byte*)&out, sizeof(T));
            return true;
        }

        template <tagged_aggregate T>
        [[nodiscard]] std::optional<T> get(record_id id) const {
            T value;
            if (!get(id, value)) return std::nullopt;
            return value;
        }

        [[nodiscard]] bool read_raw(record_id id, std::span<std::byte> out) const {
            if (!alive(id)) return false;
            const uint32_t len = record_len(id);
            if (out.size() < len) return false;
            assemble(id, out.data(), len);
            return true;
        }

        [[nodiscard]] tag_t tag_of(record_id id) const noexcept {
            return alive(id) ? header_of(region_of(slots[id].base) + offset_of(slots[id].base))->tag : tag_t{0};
        }

        bool alive(record_id id) const noexcept { return id < head->slot_count && slots[id].base != 0; }
        uint32_t depth(record_id id) const noexcept { return alive(id) ? slots[id].depth : 0; }
        bool contiguous(record_id id) const noexcept { return alive(id) && slots[id].head == no_link; }
        bool materialized(record_id id) const noexcept { return alive(id) && (slots[id].base & in_data_bit); }

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
                const record_header* h = header_of(region_of(slots[id].base) + offset_of(slots[id].base));
                assemble(id, scratch, h->len);
                fn(raw_view{ id, h->tag, std::span<const std::byte>{ scratch, h->len } });
            }
        }

        // ---- the single application point --------------------------------

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

        // Appends the assembled record to the log as one CREATE and drops the
        // chain, then rehomes it into the materialization if that region will
        // still take it - which is what hands the record back its in-place
        // updates and its pointer read.
        void collapse(record_id id) {
            if (!alive(id) || slots[id].head == no_link) return;
            const record_header* h = header_of(region_of(slots[id].base) + offset_of(slots[id].base));
            const uint32_t len = h->len;
            const tag_t tag = h->tag;
            alignas(8) std::byte scratch[spec::max_record_bytes];
            assemble(id, scratch, len);
            std::byte* logged = append<record_op::create>(id, 0, tag, std::span<const std::byte>{ scratch, len });
            if (!logged) return;  // the log is full; the chain stays, which is correct, only slower
            free_chain(id);
            slots[id].base = home_for(id, len, tag, logged);
            ++collapses;
        }

        void collapse_if_deeper_than(record_id id, uint32_t depth_limit) {
            if (alive(id) && slots[id].depth > depth_limit) collapse(id);
        }

        std::size_t collapse_deep(uint32_t depth_limit) {
            sweep.clear();
            for (record_id id = 1; id < head->slot_count; ++id)
                if (alive(id) && slots[id].depth > depth_limit) sweep.push_back({ offset_of(slots[id].base), id });
            std::sort(sweep.begin(), sweep.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [addr, id] : sweep) collapse(id);
            return sweep.size();
        }

        // ---- mode --------------------------------------------------------

        bool in_place_active() const noexcept { return in_place; }

        // Manual control, for a caller that knows something the fault counter
        // cannot: a migration that is about to drop the data region, a bulk
        // load that should not dirty it, a test.
        void set_in_place(bool on) noexcept { in_place = on && spec::has_data; head->in_place = in_place; }

        std::size_t in_place_update_count() const noexcept { return in_place_updates; }
        std::size_t appended_update_count() const noexcept { return appended_updates; }
        std::size_t collapse_count() const noexcept { return collapses; }
        std::size_t refused_append_count() const noexcept { return refused_appends; }
        std::size_t materialized_bytes() const noexcept { return data_offset; }

        // ---- durability --------------------------------------------------

        void sync() noexcept {
            if (write_offset <= last_synced) return;
            const std::size_t start = last_synced & ~(page_size - 1);
            ::msync(base + start, write_offset - start, MS_SYNC);
            last_synced = write_offset;
        }

        std::size_t current_offset() const noexcept { return write_offset; }
        std::size_t live_bytes() const noexcept { return head->live_byte_count; }
        std::size_t overrides_used() const noexcept { return head->node_high_water; }
        double garbage_ratio() const noexcept {
            return write_offset == 0 ? 0.0 : 1.0 - (double)head->live_byte_count / write_offset;
        }

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
            head->data_offset = data_offset;
            return pos;
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

        bool set_last_record_flags(uint8_t bits) noexcept {
            if (last_record_offset == no_record_yet) return false;
            record_header* h = (record_header*)(base + last_record_offset);
            h->flags |= bits;
            h->checksum = checksum_of(*h, base + last_record_offset + sizeof(record_header), h->len);
            return true;
        }

        // Log, materialization and index all flushed before the marker that
        // names the offset they agree at - the same ordering argument as the
        // other two engines, with one more region in it.
        bool checkpoint() noexcept requires (spec::fast_restart) {
            sync();
            if constexpr (spec::has_data)
                if (data_offset) ::msync(data_base, (data_offset + page_size - 1) & ~(page_size - 1), MS_SYNC);
            head->write_offset = write_offset;
            head->data_offset = data_offset;
            head->in_place = in_place;
            ::msync(head, spec::index_bytes, MS_SYNC);

            checkpoint_record rec{};
            rec.magic = checkpoint_magic;
            rec.version = checkpoint_version;
            rec.write_offset = write_offset;
            rec.data_offset = data_offset;
            rec.live_byte_count = head->live_byte_count;
            rec.next_id = head->next_id;
            rec.index_count = head->slot_count;
            rec.tail_crc = log_tail_crc(write_offset);
            rec.checksum = crc32c(&rec, offsetof(checkpoint_record, checksum)) ^ 0xFFFFFFFFu;

            const std::string tmp = std::string(spec::checkpoint_path) + ".tmp";
            int cf = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (cf < 0) return false;
            const bool written = ::pwrite(cf, &rec, sizeof(rec), 0) == (ssize_t)sizeof(rec) && ::fsync(cf) == 0;
            ::close(cf);
            if (!written) { ::remove(tmp.c_str()); return false; }
            if (::rename(tmp.c_str(), spec::checkpoint_path) != 0) { ::remove(tmp.c_str()); return false; }
            sync_parent_directory(spec::checkpoint_path);
            return true;
        }

        std::size_t restart(std::size_t file_size) requires (spec::fast_restart) {
            checkpoint_record rec{};
            if (!read_checkpoint(rec)) return recover(file_size);
            if (rec.write_offset > file_size) return recover(file_size);
            if (head->magic != hybrid_index_magic || head->write_offset != rec.write_offset) return recover(file_size);
            if (head->data_offset != rec.data_offset) return recover(file_size);
            if (rec.write_offset + sizeof(record_header) <= file_size) {
                record_header next{};
                std::memcpy(&next, base + rec.write_offset, sizeof(next));
                if (next.id != 0) return recover(file_size);
            }
            if (log_tail_crc(rec.write_offset) != rec.tail_crc) return recover(file_size);

            write_offset = (std::size_t)rec.write_offset;
            data_offset = (std::size_t)rec.data_offset;
            last_synced = write_offset;
            from_checkpoint = true;
            return write_offset;
        }

        bool started_from_checkpoint() const noexcept { return from_checkpoint; }
        void discard_checkpoint() noexcept requires (spec::fast_restart) { ::remove(spec::checkpoint_path); }

        // ---- compaction --------------------------------------------------

        // TWO PASSES, and the second one is not an optimization - it is
        // required. Pass one assembles every live record into the fresh log,
        // which is the only place that can be read while both old regions are
        // still intact. Only then can the materialization be rebuilt from
        // offset zero, because writing it during pass one would overwrite the
        // very bases that later records are still being assembled from.
        //
        // `rehome` false is the migration the whole design exists to make
        // cheap: every base ends up in the log, no record is patchable any
        // more, and the data file can be deleted.
        void compact(std::byte* scratch_base, bool rehome = true) {
            std::size_t scratch_offset = 0;
            alignas(8) std::byte scratch[spec::max_record_bytes];
            std::size_t live = 0;

            for (record_id id = 1; id < head->slot_count; ++id) {
                if (!alive(id)) continue;
                const record_header* h = header_of(region_of(slots[id].base) + offset_of(slots[id].base));
                const uint32_t len = h->len;
                const tag_t tag = h->tag;
                assemble(id, scratch, len);
                std::byte* placed = write_record_at<record_op::create>(scratch_base, scratch_offset, id, 0, tag,
                                                                       std::span<const std::byte>{ scratch, len });
                free_chain(id);
                slots[id].base = (uint64_t)(placed - scratch_base);   // log-homed, bit clear
                live += len;
            }

            if (base) ::munmap(base, spec::capacity);
            if (fd >= 0) { ::close(fd); fd = -1; }
            base = scratch_base;
            write_offset = scratch_offset;
            last_synced = 0;

            data_offset = 0;
            in_place = spec::has_data && rehome;
            if constexpr (spec::has_data) {
                if (rehome) {
                    for (record_id id = 1; id < head->slot_count; ++id) {
                        if (!alive(id)) continue;
                        const std::byte* rec = base + offset_of(slots[id].base);
                        const record_header* h = header_of(rec);
                        slots[id].base = home_for(id, h->len, h->tag, rec);
                    }
                }
            }

            head->live_byte_count = live;
            head->write_offset = scratch_offset;
            head->data_offset = data_offset;
            head->in_place = in_place;
            head->node_high_water = 0;
            head->node_free_head = no_link;
        }

        std::byte* raw_base() const noexcept { return base; }
        std::byte* raw_data_base() const noexcept { return data_base; }

    private:
        // ---- region addressing -------------------------------------------

        static constexpr std::size_t offset_of(uint64_t b) noexcept { return (std::size_t)(b & ~in_data_bit); }
        const std::byte* region_of(uint64_t b) const noexcept { return (b & in_data_bit) ? data_base : base; }
        std::byte* region_of(uint64_t b) noexcept { return (b & in_data_bit) ? data_base : base; }

        // Puts a freshly logged record where it should live. In the
        // materialization if there is one, it is still being patched, and it
        // has room; otherwise the log copy that was just written is the
        // record, and it is contiguous there too - it simply cannot be patched.
        uint64_t home_for(record_id id, uint32_t len, tag_t tag, const std::byte* logged) {
            if constexpr (spec::has_data) {
                const std::size_t need = pad8(sizeof(record_header) + len);
                if (in_place && data_base && data_offset + need <= spec::data_capacity) {
                    std::byte* placed = write_record_at<record_op::create>(data_base, data_offset, id, 0, tag,
                                                                           std::span<const std::byte>{ logged, len });
                    head->data_offset = data_offset;
                    return (uint64_t)(placed - data_base) | in_data_bit;
                }
            }
            return (uint64_t)(logged - base);
        }

        // ---- the three op bodies -----------------------------------------

        void apply_create(const record_header& hdr, const std::byte* data) {
            if (hdr.len > spec::max_record_bytes) return;
            if (hdr.id >= head->slot_count) head->slot_count = hdr.id + 1;
            if (head->next_id <= hdr.id) head->next_id = hdr.id + 1;
            hybrid_slot& s = slots[hdr.id];
            if (s.base) head->live_byte_count -= record_len(hdr.id);
            obs.on_record(*this, hdr, data);
            free_chain(hdr.id);
            s.base = home_for(hdr.id, hdr.len, hdr.tag, data);
            head->live_byte_count += hdr.len;
        }

        // The fork. Both arms have already appended the same log record; they
        // differ only in what derived state they touch.
        template <provenance from>
        void apply_update(const record_header& hdr, const std::byte* payload) {
            if (!alive(hdr.id)) return;
            if constexpr (from == provenance::external) {
                if ((std::size_t)hdr.offset + hdr.len > record_len(hdr.id)) return;
            }
            obs.on_record(*this, hdr, payload);
            hybrid_slot& s = slots[hdr.id];
            if (in_place && (s.base & in_data_bit) && s.head == no_link) {
                std::memcpy(data_base + offset_of(s.base) + hdr.offset, payload, hdr.len);
                ++in_place_updates;
            } else {
                set_override(hdr.id, hdr.offset, hdr.len, (std::size_t)(payload - base));
                ++appended_updates;
                if (s.depth > spec::max_depth) collapse(hdr.id);
            }
            probe_faults();
        }

        void apply_destroy(const record_header& hdr) {
            if (!alive(hdr.id)) return;
            obs.on_record(*this, hdr, nullptr);
            head->live_byte_count -= record_len(hdr.id);
            free_chain(hdr.id);
            slots[hdr.id].base = 0;
        }

        // ---- the policy ---------------------------------------------------

        // Sampled, not measured per update: getrusage is a syscall, and at one
        // per spec::fault_probe updates it is under a nanosecond amortized.
        void probe_faults() noexcept {
            if constexpr (!spec::has_data) return;
            else {
                if (!in_place) return;
                if (++probe_counter < spec::fault_probe) return;
                probe_counter = 0;
                struct rusage ru;
                if (::getrusage(RUSAGE_SELF, &ru) != 0) return;
                const long taken = ru.ru_majflt - last_majflt;
                last_majflt = ru.ru_majflt;
                if (spec::fault_permille == 0 ||
                    (unsigned long)taken * 1000ull > (unsigned long)spec::fault_probe * spec::fault_permille) {
                    in_place = false;
                    head->in_place = 0;
                    ++mode_switches;
                }
            }
        }

    public:
        std::size_t mode_switch_count() const noexcept { return mode_switches; }

    private:
        static record_header* header_of(std::byte* data) noexcept { return (record_header*)(data - sizeof(record_header)); }
        static const record_header* header_of(const std::byte* data) noexcept {
            return (const record_header*)(data - sizeof(record_header));
        }

        uint32_t record_len(record_id id) const noexcept {
            return header_of(region_of(slots[id].base) + offset_of(slots[id].base))->len;
        }

        // The base record first - from whichever region holds it - then every
        // overridden field on top. An in-place record has no chain, so this
        // degenerates to the single memcpy storage.hpp would have done.
        void assemble(record_id id, std::byte* out, uint32_t len) const noexcept {
            const hybrid_slot& s = slots[id];
            std::memcpy(out, region_of(s.base) + offset_of(s.base), len);
            for (uint32_t n = s.head; n != no_link; n = nodes[n].next) {
                const hybrid_node& o = nodes[n];
                if ((std::size_t)o.field_offset + o.len <= len)
                    std::memcpy(out + o.field_offset, base + o.log_offset, o.len);
            }
        }

        void set_override(record_id id, uint32_t offset, uint32_t len, std::size_t at) noexcept {
            hybrid_slot& s = slots[id];
            for (uint32_t n = s.head; n != no_link; n = nodes[n].next)
                if (nodes[n].field_offset == offset && nodes[n].len == len) { nodes[n].log_offset = at; return; }
            const uint32_t n = alloc_node();
            if (n == no_link) return; // out of nodes: keep the previous value rather than a wrong one
            nodes[n] = { at, offset, len, s.head, 0 };
            s.head = n;
            ++s.depth;
        }

        uint32_t alloc_node() noexcept {
            if (head->node_free_head != no_link) {
                const uint32_t n = (uint32_t)head->node_free_head;
                head->node_free_head = nodes[n].next;
                return n;
            }
            if (head->node_high_water >= spec::max_overrides) return no_link;
            return (uint32_t)head->node_high_water++;
        }

        void free_chain(record_id id) noexcept {
            uint32_t n = slots[id].head;
            while (n != no_link) {
                const uint32_t next = nodes[n].next;
                nodes[n].next = (uint32_t)head->node_free_head;
                head->node_free_head = n;
                n = next;
            }
            slots[id].head = no_link;
            slots[id].depth = 0;
        }

        template <record_op op>
        std::byte* append(record_id id, uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            const std::size_t need = pad8(sizeof(record_header) + bytes.size());
            if (write_offset + need > spec::capacity) { ++refused_appends; return nullptr; }
            last_record_offset = write_offset;
            return write_record_at<op>(base, write_offset, id, offset, tag, bytes);
        }

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
            int cf = ::open(spec::checkpoint_path, O_RDONLY);
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
            std::memset(head, 0, sizeof(hybrid_index_header));
            head->magic = hybrid_index_magic;
            head->version = hybrid_index_version;
            head->next_id = 1;
            head->slot_count = 1;   // id 0 is never a record
            head->node_free_head = no_link;
            std::memset(slots, 0, spec::max_ids * sizeof(hybrid_slot));
            for (std::size_t i = 0; i < spec::max_ids; ++i) slots[i].head = no_link;
            data_offset = 0;
            in_place = spec::has_data;
            head->in_place = in_place;
            struct rusage ru;
            last_majflt = (::getrusage(RUSAGE_SELF, &ru) == 0) ? ru.ru_majflt : 0;
        }

        static constexpr std::size_t page_size = 4096;

        std::byte* base = nullptr;
        std::byte* data_base = nullptr;
        std::size_t write_offset = 0, data_offset = 0, last_synced = 0;
        static constexpr std::size_t no_record_yet = (std::size_t)-1;
        std::size_t last_record_offset = no_record_yet;
        std::size_t collapses = 0, refused_appends = 0, mode_switches = 0;
        std::size_t in_place_updates = 0, appended_updates = 0;
        std::size_t probe_counter = 0;
        long last_majflt = 0;
        bool in_place = spec::has_data;
        bool from_checkpoint = false;
        std::vector<std::pair<std::size_t, record_id>> sweep;
        int fd = -1, data_fd = -1, index_fd = -1;

        hybrid_index_header* head = nullptr;
        hybrid_slot* slots = nullptr;
        hybrid_node* nodes = nullptr;
        [[no_unique_address]] observer obs{};
    };

} // namespace storage
