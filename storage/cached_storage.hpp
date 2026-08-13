#pragma once
// ============================================================================
// NOT PART OF THE SUPPORTED SURFACE as of the first stable release.
//
// The field engine is the one storage this project supports, documents and
// tests. This file is kept because it works and because its measurements are
// what justified that choice - deleting it would delete the evidence - but it
// has no test coverage, no specification, and no compatibility promise. It is
// still compiled by the benches that use it, so it cannot rot silently.
//
// Read it as a record of an experiment, not as an API.
// ============================================================================
// cached_storage.hpp - the field engine plus a bounded materialization that is
// a CACHE, not a home.
//
// WHY THE PREVIOUS ATTEMPT FAILED, because this file exists to fix exactly
// that. hybrid_storage.hpp made the materialization AUTHORITATIVE: a slot's
// `base` pointed into it, so the region had to hold every record ever created,
// and once it stopped fitting there was no way out of it - reads still had to
// fault into it, and dropping it meant rewriting the whole live set. Measured
// at 26 GB on a 24 GB machine (bench/big_dataset.cpp), against the two engines
// it was supposed to combine:
//
//                        in place    field      hybrid
//     fill (rec/sec)       234066    271962     162938   <- worst
//     random read            2880      6453       2684   <- worst
//     random update          7427     29838       9459
//     read hot, before      12186     23006       6283   <- worst
//     read hot, after    31878444   3267553      13368   <- 245x worse
//     maj faults/update     34903     10990       4609   <- best
//     on disk              56.2 GB   29.0 GB    56.5 GB
//
// The mode switch itself worked - one switch on the first probe window, and
// the fault rate per update became the best of the three. The WRITE side was
// fixed. The read side was destroyed, because the bases stayed in a 26.6 GB
// region that could never be resident, and the fill was doubled because every
// create wrote both regions.
//
// A CACHE HAS NONE OF THOSE PROBLEMS, and the reason is one sentence: it owns
// nothing. The authoritative value is always the base CREATE plus the override
// chain, exactly as in field_storage.hpp. So the cache can be absent, bounded,
// evicted, dropped, or lost across a restart, at any moment, with no migration
// and no risk to correctness. A miss is not an error - it is the field path.
//
// Which turns every knob that was structural into a number:
//
//     cache_bytes >= dataset   everything ends up cached, updates patch a
//                              resident slot, reads return a pointer. This is
//                              storage.hpp's behaviour, reached by policy.
//     cache_bytes == 0         no region at all. This is field_storage.hpp,
//                              byte for byte.
//     in between               the cache holds what is read, sized by a budget
//                              a human set, so it is resident BY CONSTRUCTION
//                              rather than by hoping the dataset stays small.
//
// TWO CONSEQUENCES WORTH SPELLING OUT.
//
// Write-through is free again. It was fatal in the hybrid because the region
// was 26.6 GB and a patch was a random fault into it; against a cache that fits
// its budget, the slot is resident by definition. So an update to a cached
// record patches the slot, and no invalidation scheme is needed.
//
// create does NOT populate the cache. Filling 27M records through a bounded
// cache would be pure eviction churn, and the hybrid's fill regression
// (162938 against 271962) came from exactly that kind of eager write. The
// cache fills on READ, which is the only access that proves a record is worth
// keeping.
//
// EVICTION IS CLOCK, NOT LRU, and that is not a detail. LRU has to write to a
// list on every HIT, which turns the cheap case - the second read, the whole
// point of the cache - into a dirtied cache line. CLOCK sets one bit, and only
// when it is not already set. The honest limit of "the second read is free" is
// that it holds while the working set fits the budget; past that the hand goes
// round faster than records are reused and the second read is a miss again.

#include "field_storage.hpp"

#include <memory>
#include <vector>

namespace storage {

    template <fixed_string path_v, std::size_t capacity_v,
              fixed_string index_path_v, std::size_t max_ids_v, std::size_t max_overrides_v,
              fixed_string cache_path_v, std::size_t cache_bytes_v,
              std::size_t max_depth_v = 8, std::size_t max_record_bytes_v = 4096,
              bool admit_on_create_v = false,
              bool admit_on_second_v = false,
              fixed_string checkpoint_path_v = fixed_string("")>
    struct cached_spec {
        // Everything the underlying field engine needs, unchanged - the cache
        // adds no parameter to it and no field to its index, because it is not
        // part of the record's identity.
        using inner = field_spec<path_v, capacity_v, index_path_v, max_ids_v, max_overrides_v,
                                 max_depth_v, max_record_bytes_v, checkpoint_path_v>;

        static constexpr const char* cache_path = cache_path_v.value;
        static constexpr std::size_t cache_bytes = cache_bytes_v;
        static constexpr bool has_cache = cache_bytes_v != 0;
        static constexpr std::size_t max_ids = max_ids_v;
        static constexpr std::size_t max_record_bytes = max_record_bytes_v;

        // One slot per record, fixed stride. A cache does not need to pack:
        // paying max_record_bytes for a shorter record wastes budget, not
        // correctness, and the alternative - a free-list allocator over the
        // region - costs a lookup on the path that exists to be fast.
        static constexpr std::size_t stride = (max_record_bytes_v + 63) & ~std::size_t(63);
        static constexpr std::size_t slots = cache_bytes_v / stride;

        // Only worth it when the budget covers the whole dataset: then the
        // cache is never evicting and a create can put the record straight in,
        // which spares the first read its miss. With a bounded cache this is
        // pure churn - see the header.
        static constexpr bool admit_on_create = admit_on_create_v;

        // Admit on the SECOND sighting, not the first. Measured at 26 GB with
        // a 2 GB budget, admitting on every miss gave 25% hits, 298359 misses,
        // ZERO evictions - the cache never even filled - and halved random
        // reads against the bare field engine (7949 against 17549). Every one
        // of those misses paid a memcpy to install a record nobody asked for
        // twice, and the RAM it took came out of the log's page cache, which
        // is what serves the misses.
        //
        // The filter is one id per bucket, no chaining: seeing a different id
        // in a bucket overwrites it. That loses sightings, which is the right
        // failure - a record that is genuinely hot will be seen again soon.
        static constexpr bool admit_on_second = admit_on_second_v;
        static constexpr std::size_t filter_entries =
            admit_on_second_v ? (slots < 1024 ? 4096 : 4 * ((slots + 1023) & ~std::size_t(1023))) : 0;

        static_assert(!has_cache || cache_path_v.value[0] != '\0', "a non-zero cache_bytes needs a cache_path");
        static_assert(!has_cache || cache_bytes_v / ((max_record_bytes_v + 63) & ~std::size_t(63)) >= 1,
                      "cache_bytes is smaller than one record");
    };

    // =====================================================================
    // Composition, not a copy: the field engine underneath is the same code
    // that was measured, so a difference in these numbers is the cache and
    // nothing else.
    // =====================================================================
    template <typename spec, typename observer = null_observer>
    class cached_storage {
    public:
        using inner_type = field_storage<typename spec::inner, observer>;
        using raw_view = typename inner_type::raw_view;

        cached_storage() {
            inner = std::make_unique<inner_type>();
            if constexpr (spec::has_cache) {
                cache_fd = ::open(spec::cache_path, O_RDWR | O_CREAT, 0644);
                if (cache_fd < 0) return;
                if (::posix_fallocate(cache_fd, 0, spec::cache_bytes) != 0) return;
                cache = (std::byte*)::mmap(nullptr, spec::cache_bytes, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, cache_fd, 0);
                if (cache == MAP_FAILED) { cache = nullptr; return; }
                // Derived state, so it starts empty every run and never needs
                // to be checkpointed, recovered or validated.
                where.assign(spec::max_ids, 0);
                if constexpr (spec::admit_on_second) filter.assign(spec::filter_entries, 0);
                owner.assign(spec::slots, 0);
                referenced.assign(spec::slots, 0);
            }
        }

        ~cached_storage() {
            if (cache) ::munmap(cache, spec::cache_bytes);
            if (cache_fd >= 0) ::close(cache_fd);
        }

        cached_storage(const cached_storage&) = delete;
        cached_storage& operator=(const cached_storage&) = delete;

        bool ok() const noexcept { return inner && inner->ok() && (!spec::has_cache || cache); }

        // NOT named field(): that shadows the `field` concept this header's
        // own update<member> constrains on, and the error it produces names
        // the concept rather than the accessor.
        inner_type& engine() noexcept { return *inner; }
        const inner_type& engine() const noexcept { return *inner; }
        observer& observer_state() noexcept { return inner->observer_state(); }

        // ---- writes -----------------------------------------------------

        template <tagged_aggregate T>
        record_id create(const T& value) {
            const record_id id = inner->create(value);
            if constexpr (spec::has_cache) {
                if (id && spec::admit_on_create) admit(id, (const std::byte*)&value, sizeof(T));
            }
            return id;
        }

        record_id create_raw(record_id id, tag_t tag, std::span<const std::byte> bytes) {
            const record_id got = inner->create_raw(id, tag, bytes);
            if constexpr (spec::has_cache) {
                if (got && spec::admit_on_create) admit(got, bytes.data(), (uint32_t)bytes.size());
                else if (got) evict_id(got);   // the shape changed under any cached copy
            }
            return got;
        }

        template <tagged_aggregate T>
        record_id recreate(record_id id, const T& value) {
            const record_id got = inner->recreate(id, value);
            if constexpr (spec::has_cache) {
                if (got) { evict_id(got); if (spec::admit_on_create) admit(got, (const std::byte*)&value, sizeof(T)); }
            }
            return got;
        }

        // The log entry and the override chain are written exactly as the
        // field engine writes them - the cache is a third, optional write to a
        // page that is resident by construction, and it is what keeps a cached
        // record's slot the current value rather than a stale one.
        template <auto member>
        requires field<member_value_t<member>>
        void update(record_id id, member_value_t<member> value) {
            inner->template update<member>(id, value);
            if constexpr (spec::has_cache) {
                const uint32_t slot = where[id];
                if (slot) {
                    const std::size_t off = (std::size_t)&(((member_class_t<member> const volatile*)0)->*member);
                    std::memcpy(cache + (slot - 1) * spec::stride + off, &value, sizeof(value));
                }
            }
        }

        void patch(record_id id, uint32_t offset, std::span<const std::byte> bytes) {
            inner->patch(id, offset, bytes);
            if constexpr (spec::has_cache) {
                const uint32_t slot = where[id];
                if (slot) std::memcpy(cache + (slot - 1) * spec::stride + offset, bytes.data(), bytes.size());
            }
        }

        void destroy(record_id id) {
            if constexpr (spec::has_cache) evict_id(id);
            inner->destroy(id);
        }

        // ---- reads ------------------------------------------------------

        template <tagged_aggregate T>
        [[nodiscard]] bool get(record_id id, T& out) const {
            if constexpr (spec::has_cache) {
                if (id < spec::max_ids) {
                    const uint32_t slot = where[id];
                    if (slot) {
                        touch(slot);
                        std::memcpy(&out, cache + (slot - 1) * spec::stride, sizeof(T));
                        ++hits;
                        return true;
                    }
                }
                if (!inner->get(id, out)) return false;
                ++misses;
                if (const_cast<cached_storage*>(this)->should_admit(id))
                    const_cast<cached_storage*>(this)->admit(id, (const std::byte*)&out, sizeof(T));
                return true;
            } else {
                return inner->get(id, out);
            }
        }

        template <tagged_aggregate T>
        [[nodiscard]] std::optional<T> get(record_id id) const {
            T value;
            if (!get(id, value)) return std::nullopt;
            return value;
        }

        // A pointer straight into the cache. Valid precisely while the record
        // is cached, which write-through keeps current - this is the read that
        // the field engine cannot offer at all, handed back by the cache.
        template <tagged_aggregate T>
        [[nodiscard]] const T* peek(record_id id) const noexcept {
            if constexpr (!spec::has_cache) return nullptr;
            else {
                if (id >= spec::max_ids) return nullptr;
                const uint32_t slot = where[id];
                if (!slot) return nullptr;
                touch(slot);
                return (const T*)(cache + (slot - 1) * spec::stride);
            }
        }

        [[nodiscard]] bool read_raw(record_id id, std::span<std::byte> out) const {
            return inner->read_raw(id, out);
        }

        [[nodiscard]] tag_t tag_of(record_id id) const noexcept { return inner->tag_of(id); }
        bool alive(record_id id) const noexcept { return inner->alive(id); }
        uint32_t depth(record_id id) const noexcept { return inner->depth(id); }
        bool cached(record_id id) const noexcept {
            if constexpr (!spec::has_cache) return false;
            else return id < spec::max_ids && where[id] != 0;
        }

        template <tagged_aggregate T>
        void for_each(auto&& fn) const { inner->template for_each<T>(fn); }
        void for_each_raw(auto&& fn) const { inner->for_each_raw(fn); }

        // ---- the application point ---------------------------------------

        // Replayed and replicated records go through the field engine as
        // before; the cache is dropped for that id rather than updated,
        // because a record arriving from a log or a wire is exactly the case
        // where the cached copy cannot be trusted to be its predecessor.
        template <provenance from = provenance::external>
        void apply(record_header hdr, const std::byte* data) {
            if constexpr (spec::has_cache) { if (hdr.id < spec::max_ids) evict_id(hdr.id); }
            inner->template apply<from>(hdr, data);
        }

        // ---- pass-through -------------------------------------------------

        void collapse(record_id id) { inner->collapse(id); }
        void collapse_if_deeper_than(record_id id, uint32_t d) { inner->collapse_if_deeper_than(id, d); }
        std::size_t collapse_deep(uint32_t d) { return inner->collapse_deep(d); }
        void sync() noexcept { inner->sync(); }
        std::size_t current_offset() const noexcept { return inner->current_offset(); }
        std::size_t live_bytes() const noexcept { return inner->live_bytes(); }
        std::size_t overrides_used() const noexcept { return inner->overrides_used(); }
        double garbage_ratio() const noexcept { return inner->garbage_ratio(); }
        bool compaction_due(double r = 0.5, std::size_t b = 64ull << 20) const noexcept { return inner->compaction_due(r, b); }
        std::size_t compacted_size() const noexcept { return inner->compacted_size(); }
        std::size_t collapse_count() const noexcept { return inner->collapse_count(); }
        bool set_last_record_flags(uint8_t bits) noexcept { return inner->set_last_record_flags(bits); }
        std::size_t for_each_record(std::size_t from, auto&& fn) const { return inner->for_each_record(from, fn); }
        [[nodiscard]] record_id reserve_id() noexcept { return inner->reserve_id(); }

        std::size_t recover(std::size_t file_size) {
            if constexpr (spec::has_cache) drop_cache();
            return inner->recover(file_size);
        }
        bool checkpoint() noexcept { return inner->checkpoint(); }
        std::size_t restart(std::size_t file_size) {
            if constexpr (spec::has_cache) drop_cache();
            return inner->restart(file_size);
        }
        bool started_from_checkpoint() const noexcept { return inner->started_from_checkpoint(); }
        void discard_checkpoint() noexcept { inner->discard_checkpoint(); }

        void compact(std::byte* scratch_base, int scratch_fd = -1) {
            if constexpr (spec::has_cache) drop_cache();
            inner->compact(scratch_base, scratch_fd);
        }

        // ---- cache accounting ---------------------------------------------

        std::size_t cache_hits() const noexcept { return hits; }
        std::size_t cache_misses() const noexcept { return misses; }
        std::size_t cache_evictions() const noexcept { return evictions; }
        std::size_t cache_resident() const noexcept { return resident; }
        std::size_t cache_slot_count() const noexcept { return spec::has_cache ? spec::slots : 0; }
        double cache_hit_ratio() const noexcept {
            const std::size_t n = hits + misses;
            return n ? (double)hits / n : 0.0;
        }

        // Throws the whole cache away. Free, and correct at any moment - that
        // is the property the authoritative region never had.
        std::size_t cache_admissions() const noexcept { return second_sightings; }
        void release_log(std::size_t upto) noexcept { inner->release_log(upto); }

        void drop_cache() noexcept {
            if constexpr (spec::has_cache) {
                std::fill(where.begin(), where.end(), 0);
                std::fill(owner.begin(), owner.end(), 0);
                std::fill(referenced.begin(), referenced.end(), 0);
                hand = 0;
                resident = 0;
            }
        }

    private:
        // One byte, and only written when it is not already set, so a hit on
        // an already-referenced slot writes nothing at all.
        void touch(uint32_t slot) const noexcept {
            if (!referenced[slot - 1]) referenced[slot - 1] = 1;
        }

        // One id per bucket. A hit clears the bucket rather than leaving it
        // set, so a record admitted once has to earn its way back in again.
        bool should_admit(record_id id) noexcept {
            if constexpr (!spec::admit_on_second) return true;
            else {
                uint64_t h = id;
                h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
                h ^= h >> 27; h *= 0x94d049bb133111ebull;
                h ^= h >> 31;
                const std::size_t i = (std::size_t)h & (spec::filter_entries - 1);
                if (filter[i] == id) { filter[i] = 0; ++second_sightings; return true; }
                filter[i] = id;
                return false;
            }
        }

        void evict_id(record_id id) noexcept {
            if (id >= spec::max_ids) return;
            const uint32_t slot = where[id];
            if (!slot) return;
            where[id] = 0;
            owner[slot - 1] = 0;
            referenced[slot - 1] = 0;
            free_slots.push_back(slot);
            --resident;
        }

        // CLOCK: sweep the hand, give a referenced slot one second chance by
        // clearing its bit, take the first slot that has none.
        uint32_t claim_slot() noexcept {
            if (!free_slots.empty()) { const uint32_t s = free_slots.back(); free_slots.pop_back(); return s; }
            for (;;) {
                if (hand >= spec::slots) hand = 0;
                const std::size_t i = hand++;
                if (referenced[i]) { referenced[i] = 0; continue; }
                if (owner[i]) { where[owner[i]] = 0; ++evictions; --resident; }
                return (uint32_t)(i + 1);
            }
        }

        void admit(record_id id, const std::byte* bytes, uint32_t len) noexcept {
            if constexpr (!spec::has_cache) return;
            else {
                if (id >= spec::max_ids || len > spec::stride) return;
                if (where[id]) return;
                const uint32_t slot = claim_slot();
                std::memcpy(cache + (slot - 1) * spec::stride, bytes, len);
                owner[slot - 1] = id;
                referenced[slot - 1] = 1;
                where[id] = slot;
                ++resident;
            }
        }

        std::unique_ptr<inner_type> inner;
        std::byte* cache = nullptr;
        int cache_fd = -1;

        // Derived state, all of it: nothing here is written to a file, and a
        // restart legitimately starts with an empty cache.
        std::vector<uint32_t> where;      // id   -> slot + 1, 0 = not cached
        std::vector<record_id> owner;     // slot -> id, 0 = free
        mutable std::vector<uint8_t> referenced;  // slot -> CLOCK bit, set on a const read
        std::vector<uint32_t> free_slots;
        std::vector<record_id> filter;    // recently-seen ids, for admit_on_second
        std::size_t hand = 0, resident = 0, evictions = 0, second_sightings = 0;
        mutable std::size_t hits = 0, misses = 0;
    };

} // namespace storage
