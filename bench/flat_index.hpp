#pragma once
// flat_index.hpp - secondary index structures that live in a mapping instead
// of on the heap.
//
// std::unordered_map and std::multimap cannot be memory-mapped: they are webs
// of pointers into the allocator, and a pointer written by one run means
// nothing to the next. Every structure here is instead a flat array of PODs
// addressed by index, laid out over a caller-supplied region. That makes them
// mmap-able, which makes them survive a restart, which is the whole point:
// a checkpoint can bring the engine back without replaying the log, but the
// indexes above it were still being rebuilt from scratch every time.
//
// Nothing here allocates. Capacities are fixed at attach() and an insert past
// them fails rather than growing, in the same spirit as the engine's
// file-backed record index.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace flat {

    // =====================================================================
    // Open-addressed hash map, linear probing. Key 0 means "empty", which
    // costs nothing here because every key this schema uses - a record id, an
    // email hash, a sku - is issued from 1.
    //
    // Deletion is backward-shift rather than tombstones: tombstones would
    // accumulate across a long-lived index and slowly turn every lookup into
    // a scan, and this index is meant to survive restarts, so "long-lived" is
    // the normal case.
    // =====================================================================
    struct hash_slot { uint64_t key; uint64_t value; };

    class hash_map {
    public:
        void attach(hash_slot* slots, std::size_t capacity, std::size_t* count) noexcept {
            table = slots;
            mask = capacity - 1; // capacity is a power of two
            used = count;
        }

        bool insert(uint64_t key, uint64_t value) noexcept {
            std::size_t i = probe(key);
            for (std::size_t step = 0; step <= mask; ++step, i = (i + 1) & mask) {
                if (table[i].key == key) { table[i].value = value; return true; }
                if (table[i].key == 0) { table[i] = { key, value }; ++*used; return true; }
            }
            return false; // full
        }

        const uint64_t* find(uint64_t key) const noexcept {
            std::size_t i = probe(key);
            for (std::size_t step = 0; step <= mask; ++step, i = (i + 1) & mask) {
                if (table[i].key == key) return &table[i].value;
                if (table[i].key == 0) return nullptr;
            }
            return nullptr;
        }

        bool contains(uint64_t key) const noexcept { return find(key) != nullptr; }

        void erase(uint64_t key) noexcept {
            std::size_t i = probe(key);
            for (std::size_t step = 0; step <= mask; ++step, i = (i + 1) & mask) {
                if (table[i].key == 0) return;
                if (table[i].key == key) break;
                if (step == mask) return;
            }
            // Backward shift: walk the cluster and pull back anything whose
            // ideal slot is at or before the hole, so no probe ever stops
            // early at a gap that should not be there.
            std::size_t hole = i;
            std::size_t j = (i + 1) & mask;
            --*used;
            while (table[j].key != 0) {
                const std::size_t ideal = probe(table[j].key);
                const bool wraps = (j < hole);
                const bool ideal_before_hole = wraps ? (ideal <= hole || ideal > j) : (ideal <= hole && ideal <= j);
                if (ideal_before_hole) { table[hole] = table[j]; hole = j; }
                j = (j + 1) & mask;
            }
            table[hole] = { 0, 0 };
        }

        std::size_t size() const noexcept { return *used; }

        // Walks every occupied slot. Needed by the restart path: an index
        // mapping is a SUPERSET of the state its header describes, because the
        // kernel writes mapped pages back on its own schedule, so entries can
        // survive for records the log no longer has. Only a sweep can find
        // them - they are not reachable from anything.
        template <typename fn>
        void for_each(fn&& f) const noexcept {
            for (std::size_t i = 0; i <= mask; ++i)
                if (table[i].key) f(table[i].key, table[i].value);
        }

        // Drops every entry the predicate rejects and recounts what is left.
        // Recounting rather than adjusting, because the stored count and the
        // slots can disagree by an unknown amount after a crash.
        template <typename pred>
        void retain(pred&& keep) noexcept {
            std::vector<uint64_t> doomed;
            for (std::size_t i = 0; i <= mask; ++i)
                if (table[i].key && !keep(table[i].key, table[i].value)) doomed.push_back(table[i].key);
            for (uint64_t k : doomed) erase(k);
            std::size_t n = 0;
            for (std::size_t i = 0; i <= mask; ++i) n += table[i].key != 0;
            *used = n;
        }

    private:
        std::size_t probe(uint64_t key) const noexcept {
            // splitmix64 finalizer - the keys here are dense counters, and a
            // weak hash would pile them into one cluster.
            uint64_t h = key;
            h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
            h ^= h >> 27; h *= 0x94d049bb133111ebull;
            h ^= h >> 31;
            return (std::size_t)h & mask;
        }
        hash_slot* table = nullptr;
        std::size_t mask = 0;
        std::size_t* used = nullptr;
    };

    // =====================================================================
    // id -> list of ids: a head array indexed directly by the key, plus a
    // node arena with a free list. Node index 0 is the end-of-list sentinel,
    // so the arena is used from 1.
    //
    // This is what replaces unordered_map<id, vector<id>>: one vector per key
    // would be one heap allocation per key, and neither the map nor the
    // vectors could be mapped.
    // =====================================================================
    struct list_node { uint64_t value; uint32_t next; uint32_t pad; };

    struct list_state { uint32_t free_head; uint32_t high_water; };

    class multi_list {
    public:
        void attach(uint32_t* head_array, std::size_t head_count,
                    list_node* node_array, std::size_t node_count, list_state* st) noexcept {
            heads = head_array;
            heads_len = head_count;
            nodes = node_array;
            nodes_len = node_count;
            state = st;
            if (state->high_water == 0) state->high_water = 1; // 0 is the sentinel
        }

        bool push(uint64_t key, uint64_t value) noexcept {
            if (key >= heads_len) return false;
            const uint32_t n = alloc();
            if (!n) return false;
            nodes[n].value = value;
            nodes[n].next = heads[key];
            heads[key] = n;
            return true;
        }

        bool erase(uint64_t key, uint64_t value) noexcept {
            if (key >= heads_len) return false;
            uint32_t* link = &heads[key];
            while (*link) {
                list_node& node = nodes[*link];
                if (node.value == value) {
                    const uint32_t dead = *link;
                    *link = node.next;
                    node.next = state->free_head;
                    state->free_head = dead;
                    return true;
                }
                link = &node.next;
            }
            return false;
        }

        bool empty(uint64_t key) const noexcept { return key >= heads_len || heads[key] == 0; }

        template <typename fn>
        void for_each(uint64_t key, fn&& f) const noexcept {
            if (key >= heads_len) return;
            for (uint32_t n = heads[key]; n; n = nodes[n].next) f(nodes[n].value);
        }

        std::size_t count(uint64_t key) const noexcept {
            std::size_t n = 0;
            for_each(key, [&](uint64_t) { ++n; });
            return n;
        }

    private:
        uint32_t alloc() noexcept {
            if (state->free_head) {
                const uint32_t n = state->free_head;
                state->free_head = nodes[n].next;
                return n;
            }
            if (state->high_water >= nodes_len) return 0;
            return state->high_water++;
        }

        uint32_t* heads = nullptr;
        std::size_t heads_len = 0;
        list_node* nodes = nullptr;
        std::size_t nodes_len = 0;
        list_state* state = nullptr;
    };

    // =====================================================================
    // An ORDERED index: (key, id) pairs kept sorted by key in one array, so a
    // range is a pair of binary searches and the answer is a subtraction.
    //
    // A sorted array, not a tree, on purpose. A B-tree in an arena would give
    // O(log n) insert instead of this O(n) memmove, but it is a great deal of
    // code to get right, and the trade only pays when insertions are frequent
    // - an ordered index over something like a price is typically written far
    // less often than it is queried. The cost is stated rather than hidden:
    // inserting into the middle of n entries moves the tail.
    // =====================================================================
    struct ordered_entry { int64_t key; uint64_t id; };

    class ordered_array {
    public:
        void attach(ordered_entry* array, std::size_t capacity, std::size_t* count) noexcept {
            entries = array;
            cap = capacity;
            used = count;
        }

        bool insert(int64_t key, uint64_t id) noexcept {
            if (*used >= cap) return false;
            const std::size_t at = lower_bound(key);
            std::memmove(entries + at + 1, entries + at, (*used - at) * sizeof(ordered_entry));
            entries[at] = { key, id };
            ++*used;
            return true;
        }

        bool erase(int64_t key, uint64_t id) noexcept {
            for (std::size_t i = lower_bound(key); i < *used && entries[i].key == key; ++i) {
                if (entries[i].id != id) continue;
                std::memmove(entries + i, entries + i + 1, (*used - i - 1) * sizeof(ordered_entry));
                --*used;
                return true;
            }
            return false;
        }

        std::size_t count_range(int64_t lo, int64_t hi) const noexcept {
            return upper_bound(hi) - lower_bound(lo);
        }

        std::size_t size() const noexcept { return *used; }

    private:
        std::size_t lower_bound(int64_t key) const noexcept {
            std::size_t lo = 0, hi = *used;
            while (lo < hi) {
                const std::size_t mid = lo + (hi - lo) / 2;
                if (entries[mid].key < key) lo = mid + 1; else hi = mid;
            }
            return lo;
        }
        std::size_t upper_bound(int64_t key) const noexcept {
            std::size_t lo = 0, hi = *used;
            while (lo < hi) {
                const std::size_t mid = lo + (hi - lo) / 2;
                if (entries[mid].key <= key) lo = mid + 1; else hi = mid;
            }
            return lo;
        }
        ordered_entry* entries = nullptr;
        std::size_t cap = 0;
        std::size_t* used = nullptr;
    };

    // Rounds up to a power of two, which the hash map's mask depends on.
    inline constexpr std::size_t round_pow2(std::size_t n) noexcept {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    inline constexpr std::size_t align_up(std::size_t n, std::size_t to) noexcept {
        return (n + to - 1) & ~(to - 1);
    }

} // namespace flat
