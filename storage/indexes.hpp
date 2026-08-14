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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
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
        // object: the same trick storage.hpp's member_offset uses, and the
        // reason a pointer-to-member is enough to name a field.
        //
        // Not consteval, and it cannot be: the expression is a reinterpret_cast
        // under the covers, which is exactly what a constant expression may not
        // contain. It was declared consteval here and nothing had called it, so
        // the first caller is what found out.
        template <auto member>
        std::size_t offset_of() noexcept {
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

        // What an index IS, as a number: its kind, the record type it belongs
        // to, where in that record its field sits, and how wide the key is.
        // Every one of those changes the meaning of the bytes on disk, so every
        // one of them has to change the number that decides whether those bytes
        // are still readable.
        inline uint64_t index_signature(uint64_t kind, uint64_t owner_tag,
                                           uint64_t field_offset, uint64_t key_size) noexcept {
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint64_t part : { kind, owner_tag, field_offset, key_size }) {
                h ^= part;
                h *= 0x100000001b3ull;
            }
            return h;
        }

        inline constexpr std::size_t round_pow2(std::size_t n) noexcept {
            std::size_t p = 1;
            while (p < n) p <<= 1;
            return p;
        }

    } // namespace detail

    // =====================================================================
    // Where every index in a set lives: one contiguous block of bytes, carved
    // up by a bump pointer.
    //
    // The block is a file mapping when the engine gives it one, and a heap
    // buffer otherwise, and NOTHING below this class knows which - that is the
    // entire reason it exists. An index made of std::vectors is a web of
    // allocator-owned regions that means nothing to the next run; the same index
    // made of spans over one block is a file, and a file the engine can stamp
    // with a log offset is an index that survives a restart for free.
    //
    // Sizes are carved once and never grow, which is the rule the whole engine
    // already follows. That is what makes the two passes below safe.
    //
    // TWO PASSES, ONE CODE PATH. The engine has to size the file before it can
    // map it, and only the index set knows how big it should be. So place() is
    // run first against a MEASURING arena - no memory, same arithmetic, every
    // take() returning an empty span - and used() is the answer. Then the file
    // is mapped and the identical place() runs against a real one. Writing the
    // size calculation separately from the layout is how these things drift;
    // here there is only one of them, so they cannot.
    // =====================================================================
    class index_arena {
    public:
        constexpr index_arena() noexcept = default;
        constexpr index_arena(std::byte* bytes, std::size_t size) noexcept : base(bytes), limit(size) {}

        static constexpr index_arena measuring() noexcept { return index_arena(nullptr, (std::size_t)-1); }

        template <typename T>
        std::span<T> take(std::size_t count) noexcept {
            offset = (offset + alignof(T) - 1) & ~(alignof(T) - 1);
            const std::size_t bytes = count * sizeof(T);
            if (offset + bytes > limit) { overflowed = true; return {}; }
            // The measuring arena has no memory, so the offset is advanced
            // WITHOUT forming an address. `nullptr + offset` is undefined even
            // when the result is thrown away, and it is the kind of undefined
            // that costs nothing until a compiler decides base cannot be null
            // and deletes the check below it.
            //
            // Found by clang's UBSan on Android, not by GCC's on this machine:
            // -fsanitize=pointer-overflow is part of clang's `undefined` group
            // and GCC does not implement it at all. The Linux run was clean and
            // the code was still wrong.
            std::span<T> out{};
            if (base) out = std::span<T>((T*)(base + offset), count);
            offset += bytes;
            return out;
        }

        std::size_t used() const noexcept { return offset; }
        bool ok() const noexcept { return !overflowed; }
        bool measuring_only() const noexcept { return base == nullptr; }

    private:
        std::byte* base = nullptr;
        std::size_t limit = 0;
        std::size_t offset = 0;
        bool overflowed = false;
    };

    // =====================================================================
    // Open-addressed hash map with linear probing, over an array carved from an
    // arena. Key 0 means "empty", which costs nothing for the keys an index
    // actually holds - a record id is issued from 1, and a hash colliding with
    // exactly zero is a case worth losing to keep the probe branch-free.
    //
    // Deletion shifts backward rather than leaving a tombstone. Tombstones
    // accumulate over a long-lived index and turn every lookup into a scan,
    // and an index that survives restarts is long-lived by construction.
    // =====================================================================
    class flat_hash_map {
    public:
        // Only `used` lives in the arena. Everything else about the layout -
        // the capacity, and therefore the mask - is derived from the same
        // compile-time record count on every run, so storing it would be
        // storing a second copy of something that cannot disagree.
        struct header { uint64_t used; };

        void place(index_arena& arena, std::size_t entries) {
            const std::size_t capacity = detail::round_pow2(entries < 8 ? 8 : entries * 2);
            mask = capacity - 1;
            meta = arena.take<header>(1).data();
            slots = arena.take<slot>(capacity);
        }

        void clear() noexcept {
            if (!meta) return;
            meta->used = 0;
            std::memset(slots.data(), 0, slots.size() * sizeof(slot));
        }

        bool insert(uint64_t key, uint64_t value) noexcept {
            if (meta->used * 4 >= slots.size() * 3) return false;   // 75% and no growth
            std::size_t i = key & mask;
            for (; slots[i].key; i = (i + 1) & mask)
                if (slots[i].key == key) { slots[i].value = value; return true; }
            slots[i] = { key, value };
            ++meta->used;
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
            --meta->used;
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

        std::size_t size() const noexcept { return meta ? meta->used : 0; }
        std::size_t capacity() const noexcept { return slots.size(); }

    private:
        struct slot { uint64_t key; uint64_t value; };
        header* meta = nullptr;
        std::span<slot> slots;
        std::size_t mask = 0;
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
        struct header { uint64_t high_water; uint32_t free_head; uint32_t padding; };

        void place(index_arena& arena, std::size_t keys, std::size_t total_entries) {
            heads.place(arena, keys);
            meta = arena.take<header>(1).data();
            nodes = arena.take<node>(total_entries + 1);   // index 0 is the end sentinel
        }

        void clear() noexcept {
            heads.clear();
            if (!meta) return;
            *meta = header{ 1, 0, 0 };
            std::memset(nodes.data(), 0, nodes.size() * sizeof(node));
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
                    nodes[current].next = meta->free_head;
                    meta->free_head = current;
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
        struct node { uint64_t value; uint32_t next; uint32_t reserved; };

        uint32_t alloc() noexcept {
            if (meta->free_head) {
                const uint32_t n = meta->free_head;
                meta->free_head = nodes[n].next;
                return n;
            }
            if (meta->high_water >= nodes.size()) return 0;
            return (uint32_t)meta->high_water++;
        }

        flat_hash_map heads;
        header* meta = nullptr;
        std::span<node> nodes;
    };

    // =====================================================================
    // (key, id) pairs kept sorted, so a range is two binary searches and a
    // subtraction.
    //
    // A sorted array rather than a tree. The obvious way to write that is to
    // memmove the tail on every insert, and this class did - until the number
    // was measured. Filling 200,000 products cost 3.3 seconds against 0.3 for
    // the same fill with the other five indexes: n²/2 elements moved, 320 GB of
    // memmove for one bulk load. "An insert into the middle moves the tail" was
    // an honest sentence that nobody would have turned into that number.
    //
    // So inserts land in an unsorted PENDING tail and are merged in batches:
    // one sort of the tail plus one inplace_merge, amortised to O(n log n) for
    // a bulk load, with the sorted array - and therefore the two binary
    // searches a range costs - unchanged. A B-tree would beat this on a write
    // stream that never stops, at the price of a great deal of code that has to
    // be right; this is thirty lines and the same data structure underneath.
    //
    // The catch, stated because it is easy to trip on: a QUERY may settle the
    // pending tail, so the const query methods mutate. This class was never
    // shareable between threads and is not shareable now; nothing about the
    // const on those methods should be read as a promise that it is.
    // =====================================================================
    class flat_ordered_map {
    public:
        struct header { uint64_t count; };

        void place(index_arena& arena, std::size_t entries) {
            limit = entries;
            meta = arena.take<header>(1).data();
            pairs = arena.take<entry>(entries);
        }

        // Clears rather than merely re-placing: index_set::on_restart re-places
        // every index and then rebuilds, so keeping the old count would double
        // every entry instead of replacing it. The hash maps zero their arrays
        // and so were always right; this one was not.
        void clear() noexcept {
            pending.clear();
            if (meta) meta->count = 0;
        }

        bool insert(int64_t key, uint64_t id) {
            if (count() + pending.size() >= limit) return false;
            pending.push_back(entry{ key, id });
            if (pending.size() >= merge_threshold()) settle();
            return true;
        }

        bool erase(int64_t key, uint64_t id) {
            // The pending tail is unsorted and bounded, so it is scanned; an
            // entry removed before it is merged never costs a memmove at all.
            for (std::size_t i = 0; i < pending.size(); ++i) {
                if (pending[i].key != key || pending[i].id != id) continue;
                pending[i] = pending.back();
                pending.pop_back();
                return true;
            }
            for (std::size_t i = lower_bound(key); i < count() && pairs[i].key == key; ++i) {
                if (pairs[i].id != id) continue;
                std::memmove(&pairs[i], &pairs[i + 1], (count() - i - 1) * sizeof(entry));
                --meta->count;
                return true;
            }
            return false;
        }

        // Not noexcept: settling the pending tail sorts and merges, and both
        // are allowed to allocate. Nothing else here can throw.
        std::size_t count_range(int64_t lo, int64_t hi) const {
            settle();
            return upper_bound(hi) - lower_bound(lo);
        }

        void each_range(int64_t lo, int64_t hi, auto&& fn) const {
            settle();
            for (std::size_t i = lower_bound(lo), end = upper_bound(hi); i < end; ++i)
                fn(pairs[i].id, pairs[i].key);
        }

        std::size_t size() const noexcept { return count() + pending.size(); }

        // Everything pending, merged into the array. The engine calls this
        // before it flushes the arena to disk: the pending tail is the one part
        // of this index that lives on the heap, so a flush that did not settle
        // first would write a file missing every insert since the last query.
        void flush_pending() const { settle(); }

    private:
        struct entry { int64_t key; uint64_t id; };

        static bool by_key(const entry& a, const entry& b) noexcept { return a.key < b.key; }

        std::size_t count() const noexcept { return meta ? (std::size_t)meta->count : 0; }

        // Merge when the tail is an eighth of what is already sorted, so the
        // number of merges over a bulk load is logarithmic rather than linear.
        // The floor keeps a small index from merging on nearly every insert.
        std::size_t merge_threshold() const noexcept {
            const std::size_t proportional = count() / 8;
            return proportional < 1024 ? 1024 : proportional;
        }

        void settle() const {
            if (pending.empty()) return;
            std::sort(pending.begin(), pending.end(), by_key);
            const std::size_t sorted_end = count();
            std::memcpy(&pairs[sorted_end], pending.data(), pending.size() * sizeof(entry));
            meta->count = sorted_end + pending.size();
            std::inplace_merge(pairs.data(), pairs.data() + sorted_end, pairs.data() + meta->count, by_key);
            pending.clear();
        }

        std::size_t lower_bound(int64_t key) const noexcept {
            std::size_t lo = 0, hi = count();
            while (lo < hi) { const auto mid = lo + (hi - lo) / 2; if (pairs[mid].key < key) lo = mid + 1; else hi = mid; }
            return lo;
        }
        std::size_t upper_bound(int64_t key) const noexcept {
            std::size_t lo = 0, hi = count();
            while (lo < hi) { const auto mid = lo + (hi - lo) / 2; if (pairs[mid].key <= key) lo = mid + 1; else hi = mid; }
            return lo;
        }

        header* meta = nullptr;
        std::span<entry> pairs;
        // The one thing here that is NOT in the arena. It does not need to be:
        // it is settled before every flush, so a file never contains a pending
        // tail, and a crash that loses it also invalidates the stamp that would
        // have let the file be reused.
        mutable std::vector<entry> pending;
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
    template <auto member, std::size_t capacity_v = 0>
    class unique_index {
    public:
        using owner = member_class_t<member>;
        using key_type = member_value_t<member>;
        static_assert(detail::index_key<key_type>,
                      "an index key must be an integer or an enum - hash the field yourself and index the hash");
        static constexpr auto field = member;
        static constexpr tag_t owner_tag = owner::tag;
        static constexpr std::size_t declared_capacity = capacity_v;
        static uint64_t signature() { return detail::index_signature(1, owner_tag, detail::offset_of<member>(), sizeof(key_type)) ^ (capacity_v * 0x9e3779b97f4a7c15ull); }

        void place(index_arena& arena, std::size_t records) { map.place(arena, capacity_v ? capacity_v : records); }
        void clear() noexcept { map.clear(); }
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

    template <auto member, std::size_t capacity_v = 0, std::size_t keys_v = 0>
    class multi_index {
    public:
        using owner = member_class_t<member>;
        using key_type = member_value_t<member>;
        static_assert(detail::index_key<key_type>,
                      "an index key must be an integer or an enum - hash the field yourself and index the hash");
        static constexpr auto field = member;
        static constexpr tag_t owner_tag = owner::tag;
        static constexpr std::size_t declared_capacity = capacity_v;
        static uint64_t signature() { return detail::index_signature(2, owner_tag, detail::offset_of<member>(), sizeof(key_type)) ^ (capacity_v * 0x9e3779b97f4a7c15ull) ^ (keys_v * 0xff51afd7ed558ccdull); }

        // Two capacities, because a multi index has two: how many ENTRIES it
        // can hold, and how many distinct KEYS those fall under. Sizing the key
        // table to the entry count is the easy mistake and it is expensive -
        // 4,000,000 order lines belong to 1,200,000 orders, and a head table
        // sized for the former is three times the one sized for the latter,
        // holding nothing.
        void place(index_arena& arena, std::size_t records) {
            const std::size_t entries = capacity_v ? capacity_v : records;
            const std::size_t keys = keys_v ? keys_v : entries;
            map.place(arena, keys, entries);
        }
        void clear() noexcept { map.clear(); }
        bool add(record_id id, const owner& value) { return map.insert(detail::key_bits(value.*member), id); }
        void remove(record_id id, const owner& value) { map.erase(detail::key_bits(value.*member), id); }

        void each(key_type key, auto&& fn) const { map.each(detail::key_bits(key), [&](uint64_t id) { fn((record_id)id); }); }
        std::size_t count(key_type key) const noexcept { return map.count(detail::key_bits(key)); }
        bool empty(key_type key) const noexcept { return map.empty(detail::key_bits(key)); }

    private:
        flat_multi_map map;
    };

    template <auto member, std::size_t capacity_v = 0>
    class ordered_index {
    public:
        using owner = member_class_t<member>;
        using key_type = member_value_t<member>;
        static_assert(detail::index_key<key_type>,
                      "an ordered index key must be an integer or an enum");
        static constexpr auto field = member;
        static constexpr tag_t owner_tag = owner::tag;
        static constexpr std::size_t declared_capacity = capacity_v;
        static uint64_t signature() { return detail::index_signature(3, owner_tag, detail::offset_of<member>(), sizeof(key_type)) ^ (capacity_v * 0x9e3779b97f4a7c15ull); }

        void place(index_arena& arena, std::size_t records) { map.place(arena, capacity_v ? capacity_v : records); }
        void clear() noexcept { map.clear(); }
        void flush_pending() const { map.flush_pending(); }
        bool add(record_id id, const owner& value) { return map.insert((int64_t)(value.*member), id); }
        void remove(record_id id, const owner& value) { map.erase((int64_t)(value.*member), id); }

        std::size_t count_range(key_type lo, key_type hi) const {
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
    template <std::size_t records, typename... index_types>
    class index_set {
    public:
        // Capacity is fixed here rather than grown later, deliberately: an index
        // that reallocates under a write is an index whose cost is
        // unpredictable exactly when it matters, and the engine's own index
        // makes the same trade. Sized in records, since that is the number an
        // application actually knows.
        //
        // It is a TEMPLATE parameter rather than a constructor argument, and
        // that is load-bearing rather than a matter of taste: the engine has to
        // map the arena file while it is constructing itself, which is before
        // any caller could have handed it a number. Making the size part of the
        // type means there is no window in which the set exists unsized, and no
        // "did you remember to size it before the first write" to get wrong.
        static constexpr std::size_t expected_records = records;

        index_set() { adopt({}, true); }

        // Spans into an arena are not copyable state - a copy would leave the
        // second object pointing into the first one's buffer, which works right
        // up until the first one dies. The engine holds this by value and never
        // copies it; anything that wanted to would have wanted a second arena.
        index_set(const index_set&) = delete;
        index_set& operator=(const index_set&) = delete;

        // ---- the arena ---------------------------------------------------
        //
        // How many bytes this set needs, computed by laying it out against an
        // arena that has no memory. The engine calls this to size the file.
        //
        // Lays out a bare tuple rather than a whole index_set, because
        // constructing one of those is what calls this.
        // Which index set wrote a file. The arena is a flat block whose
        // meaning is entirely in the layout that produced it: reorder two
        // indexes, change a field's offset, or resize the set, and the same
        // bytes are a different structure that would be read as garbage rather
        // than refused. The engine stamps this alongside the data and declines
        // to believe a file that disagrees.
        static uint64_t layout_id() {
            uint64_t h = 0xcbf29ce484222325ull;
            const auto mix = [&h](uint64_t v) { h ^= v; h *= 0x100000001b3ull; };
            mix(records);
            mix(sizeof...(index_types));
            (mix(index_types::signature()), ...);
            return h;
        }

        static std::size_t arena_bytes() {
            index_arena measuring = index_arena::measuring();
            std::tuple<index_types...> probe;
            std::apply([&](auto&... ix) { (ix.place(measuring, records), ...); }, probe);
            return measuring.used();
        }

        // Point every index at `bytes`. An empty span means the heap buffer
        // this object owns, which is what happens when the engine has no arena
        // file for it - the structures cannot tell the difference.
        //
        // `fresh` is the whole contract with the engine: false says the bytes
        // are a valid index for the log as it stands and must be used as they
        // are, true says they are scratch and must be zeroed. Getting that
        // backwards is either a rebuild that was not needed or an index full of
        // whatever the file used to be, so the engine decides it from a stamp
        // rather than from a guess.
        void adopt(std::span<std::byte> bytes, bool fresh) {
            if (bytes.size() < arena_bytes()) {
                // No arena, or one too small to hold the layout. Either way the
                // answer is the same and it is not "carve what fits": a
                // half-placed index is a set of null pointers waiting for the
                // first lookup. Fall back to a buffer that is the right size by
                // construction, and say so through placed().
                heap.assign(arena_bytes(), std::byte{});
                bytes = heap;
                fresh = true;
                mapped = false;
            } else {
                mapped = true;
            }
            index_arena arena(bytes.data(), bytes.size());
            place_all(arena);
            if (fresh) { std::apply([](auto&... ix) { (ix.clear(), ...); }, indexes); failures = 0; }
        }

        // Everything the indexes keep off the arena, put back on it. Called by
        // the engine before it flushes the mapping - see flat_ordered_map.
        void before_flush() const {
            std::apply([](const auto&... ix) { (flush_one(ix), ...); }, indexes);
        }

        // False when the arena it was handed was too small for the layout,
        // which the engine turns into a refusal to use the file at all.
        bool placed() const noexcept { return mapped; }

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
        // live records rather than by replaying the log.
        void index_existing(record_id id, tag_t what, const std::byte* bytes) {
            std::apply([&](auto&... ix) { (add_if_matching(ix, id, what, bytes), ...); }, indexes);
        }

        // Called by the engine after a checkpoint restore, which is the one
        // path where on_record never fires - the replay is skipped, so without
        // this every index comes back empty and answers "no" to everything.
        //
        // The cost is one pass over the LIVE RECORDS rather than over the log,
        // which is the trade a checkpoint was bought for in the first place:
        // the engine's own state came back for free and this is what is left
        // to pay. bench/app_layer.hpp pays nothing instead by keeping its index
        // in a mapping; that is a further step, not a different answer.
        void on_restart(auto& table) {
            std::apply([](auto&... ix) { (ix.clear(), ...); }, indexes);
            failures = 0;
            table.for_each_raw([&](auto view) { index_existing(view.id, view.tag, view.bytes.data()); });
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

        // The layout, in one place and run twice: once against a measuring
        // arena to size the file, once against the real one to carve it.
        void place_all(index_arena& arena) {
            std::apply([&](auto&... ix) { (ix.place(arena, records), ...); }, indexes);
        }

        // Only an ordered index keeps anything off the arena, so only it has
        // the member - the others pay nothing for a call that does not exist.
        static void flush_one(const auto& ix) {
            if constexpr (requires { ix.flush_pending(); }) ix.flush_pending();
        }

        std::tuple<index_types...> indexes;
        std::vector<std::byte> heap;   // used only when the engine has no arena file
        bool mapped = false;
        std::size_t failures = 0;
    };

} // namespace storage
