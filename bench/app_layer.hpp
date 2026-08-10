#pragma once
// app_layer.hpp - a small but real application schema built ON TOP of the
// storage engine: secondary indexes, unique/foreign-key/check constraints,
// triggers, and transactions with undo and crash atomicity.
//
// The engine deliberately provides none of that (see HEAP_API.md). The point
// of this file is to find out how much of a database you can actually build
// on the primitives it does provide, and what each layer costs - measured by
// bench/app_layer_bench.cpp.
//
// THE ONE STRUCTURAL RULE, and everything here follows from it:
//
//   Index maintenance goes in the OBSERVER, because the observer is the only
//   thing the engine replays. Constraints and triggers go in the TRANSACTION
//   layer, because they must NOT re-run on replay.
//
// A trigger that decremented stock when an order shipped must not decrement it
// again while the log is being replayed at startup - the decrement is already
// a record in that same log. An index, by contrast, lives only in memory and
// must be rebuilt identically every time. Putting each on the correct side of
// apply() is what makes recovery come out right, and it is the whole reason
// the engine exposes an observer at that exact point rather than a callback
// on create/update/destroy.

#include "../storage/storage.hpp"
#include "flat_index.hpp"

#include <cstring>
#include <fcntl.h>
#include <span>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace app {

    using storage::record_id;
    using storage::tag_t;

    enum class kind : tag_t { customer = 1, product = 2, order = 3, order_line = 4 };

    // A transaction boundary, in one bit of record_header::flags rather than a
    // record of its own. The engine writes those eight bits as zero and never
    // reads them; this layer sets bit 0 on the last record of every committed
    // transaction, and recovery stops after the last record carrying it.
    //
    // It used to be a commit_marker record: 40 bytes plus a full pass through
    // apply() and the observer, on every transaction. For a single-statement
    // transaction that was 40 bytes on top of 48 - the bookkeeping was almost
    // as large as the data.
    constexpr uint8_t txn_commit_flag = 1u << 0;

    // Every type is explicitly padded to satisfy tagged_aggregate - a padding
    // hole is exactly what must not reach a disk or a wire.
    struct customer {
        static constexpr tag_t tag = (tag_t)kind::customer;
        uint64_t email_hash;
        int64_t  balance;    // CHECK (balance >= 0)
        uint32_t country;
        uint32_t status;     // 0 active, 1 closed
    };

    struct product {
        static constexpr tag_t tag = (tag_t)kind::product;
        uint64_t sku_hash;
        int64_t  price;
        int64_t  stock;      // CHECK (stock >= 0)
    };

    struct order {
        static constexpr tag_t tag = (tag_t)kind::order;
        record_id customer_id;   // FK -> customer, ON DELETE RESTRICT
        int64_t   total;
        uint32_t  status;        // 0 new, 1 paid, 2 shipped
        uint32_t  reserved;
    };

    struct order_line {
        static constexpr tag_t tag = (tag_t)kind::order_line;
        record_id order_id;      // FK -> order, ON DELETE CASCADE
        record_id product_id;    // FK -> product, ON DELETE RESTRICT
        int64_t   qty;           // CHECK (qty > 0)
        int64_t   price;
    };

    static_assert(storage::tagged_aggregate<customer>);
    static_assert(storage::tagged_aggregate<product>);
    static_assert(storage::tagged_aggregate<order>);
    static_assert(storage::tagged_aggregate<order_line>);

    // =====================================================================
    // The observer: every secondary index, and nothing else.
    //
    // It runs before each record is applied, so the storage still holds the
    // pre-record state - which is what lets it retract the old entry before
    // adding the new one. It is called identically by a live write, by
    // recover(), and by replication ingest, so the indexes a replica holds
    // and the indexes rebuilt at startup are the same indexes by construction
    // rather than by discipline.
    //
    // Every structure lives in ONE MAPPING (see flat_index.hpp), not on the
    // heap. That is what lets the indexes survive a restart: the engine's
    // checkpoint brings its own state back without replaying the log, and
    // msync'ing this region brings the indexes back with it. With STL
    // containers there was nothing to msync - a std::unordered_map is a web of
    // allocator pointers, and the next run would have had to rebuild it from
    // live records every time.
    // =====================================================================
    struct index_header {
        uint64_t magic;
        uint64_t capacity;
        std::size_t emails, skus, lines_per_product_count, prices, indexed;
        flat::list_state orders_of_customer, lines_of_order;
    };
    inline constexpr uint64_t index_magic = 0x5844495f414d4548ull; // "HEMA_IDX"

    class schema_index {
    public:
        // Bytes a region must have to index `capacity` ids. Everything is
        // sized off that one number, and nothing grows afterwards.
        static std::size_t bytes_needed(std::size_t capacity) noexcept {
            const std::size_t buckets = flat::round_pow2(capacity * 2);
            std::size_t n = flat::align_up(sizeof(index_header), 64);
            n += 3 * buckets * sizeof(flat::hash_slot);
            n += 2 * capacity * sizeof(uint32_t);
            n += 2 * capacity * sizeof(flat::list_node);
            n += capacity * sizeof(flat::ordered_entry);
            return flat::align_up(n, 4096);
        }

        // Lays the structures out over `base`. A region whose header already
        // carries the right magic and capacity is adopted AS IS - that is the
        // restart path, and the reason none of this can be an STL container.
        bool attach(void* base, std::size_t bytes, std::size_t capacity) noexcept {
            if (bytes < bytes_needed(capacity)) return false;
            std::byte* p = (std::byte*)base;
            head = (index_header*)p;

            const bool fresh = (head->magic != index_magic || head->capacity != capacity);
            if (fresh) {
                std::memset(head, 0, sizeof(index_header));
                head->magic = index_magic;
                head->capacity = capacity;
            }

            const std::size_t buckets = flat::round_pow2(capacity * 2);
            p += flat::align_up(sizeof(index_header), 64);
            auto take = [&](std::size_t n) { std::byte* at = p; p += n; return at; };

            flat::hash_slot* email_slots = (flat::hash_slot*)take(buckets * sizeof(flat::hash_slot));
            flat::hash_slot* sku_slots = (flat::hash_slot*)take(buckets * sizeof(flat::hash_slot));
            flat::hash_slot* lpp_slots = (flat::hash_slot*)take(buckets * sizeof(flat::hash_slot));
            uint32_t* order_heads = (uint32_t*)take(capacity * sizeof(uint32_t));
            uint32_t* line_heads = (uint32_t*)take(capacity * sizeof(uint32_t));
            flat::list_node* order_nodes = (flat::list_node*)take(capacity * sizeof(flat::list_node));
            flat::list_node* line_nodes = (flat::list_node*)take(capacity * sizeof(flat::list_node));
            flat::ordered_entry* price_entries = (flat::ordered_entry*)take(capacity * sizeof(flat::ordered_entry));

            if (fresh) {
                std::memset(email_slots, 0, buckets * sizeof(flat::hash_slot));
                std::memset(sku_slots, 0, buckets * sizeof(flat::hash_slot));
                std::memset(lpp_slots, 0, buckets * sizeof(flat::hash_slot));
                std::memset(order_heads, 0, capacity * sizeof(uint32_t));
                std::memset(line_heads, 0, capacity * sizeof(uint32_t));
            }

            emails.attach(email_slots, buckets, &head->emails);
            skus.attach(sku_slots, buckets, &head->skus);
            lines_per_product.attach(lpp_slots, buckets, &head->lines_per_product_count);
            orders_of_customer.attach(order_heads, capacity, order_nodes, capacity, &head->orders_of_customer);
            lines_of_order.attach(line_heads, capacity, line_nodes, capacity, &head->lines_of_order);
            prices.attach(price_entries, capacity, &head->prices);
            return true;
        }

        bool attached() const noexcept { return head != nullptr; }

        void on_record(auto& table, const storage::record_header& hdr, const std::byte* payload) {
            if (!head) return;
            // Retract whatever this id currently contributes. tag_of() is how
            // the kind is known at all: an UPDATE or DESTROY record names an
            // id and a byte range, never a type.
            const tag_t existing = table.tag_of(hdr.id);
            if (existing) {
                retract(table, hdr.id, existing);
                --head->indexed;
            }
            if (hdr.op == storage::record_op::destroy) return;

            // A CREATE names its type; an UPDATE carries tag 0, so the kind
            // comes from the record already in place.
            const tag_t what = (hdr.op == storage::record_op::create) ? hdr.tag : existing;
            if (!what) return;

            switch ((kind)what) {
                case kind::customer:   add(hdr.id, resolve<customer>(table, hdr, payload)); break;
                case kind::product:    add(hdr.id, resolve<product>(table, hdr, payload)); break;
                case kind::order:      add(hdr.id, resolve<order>(table, hdr, payload)); break;
                case kind::order_line: add(hdr.id, resolve<order_line>(table, hdr, payload)); break;
            }
            ++head->indexed;
        }

        // Indexes a record that already exists, for rebuilding from live
        // records rather than from the log - the fallback when there is no
        // checkpoint to restore the mapping from.
        void index_existing(record_id id, tag_t what, const std::byte* bytes) {
            switch ((kind)what) {
                case kind::customer:   { customer v; std::memcpy(&v, bytes, sizeof(v)); add(id, v); break; }
                case kind::product:    { product v; std::memcpy(&v, bytes, sizeof(v)); add(id, v); break; }
                case kind::order:      { order v; std::memcpy(&v, bytes, sizeof(v)); add(id, v); break; }
                case kind::order_line: { order_line v; std::memcpy(&v, bytes, sizeof(v)); add(id, v); break; }
            }
            ++head->indexed;
        }

        // ---- queries ----------------------------------------------------
        record_id customer_by_email(uint64_t email) const noexcept {
            const uint64_t* v = emails.find(email);
            return v ? (record_id)*v : 0;
        }
        bool has_email(uint64_t email) const noexcept { return emails.contains(email); }
        bool has_sku(uint64_t sku) const noexcept { return skus.contains(sku); }
        bool customer_has_orders(record_id id) const noexcept { return !orders_of_customer.empty(id); }
        template <typename fn> void each_order_of(record_id id, fn&& f) const { orders_of_customer.for_each(id, f); }
        template <typename fn> void each_line_of(record_id id, fn&& f) const { lines_of_order.for_each(id, f); }
        std::size_t products_in_price_range(int64_t lo, int64_t hi) const noexcept { return prices.count_range(lo, hi); }

        // ---- what the benchmark compares before and after a restart -----
        std::size_t emails_indexed() const noexcept { return head ? head->emails : 0; }
        std::size_t skus_indexed() const noexcept { return head ? head->skus : 0; }
        std::size_t price_entries() const noexcept { return head ? head->prices : 0; }
        std::size_t products_referenced() const noexcept { return head ? head->lines_per_product_count : 0; }
        std::size_t indexed_records() const noexcept { return head ? head->indexed : 0; }
        std::size_t customers_with_orders() const noexcept { return count_nonempty(orders_of_customer); }
        std::size_t orders_with_lines() const noexcept { return count_nonempty(lines_of_order); }

    private:
        // The value the record WILL have. A create carries it whole; an
        // update carries only a diff, so the current record supplies the rest.
        template <typename T>
        static T resolve(auto& table, const storage::record_header& hdr, const std::byte* payload) {
            T value{};
            if (hdr.op == storage::record_op::create) {
                std::memcpy(&value, payload, sizeof(T));
            } else {
                value = *table.template get<T>(hdr.id);
                std::memcpy((std::byte*)&value + hdr.offset, payload, hdr.len);
            }
            return value;
        }

        void add(record_id id, const customer& c) { emails.insert(c.email_hash, id); }
        void add(record_id id, const product& p) {
            skus.insert(p.sku_hash, id);
            prices.insert(p.price, id);
        }
        void add(record_id id, const order& o) { orders_of_customer.push(o.customer_id, id); }
        void add(record_id id, const order_line& l) {
            lines_of_order.push(l.order_id, id);
            const uint64_t* n = lines_per_product.find(l.product_id);
            lines_per_product.insert(l.product_id, n ? *n + 1 : 1);
        }

        void retract(auto& table, record_id id, tag_t what) {
            switch ((kind)what) {
                case kind::customer:
                    emails.erase(table.template get<customer>(id)->email_hash);
                    break;
                case kind::product: {
                    // auto, not const product* - this file is compiled against
                    // both engines, and get() hands back a pointer from one
                    // and an optional from the other. Both spell -> the same.
                    auto p = table.template get<product>(id);
                    skus.erase(p->sku_hash);
                    prices.erase(p->price, id);
                    break;
                }
                case kind::order:
                    orders_of_customer.erase(table.template get<order>(id)->customer_id, id);
                    break;
                case kind::order_line: {
                    auto l = table.template get<order_line>(id);
                    lines_of_order.erase(l->order_id, id);
                    const uint64_t* n = lines_per_product.find(l->product_id);
                    if (n && *n > 1) lines_per_product.insert(l->product_id, *n - 1);
                    else if (n) lines_per_product.erase(l->product_id);
                    break;
                }
            }
        }

        std::size_t count_nonempty(const flat::multi_list& list) const noexcept {
            if (!head) return 0;
            std::size_t n = 0;
            for (std::size_t id = 0; id < head->capacity; ++id)
                if (!list.empty(id)) ++n;
            return n;
        }

        index_header* head = nullptr;
        flat::hash_map emails, skus, lines_per_product;
        flat::multi_list orders_of_customer, lines_of_order;
        flat::ordered_array prices;
    };

    // =====================================================================
    // Owns the file the indexes live in, so they can be msync'd alongside the
    // engine's own checkpoint and mapped straight back on the next run.
    // =====================================================================
    class index_store {
    public:
        index_store() = default;
        ~index_store() { close(); }
        index_store(const index_store&) = delete;
        index_store& operator=(const index_store&) = delete;

        bool open(const char* path, std::size_t capacity) noexcept {
            close();
            bytes = schema_index::bytes_needed(capacity);
            fd = ::open(path, O_RDWR | O_CREAT, 0644);
            if (fd < 0) return false;
            if (::posix_fallocate(fd, 0, (off_t)bytes) != 0) { close(); return false; }
            void* m = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m == MAP_FAILED) { close(); return false; }
            base = m;
            return true;
        }

        void sync() noexcept { if (base) ::msync(base, bytes, MS_SYNC); }
        void* region() const noexcept { return base; }
        std::size_t region_bytes() const noexcept { return bytes; }

        void close() noexcept {
            if (base) { ::munmap(base, bytes); base = nullptr; }
            if (fd >= 0) { ::close(fd); fd = -1; }
            bytes = 0;
        }

    private:
        void* base = nullptr;
        std::size_t bytes = 0;
        int fd = -1;
    };

} // namespace app
