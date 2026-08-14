#pragma once
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

#include "field_storage.hpp"

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

        // Index keys are integers. A hash, an id, a timestamp, a price, a
        // status - that is what a secondary index is nearly always over, and
        // making it general would mean choosing a hash function on the
        // application's behalf. Hash the field yourself and index the hash.
        template <typename K>
        concept index_key = std::is_integral_v<K> || std::is_enum_v<K>;

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
        bool add(record_id id, const owner& value) { return map.insert((uint64_t)(value.*member), id); }
        void remove(record_id, const owner& value) { map.erase((uint64_t)(value.*member)); }

        // 0 when absent, since an id is issued from 1.
        record_id find(key_type key) const noexcept {
            const uint64_t* found = map.find((uint64_t)key);
            return found ? (record_id)*found : 0;
        }
        bool contains(key_type key) const noexcept { return map.find((uint64_t)key) != nullptr; }
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
        bool add(record_id id, const owner& value) { return map.insert((uint64_t)(value.*member), id); }
        void remove(record_id id, const owner& value) { map.erase((uint64_t)(value.*member), id); }

        void each(key_type key, auto&& fn) const { map.each((uint64_t)key, [&](uint64_t id) { fn((record_id)id); }); }
        std::size_t count(key_type key) const noexcept { return map.count((uint64_t)key); }
        bool empty(key_type key) const noexcept { return map.empty((uint64_t)key); }

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
