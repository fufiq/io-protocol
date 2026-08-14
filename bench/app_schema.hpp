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
// WHAT IS GIVEN UP, and it is real. app_layer keeps its index in a mapping, so
// a restart from a checkpoint costs nothing: the index comes back with the
// engine. Here it is on the heap and a checkpoint restart rebuilds it by
// scanning the live records - which the engine now triggers automatically
// (index_set::on_restart), so it is a cost rather than a bug, but it is still a
// cost. bench/app_schema_bench.cpp measures it, because "some" is not a number.
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
    using schema_indexes = storage::index_set<
        storage::unique_index<&customer::email_hash>,
        storage::unique_index<&product::sku_hash>,
        storage::ordered_index<&product::price>,
        storage::multi_index<&order::buyer>,
        storage::multi_index<&order_line::of>,
        storage::multi_index<&order_line::item>>;

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
