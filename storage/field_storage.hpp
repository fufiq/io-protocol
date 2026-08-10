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
// WHAT IT BUYS, and what can honestly be claimed about it.
//
// At 26 GB on a 31 GB machine (bench/big_dataset.cpp), TIMINGS ARE NOT
// TRUSTWORTHY: two consecutive runs of the same binary gave 1345113 and
// 257805 records/sec for the identical fill, and 29469 and 8608 ops/sec for
// the identical random read. A 5x spread from page-cache and filesystem state
// alone, and each engine needs the whole disk, so runs cannot be interleaved
// to cancel it. Any speed comparison at this scale on this machine is noise
// dressed as a result.
//
// What IS stable, because it is counted rather than timed:
//
//   major faults per update    storage.hpp  22013 / 200000 = 0.110
//                              this file     1349 / 700000 = 0.002
//
//     An update here appends the field and moves a pointer. It touches no
//     page of the record at all, so there is nothing to fault on. Patching in
//     place must reach a random page of a 26 GB region every time. Roughly 50x
//     fewer faults per update, and that ratio holds across every run.
//
//   on disk                    56.2 GB  ->  28.5 GB (28.0 log + 0.5 index)
//
//     No second copy. The shipping design keeps the log AND a materialization
//     of every live record.
//
// A CLAIM RETRACTED. An earlier version of this file said reads got 3.5x
// faster because half the footprint means twice the cache hit rate. Repeated
// runs do not support it: read faults came out at 27057, 75523 and 23535 for
// the same 200k reads, which straddles storage.hpp's 78472. The mechanism may
// well be real, but this benchmark cannot show it, and the number should not
// have been stated.
//
// The other cost is index memory: a slot per id (16 bytes) plus a node per
// overridden field (24 bytes), against storage.hpp's 8 bytes per id.

#include "storage.hpp"

#include <string>

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

    struct field_slot {
        uint64_t base;      // 8 - log offset of the CREATE's payload; 0 = no record
        uint32_t head;      // 4 - first override, or no_override
        uint32_t depth;     // 4
    };
    static_assert(sizeof(field_slot) == 16);

    inline constexpr uint32_t no_override = 0xFFFFFFFFu;

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
    inline constexpr uint32_t field_index_version = 1;

    // =====================================================================
    // level is always local_sync here: a field storage owns its files by
    // construction - there is nowhere for a caller-provided region to fit,
    // because the index is not a simple array the caller could supply.
    // =====================================================================
    template <fixed_string path_v, std::size_t capacity_v,
              fixed_string index_path_v, std::size_t max_ids_v, std::size_t max_overrides_v,
              std::size_t max_depth_v = 8, std::size_t max_record_bytes_v = 4096,
              fixed_string checkpoint_path_v = fixed_string("")>
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

        static constexpr const char* checkpoint_path = checkpoint_path_v.value;
        static constexpr bool fast_restart = checkpoint_path_v.value[0] != '\0';

        static_assert(index_path_v.value[0] != '\0', "a field storage keeps its index in a file - give the spec an index_path");
        static_assert(max_depth_v >= 1, "max_depth 0 would collapse on every update, which is just storage.hpp with extra steps");
        static constexpr std::size_t index_bytes =
            sizeof(field_index_header) + max_ids_v * sizeof(field_slot) + max_overrides_v * sizeof(override_node);
    };

    // =====================================================================
    template <typename spec, typename observer = null_observer>
    class field_storage {
    public:
        struct raw_view { record_id id; tag_t tag; std::span<const std::byte> bytes; };

        field_storage() {
            fd = ::open(spec::path.value, O_RDWR | O_CREAT, 0644);
            if (fd < 0) return;
            if (::posix_fallocate(fd, 0, spec::capacity) != 0) return;
            base = (std::byte*)::mmap(nullptr, spec::capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) { base = nullptr; return; }

            index_fd = ::open(spec::index_path, O_RDWR | O_CREAT, 0644);
            if (index_fd < 0) return;
            // Preserved across runs only when a checkpoint can vouch for it;
            // otherwise the previous run's contents are meaningless and
            // dropping them is what guarantees every slot starts empty.
            if constexpr (!spec::fast_restart) { if (::ftruncate(index_fd, 0) != 0) return; }
            if (::posix_fallocate(index_fd, 0, spec::index_bytes) != 0) return;
            void* m = ::mmap(nullptr, spec::index_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, index_fd, 0);
            if (m == MAP_FAILED) return;

            head = (field_index_header*)m;
            slots = (field_slot*)((std::byte*)m + sizeof(field_index_header));
            nodes = (override_node*)((std::byte*)slots + spec::max_ids * sizeof(field_slot));
            if (head->magic != field_index_magic || head->version != field_index_version) reset_header();
        }

        ~field_storage() {
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
            apply<provenance::local>(*header_of(logged), logged);
            return id;
        }

        template <tagged_aggregate T>
        record_id recreate(record_id id, const T& value) {
            static_assert(sizeof(T) <= spec::max_record_bytes, "raise field_spec::max_record_bytes for this type");
            if (id >= spec::max_ids) return 0;
            std::byte* logged = append<record_op::create>(id, 0, T::tag, std::as_bytes(std::span{&value, 1}));
            apply<provenance::local>(*header_of(logged), logged);
            return id;
        }

        template <auto member>
        requires field<member_value_t<member>>
        void update(record_id id, member_value_t<member> value) {
            if (!alive(id)) return;
            const std::size_t off = (std::size_t)&(((member_class_t<member> const volatile*)0)->*member);
            std::byte* logged = append<record_op::update>(id, (uint32_t)off, tag_t{0}, std::as_bytes(std::span{&value, 1}));
            apply<provenance::local>(*header_of(logged), logged);
        }

        // Run-time-offset counterpart of update<member>, for a caller holding
        // bytes without naming a type. Checked, since the offset is data.
        void patch(record_id id, uint32_t offset, std::span<const std::byte> bytes) {
            if (!alive(id)) return;
            if ((std::size_t)offset + bytes.size() > record_len(id)) return;
            std::byte* logged = append<record_op::update>(id, offset, tag_t{0}, bytes);
            apply<provenance::local>(*header_of(logged), logged);
        }

        void destroy(record_id id) {
            if (!alive(id)) return;
            std::byte* logged = append<record_op::destroy>(id, 0, tag_t{0}, {});
            apply<provenance::local>(*header_of(logged), logged);
        }

        // ---- reads ------------------------------------------------------
        //
        // By reference, not by pointer, and that is the API consequence of the
        // whole design: the record is the base CREATE plus its overrides, and
        // those live wherever the log happened to be when each was written.
        // There is no address at which the current record exists.

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
            return alive(id) ? header_of(base + slots[id].base)->tag : tag_t{0};
        }

        bool alive(record_id id) const noexcept { return id < head->slot_count && slots[id].base != 0; }
        uint32_t depth(record_id id) const noexcept { return alive(id) ? slots[id].depth : 0; }

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
                const record_header* h = header_of(base + slots[id].base);
                assemble(id, scratch, h->len);
                fn(raw_view{ id, h->tag, std::span<const std::byte>{ scratch, h->len } });
            }
        }

        // ---- the single application point --------------------------------

        template <provenance from = provenance::external>
        void apply(record_header hdr, const std::byte* data) {
            if (hdr.id >= spec::max_ids) return;
            const std::size_t payload_at = (std::size_t)(data - base);

            if (hdr.op == record_op::create) {
                if (hdr.len > spec::max_record_bytes) return;
                if (hdr.id >= head->slot_count) head->slot_count = hdr.id + 1;
                if (head->next_id <= hdr.id) head->next_id = hdr.id + 1;
                field_slot& s = slots[hdr.id];
                if (s.base) head->live_byte_count -= header_of(base + s.base)->len;

                obs.on_record(*this, hdr, data);
                free_chain(hdr.id);
                s.base = payload_at;
                head->live_byte_count += hdr.len;
                return;
            }

            if (!alive(hdr.id)) return;
            if (hdr.op == record_op::update) {
                // A diff has to land inside the record it claims to patch.
                // For a local update the offset came from a pointer-to-member
                // and there is nothing to re-derive; for a record off the disk
                // or a wire it is just a number that happened to checksum.
                if constexpr (from == provenance::external) {
                    if ((std::size_t)hdr.offset + hdr.len > record_len(hdr.id)) return;
                }
                obs.on_record(*this, hdr, data);
                set_override(hdr.id, hdr.offset, hdr.len, payload_at);
                maybe_collapse(hdr.id);
                return;
            }

            obs.on_record(*this, hdr, data);
            head->live_byte_count -= record_len(hdr.id);
            free_chain(hdr.id);
            slots[hdr.id].base = 0;
        }

        // ---- depth ------------------------------------------------------

        // Rewrites the record as one contiguous CREATE and drops its chain, so
        // the next read is a single access again. Called automatically once a
        // record passes spec::max_depth; exposed because a caller may know
        // better than the threshold does.
        void collapse(record_id id) {
            if (!alive(id) || slots[id].head == no_override) return;
            const record_header* h = header_of(base + slots[id].base);
            const uint32_t len = h->len;
            const tag_t tag = h->tag;
            alignas(8) std::byte scratch[spec::max_record_bytes];
            assemble(id, scratch, len);
            std::byte* logged = append<record_op::create>(id, 0, tag, std::span<const std::byte>{ scratch, len });
            free_chain(id);
            slots[id].base = (std::size_t)(logged - base);
            ++collapses;
        }

        std::size_t collapse_count() const noexcept { return collapses; }

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

        // Replays the log. Identical in shape to storage.hpp's: verify, then
        // hand each record to the same apply() a live write goes through, so
        // an observer maintained above cannot drift from the data.
        std::size_t recover(std::size_t file_size) {
            reset_header();
            const std::size_t pos = scan(base, 0, file_size, [this](record_header hdr, const std::byte* payload) {
                apply(hdr, payload);
                return true;
            });
            write_offset = pos;
            last_synced = pos;
            head->write_offset = pos;
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

        // Flushes the log and the index together, then names the offset they
        // agree at. Same ordering argument as storage.hpp: everything the
        // marker describes is durable before the marker exists.
        bool checkpoint() noexcept requires (spec::fast_restart) {
            sync();
            head->write_offset = write_offset;
            ::msync(head, spec::index_bytes, MS_SYNC);

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
        void discard_checkpoint() noexcept requires (spec::fast_restart) { ::remove(spec::checkpoint_path); }

        // ---- compaction --------------------------------------------------

        // Rewrites every live record as one contiguous CREATE into a fresh
        // region, which drops every chain at once. Compaction here reclaims
        // more than dead records: the whole update history goes with it.
        void compact(std::byte* scratch_base) {
            std::size_t scratch_offset = 0;
            alignas(8) std::byte scratch[spec::max_record_bytes];
            std::size_t live = 0;
            for (record_id id = 1; id < head->slot_count; ++id) {
                if (!alive(id)) continue;
                const record_header* h = header_of(base + slots[id].base);
                const uint32_t len = h->len;
                const tag_t tag = h->tag;
                assemble(id, scratch, len);
                std::byte* placed = write_record_at<record_op::create>(scratch_base, scratch_offset, id, 0, tag,
                                                                       std::span<const std::byte>{ scratch, len });
                free_chain(id);
                slots[id].base = (std::size_t)(placed - scratch_base);
                live += len;
            }
            if (base) ::munmap(base, spec::capacity);
            if (fd >= 0) { ::close(fd); fd = -1; }
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

    private:
        static record_header* header_of(std::byte* data) noexcept { return (record_header*)(data - sizeof(record_header)); }
        static const record_header* header_of(const std::byte* data) noexcept {
            return (const record_header*)(data - sizeof(record_header));
        }

        uint32_t record_len(record_id id) const noexcept { return header_of(base + slots[id].base)->len; }

        // The gather. Base record first, then every overridden field on top -
        // newest chain node first, and since an update to an already-overridden
        // field replaces its node, every node here is the current value.
        void assemble(record_id id, std::byte* out, uint32_t len) const noexcept {
            std::memcpy(out, base + slots[id].base, len);
            for (uint32_t n = slots[id].head; n != no_override; n = nodes[n].next) {
                const override_node& o = nodes[n];
                if ((std::size_t)o.field_offset + o.len <= len)
                    std::memcpy(out + o.field_offset, base + o.log_offset, o.len);
            }
        }

        void set_override(record_id id, uint32_t offset, uint32_t len, std::size_t at) noexcept {
            for (uint32_t n = slots[id].head; n != no_override; n = nodes[n].next) {
                if (nodes[n].field_offset == offset && nodes[n].len == len) { nodes[n].log_offset = at; return; }
            }
            const uint32_t n = alloc_node();
            if (n == no_override) return; // out of nodes: the record keeps its previous value rather than a wrong one
            nodes[n] = { at, offset, len, slots[id].head, 0 };
            slots[id].head = n;
            ++slots[id].depth;
        }

        // The bound on how far a read can degrade. Without it a chain grows
        // with the number of DISTINCT fields a record has ever had touched,
        // and a gather grows with it.
        void maybe_collapse(record_id id) {
            if (slots[id].depth > spec::max_depth) collapse(id);
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

        void free_chain(record_id id) noexcept {
            uint32_t n = slots[id].head;
            while (n != no_override) {
                const uint32_t next = nodes[n].next;
                nodes[n].next = (uint32_t)head->node_free_head;
                head->node_free_head = n;
                n = next;
            }
            slots[id].head = no_override;
            slots[id].depth = 0;
        }

        template <record_op op>
        std::byte* append(record_id id, uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
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
            std::memset(head, 0, sizeof(field_index_header));
            head->magic = field_index_magic;
            head->version = field_index_version;
            head->next_id = 1;
            head->slot_count = 1;      // id 0 is never a record
            head->node_free_head = no_override;
            std::memset(slots, 0, spec::max_ids * sizeof(field_slot));
            for (std::size_t i = 0; i < spec::max_ids; ++i) slots[i].head = no_override;
        }

        static constexpr std::size_t page_size = 4096;

        std::byte* base = nullptr;
        std::size_t write_offset = 0;
        std::size_t last_synced = 0;
        std::size_t last_record_offset = 0;
        std::size_t collapses = 0;
        bool from_checkpoint = false;
        int fd = -1, index_fd = -1;

        field_index_header* head = nullptr;
        field_slot* slots = nullptr;
        override_node* nodes = nullptr;
        [[no_unique_address]] observer obs{};
    };

} // namespace storage
