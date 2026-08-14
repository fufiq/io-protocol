#pragma once
// app_schema.hpp - the schema from app_layer.hpp, written against the declared
// index layer instead of a hand-rolled observer.
//
// It exists to answer one question honestly: does storage/indexes.hpp actually
// carry a real schema, or only the one it was written against? Same four record
// types, same six indexes, same queries - so the two files can be compared line
// for line rather than in the abstract.
//
//   app_layer.hpp    285 lines of schema_index: a switch over four tags, a
//                    retract before every add, a resolve for UPDATE, plus the
//                    mapping, its sizing, its commit record and its sweep.
//
//   this file        six declarations.
//
// WHAT IS GAINED BESIDES THE LINES. The foreign keys are types now. app_layer's
// order_line holds two record_ids told apart by a comment; swap them at a call
// site and it compiles. Here they are ref<order> and ref<product>, and the swap
// is an error.
//
// WHAT IT COSTS. Nothing that app_layer.hpp did not also cost. The one thing
// this layer gave up at first was the restart: app_layer keeps its index in a
// mapping, so a checkpoint brings it back with the engine, while this one was on
// the heap and had to be rebuilt by scanning every live record. That gap was
// measured (0.108 s on 3.7M records) and then closed - the indexes live in a
// mapped arena now, stamped by the same checkpoint that stamps the engine's own
// slots, and a restart costs an mmap. bench/app_schema_bench.cpp is where both
// numbers come from, because "some" is not a number.
//
// The transaction layer above is unchanged and unaffected: constraints and
// triggers still live there (app_txn.hpp), and nothing here moved them.

#include "../storage/blob.hpp"
#include "../storage/indexes.hpp"

namespace app2 {

    using storage::record_id;
    using storage::tag_t;

    enum class kind : tag_t { customer = 1, product = 2, order = 3, order_line = 4, text = 5 };

    // A transaction boundary in one bit of record_header::flags - unchanged
    // from app_layer.hpp, and still cheaper than spending a record on it.
    inline constexpr uint8_t txn_commit_flag = 1u << 0;

    using text_ref = storage::blob_ref<(tag_t)kind::text>;

    struct customer;
    struct product;
    struct order;

    struct customer {
        static constexpr tag_t tag = (tag_t)kind::customer;
        uint64_t email_hash;
        int64_t  balance;     // CHECK (balance >= 0), enforced in the txn layer
        uint32_t country;
        uint32_t status;      // 0 active, 1 closed
    };

    struct product {
        static constexpr tag_t tag = (tag_t)kind::product;
        uint64_t sku_hash;
        int64_t  price;
        int64_t  stock;       // CHECK (stock >= 0)
    };

    struct order {
        static constexpr tag_t tag = (tag_t)kind::order;
        storage::ref<customer> buyer;    // FK, ON DELETE RESTRICT
        int64_t                total;
        uint32_t               status;   // 0 new, 1 paid, 2 shipped
        uint32_t               reserved = 0;
    };

    struct order_line {
        static constexpr tag_t tag = (tag_t)kind::order_line;
        storage::ref<order>   of;        // FK, ON DELETE CASCADE
        storage::ref<product> item;      // FK, ON DELETE RESTRICT
        int64_t               qty;       // CHECK (qty > 0)
        int64_t               price;
    };

    static_assert(storage::tagged_aggregate<customer>);
    static_assert(storage::tagged_aggregate<product>);
    static_assert(storage::tagged_aggregate<order>);
    static_assert(storage::tagged_aggregate<order_line>);

    // The layout did not move: a typed reference is the same eight bytes the
    // record_id was, so a log written by app_layer.hpp is readable here.
    static_assert(sizeof(order) == 3 * sizeof(uint64_t));
    static_assert(sizeof(order_line) == 4 * sizeof(uint64_t));

    // =====================================================================
    // Every index the hand-written schema_index had, in the order it had them.
    //
    //   emails             -> unique on customer::email_hash
    //   skus               -> unique on product::sku_hash
    //   prices             -> ordered on product::price      (range queries)
    //   orders_of_customer -> multi on order::buyer          (each_order_of)
    //   lines_of_order     -> multi on order_line::of        (ON DELETE CASCADE)
    //   lines_per_product  -> multi on order_line::item      (ON DELETE RESTRICT)
    // =====================================================================
    // THE NUMBERS ON EACH LINE ARE THE POINT, and leaving them off costs 3x.
    // Every index shares the set's capacity unless it declares its own, so
    // sizing the set for the largest - the two on order_line, one entry per
    // line - gave the index over 500,000 customers room for 4,000,000 of them
    // too. The arena came to 884 MB against a 215 MB log.
    //
    // A multi index takes two numbers because it has two capacities: entries,
    // and the distinct keys they fall under. 4,000,000 lines belong to
    // 1,200,000 orders and to 250,000 products, and a key table sized for the
    // entries rather than the keys is most of what was left. 884 MB -> 288 MB,
    // and now the declaration says what each table actually holds.
    inline constexpr std::size_t max_customers = 600'000;
    inline constexpr std::size_t max_products = 250'000;
    inline constexpr std::size_t max_orders = 1'200'000;
    inline constexpr std::size_t max_lines = 4'000'000;

    using schema_indexes = storage::index_set<max_lines,
        storage::unique_index<&customer::email_hash, max_customers>,
        storage::unique_index<&product::sku_hash, max_products>,
        storage::ordered_index<&product::price, max_products>,
        storage::multi_index<&order::buyer, max_orders, max_customers>,
        storage::multi_index<&order_line::of, max_lines, max_orders>,
        storage::multi_index<&order_line::item, max_lines, max_products>>;

    // ---- the same queries, over the declared indexes --------------------
    //
    // Free functions rather than members, because there is no class left to put
    // them on - which is the point.

    inline record_id customer_by_email(const auto& table, uint64_t email) {
        return table.observer_state().template find<&customer::email_hash>(email);
    }

    inline bool has_email(const auto& table, uint64_t email) {
        return customer_by_email(table, email) != 0;
    }

    inline bool has_sku(const auto& table, uint64_t sku) {
        return table.observer_state().template find<&product::sku_hash>(sku) != 0;
    }

    inline bool customer_has_orders(const auto& table, storage::ref<customer> who) {
        return table.observer_state().template count<&order::buyer>(who) != 0;
    }

    inline void each_order_of(const auto& table, storage::ref<customer> who, auto&& fn) {
        table.observer_state().template each<&order::buyer>(who, fn);
    }

    inline void each_line_of(const auto& table, storage::ref<order> which, auto&& fn) {
        table.observer_state().template each<&order_line::of>(which, fn);
    }

    inline bool product_is_referenced(const auto& table, storage::ref<product> item) {
        return table.observer_state().template count<&order_line::item>(item) != 0;
    }

    inline std::size_t products_in_price_range(const auto& table, int64_t lo, int64_t hi) {
        return table.observer_state().template count_range<&product::price>(lo, hi);
    }

    inline int64_t sum_customer_order_totals(const auto& table, storage::ref<customer> who) {
        int64_t total = 0;
        each_order_of(table, who, [&](record_id id) {
            if (auto rec = table.template get<order>(id)) total += rec->total;
        });
        return total;
    }

} // namespace app2
