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

#include <cstring>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

namespace app {

    using storage::record_id;
    using storage::tag_t;

    enum class kind : tag_t { customer = 1, product = 2, order = 3, order_line = 4, commit = 5 };

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

    // Written as the last record of every committed transaction. Recovery
    // stops at the last one, which is what makes a transaction atomic across
    // a crash: a half-written transaction has no marker after it, so none of
    // it is replayed.
    struct commit_marker {
        static constexpr tag_t tag = (tag_t)kind::commit;
        uint64_t txn_id;
        uint64_t op_count;
    };

    static_assert(storage::tagged_aggregate<customer>);
    static_assert(storage::tagged_aggregate<product>);
    static_assert(storage::tagged_aggregate<order>);
    static_assert(storage::tagged_aggregate<order_line>);
    static_assert(storage::tagged_aggregate<commit_marker>);

    // =====================================================================
    // The observer: every secondary index, and nothing else.
    //
    // It runs before each record is applied, so the storage still holds the
    // pre-record state - which is what lets it retract the old entry before
    // adding the new one. It is called identically by a live write, by
    // recover(), and by replication ingest, so the indexes a replica holds
    // and the indexes rebuilt at startup are the same indexes by
    // construction rather than by discipline.
    // =====================================================================
    struct schema_index {
        // UNIQUE
        std::unordered_map<uint64_t, record_id> customer_by_email;
        std::unordered_map<uint64_t, record_id> product_by_sku;
        // FOREIGN KEY navigation, and the reference counts that make
        // ON DELETE RESTRICT answerable without a scan
        std::unordered_map<record_id, std::vector<record_id>> orders_by_customer;
        std::unordered_map<record_id, std::vector<record_id>> lines_by_order;
        std::unordered_map<record_id, uint32_t> lines_per_product;
        // ORDERED, for range queries
        std::multimap<int64_t, record_id> products_by_price;

        std::size_t indexed_records = 0;

        void on_record(auto& table, const storage::record_header& hdr, const std::byte* payload) {
            // Retract whatever this id currently contributes. tag_of() is how
            // the kind is known at all: an UPDATE or DESTROY record names an
            // id and a byte range, never a type.
            const tag_t existing = table.tag_of(hdr.id);
            if (existing) {
                retract(table, hdr.id, existing);
                --indexed_records;
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
                case kind::commit:     return; // a marker indexes nothing
            }
            ++indexed_records;
        }

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

        void add(record_id id, const customer& c) { customer_by_email[c.email_hash] = id; }
        void add(record_id id, const product& p) {
            product_by_sku[p.sku_hash] = id;
            products_by_price.emplace(p.price, id);
        }
        void add(record_id id, const order& o) { orders_by_customer[o.customer_id].push_back(id); }
        void add(record_id id, const order_line& l) {
            lines_by_order[l.order_id].push_back(id);
            ++lines_per_product[l.product_id];
        }

        void retract(auto& table, record_id id, tag_t what) {
            switch ((kind)what) {
                case kind::customer:
                    customer_by_email.erase(table.template get<customer>(id)->email_hash);
                    break;
                case kind::product: {
                    const product* p = table.template get<product>(id);
                    product_by_sku.erase(p->sku_hash);
                    erase_pair(products_by_price, p->price, id);
                    break;
                }
                case kind::order:
                    erase_from(orders_by_customer, table.template get<order>(id)->customer_id, id);
                    break;
                case kind::order_line: {
                    const order_line* l = table.template get<order_line>(id);
                    erase_from(lines_by_order, l->order_id, id);
                    if (auto it = lines_per_product.find(l->product_id); it != lines_per_product.end() && it->second)
                        --it->second;
                    break;
                }
                case kind::commit: break;
            }
        }

        static void erase_pair(std::multimap<int64_t, record_id>& m, int64_t key, record_id id) {
            auto [lo, hi] = m.equal_range(key);
            for (auto it = lo; it != hi; ++it)
                if (it->second == id) { m.erase(it); return; }
        }
        static void erase_from(std::unordered_map<record_id, std::vector<record_id>>& m, record_id key, record_id id) {
            auto it = m.find(key);
            if (it == m.end()) return;
            auto& v = it->second;
            for (std::size_t i = 0; i < v.size(); ++i)
                if (v[i] == id) { v[i] = v.back(); v.pop_back(); break; }
            if (v.empty()) m.erase(it);
        }
    };

} // namespace app
