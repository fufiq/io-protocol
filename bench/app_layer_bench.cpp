// app_layer_bench.cpp - what a real application schema costs on top of this
// engine, one layer at a time.
//
// The engine gives you records, a WAL, an observer and recovery. It gives you
// no indexes, no constraints, no triggers and no transactions. bench/app_*.hpp
// build all four on top; this measures them, so that "the engine is fast" can
// be separated from "the engine is fast at the thing you would actually do
// with it".
//
// Layers, each adding to the one above it:
//   L0  raw engine            create() straight into the storage
//   L1  + secondary indexes   the observer maintaining 6 indexes
//   L2  + constraints         UNIQUE, FOREIGN KEY, CHECK before every write
//   L3  + triggers            stock and order totals kept correct on insert
//   L4  + transactions        undo log, commit markers, aborts
//
// MEASURED (50k customers, 20k products, 50k orders x 3 lines, NVMe, gcc -O3):
//
//   L0  insert customers, no layer                8493338 ops/sec
//   L0  update balances, no layer                19613673 ops/sec
//   L1  insert customers, 6 indexes               6189067 ops/sec
//   L1  update balances, 6 indexes                4905186 ops/sec
//   L4  insert customers, txn+constraints         4136884 ops/sec
//   L2  rejected by UNIQUE (nothing written)    225463384 ops/sec
//   L3  place+pay order (1 order + 3 lines)        247792 ops/sec
//   L4  abort + undo (8 ops reversed)              214990 ops/sec
//   L4  delete order ON DELETE CASCADE            1654242 ops/sec
//   Q   point lookup via UNIQUE index           148057449 ops/sec
//   Q   same lookup by full scan                      1540 ops/sec
//   Q   FK navigation, orders of customer        29229702 ops/sec
//   Q   range query on ORDERED index                40519 ranges/sec (2020 entries each)
//
//   durable, one fsync per transaction:
//   L4d insert customers                              1107 ops/sec
//   L4d place+pay order                               1030 ops/sec
//
// What the numbers say:
//
// * Six secondary indexes cost 1.37x on insert. Constraints and transactions
//   on top of that bring it to 2.05x. So a full schema - UNIQUE, FOREIGN KEY,
//   CHECK, triggers, undo, commit markers - runs at about half the raw engine,
//   which is the honest price of being a database rather than a heap.
//
// * A place-and-pay order is 247k/sec, and that is 13 records per order:
//   the order, three lines, six trigger updates (stock and running total per
//   line), two payment updates, and the commit marker. Per RECORD that is
//   3.2M/sec - the transaction is not slow, it is thirteen writes.
//
// * A rejected constraint costs nothing at all (225M/sec) because it is a
//   hash lookup that fails before anything is written, and the benchmark
//   asserts the log did not grow by a single byte.
//
// * The point lookup is 96000x faster than the equivalent full scan. That is
//   the entire argument for maintaining indexes in the observer.
//
// * Durability is everything else: 4.1M/sec becomes 1107/sec with one fsync
//   per transaction. At that point nothing above the engine matters - batch
//   transactions or accept ~1ms each.
//
// * RECOVERY IS THE ONE THAT SHOULD WORRY YOU. 44.9 MB replayed in 0.202 s is
//   222 MB/s, against roughly 5.8 GB/s for the same replay with no observer
//   (bench/probe_restart_cost.cpp). Rebuilding the indexes is ~26x the cost of
//   replaying the log, and it dominates startup completely. This is the
//   measurement that argues for persisting indexes and replaying only the
//   delta with for_each_record(), rather than rebuilding from zero each time.
//
// Build:
//   g++ -std=c++23 -O3 -march=native -o app_layer_bench bench/app_layer_bench.cpp
#include "app_txn.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <sys/stat.h>

using namespace app;

namespace {

    constexpr std::size_t n_customers = 50'000;
    constexpr std::size_t n_products = 20'000;
    constexpr std::size_t n_orders = 50'000;
    constexpr std::size_t lines_per_order = 3;

    using plain_spec = storage::storage_spec<storage::durability::local_sync, "applayer_plain.db", 1ull << 30>;
    using app_spec = storage::storage_spec<storage::durability::local_sync, "applayer.db", 1ull << 30>;

    double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }

    struct row {
        std::string layer;
        std::string what;
        std::size_t ops;
        double seconds;
    };
    std::vector<row> results;

    void record(std::string layer, std::string what, std::size_t ops, double s) {
        results.push_back(row{ std::move(layer), std::move(what), ops, s });
        const row& r = results.back();
        std::printf("  %-5s %-34s %9zu ops  %8.3f s  %12.0f ops/sec\n",
                    r.layer.c_str(), r.what.c_str(), r.ops, r.seconds, r.ops / r.seconds);
    }

    void wipe(const char* path) {
        std::remove(path);
        std::remove((std::string(path) + ".data").c_str());
    }

    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };

    // -----------------------------------------------------------------
    // L0 - the engine on its own, so every later number has a floor to be
    // compared against.
    // -----------------------------------------------------------------
    void layer0() {
        wipe(plain_spec::path.value);
        storage::storage<plain_spec> table;
        xorshift rng(1);

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_customers; ++i)
            table.create(customer{ rng.next(), 1'000'000, (uint32_t)(i % 40), 0 });
        auto t1 = std::chrono::steady_clock::now();
        record("L0", "insert customers, no layer", n_customers, secs(t0, t1));

        std::vector<record_id> ids;
        ids.reserve(n_customers);
        table.for_each<customer>([&](record_id id, const customer&) { ids.push_back(id); });

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_customers; ++i)
            table.update<&customer::balance>(ids[rng.range(ids.size())], (int64_t)i);
        t1 = std::chrono::steady_clock::now();
        record("L0", "update balances, no layer", n_customers, secs(t0, t1));
        wipe(plain_spec::path.value);
    }

    // -----------------------------------------------------------------
    // L1 - identical writes, but now the observer maintains every index.
    // The gap against L0 is what index maintenance costs, and nothing else.
    // -----------------------------------------------------------------
    void layer1() {
        wipe(app_spec::path.value);
        table_t<app_spec> table;
        xorshift rng(1);

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_customers; ++i)
            table.create(customer{ rng.next(), 1'000'000, (uint32_t)(i % 40), 0 });
        auto t1 = std::chrono::steady_clock::now();
        record("L1", "insert customers, 6 indexes", n_customers, secs(t0, t1));

        std::vector<record_id> ids;
        ids.reserve(n_customers);
        table.for_each<customer>([&](record_id id, const customer&) { ids.push_back(id); });

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_customers; ++i)
            table.update<&customer::balance>(ids[rng.range(ids.size())], (int64_t)i);
        t1 = std::chrono::steady_clock::now();
        record("L1", "update balances, 6 indexes", n_customers, secs(t0, t1));
        wipe(app_spec::path.value);
    }

    // -----------------------------------------------------------------
    // L2/L3/L4 - the real schema. Built once, then measured from several
    // angles, because tearing it down between phases would measure setup.
    // -----------------------------------------------------------------
    struct world {
        std::vector<record_id> customers;
        std::vector<record_id> products;
        std::vector<record_id> orders;
    };

    world build_catalogue(table_t<app_spec>& table, bool durable) {
        world w;
        xorshift rng(7);
        uint64_t txn_id = 1;

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_customers; ++i) {
            txn<app_spec> tx(table, txn_id++);
            record_id id = tx.insert_customer(customer{ (uint64_t)i + 1, 1'000'000'000, (uint32_t)(i % 40), 0 });
            if (tx.commit(durable) == txn_result::committed) w.customers.push_back(id);
        }
        auto t1 = std::chrono::steady_clock::now();
        record(durable ? "L4d" : "L4", "insert customers, txn+constraints", n_customers, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_products; ++i) {
            txn<app_spec> tx(table, txn_id++);
            record_id id = tx.insert_product(product{ (uint64_t)i + 1, (int64_t)(rng.range(1000) + 1), 1'000'000 });
            if (tx.commit(durable) == txn_result::committed) w.products.push_back(id);
        }
        t1 = std::chrono::steady_clock::now();
        record(durable ? "L4d" : "L4", "insert products, txn+constraints", n_products, secs(t0, t1));
        return w;
    }

    void layer2_uniqueness(table_t<app_spec>& table, const world& w) {
        // Every insert here violates UNIQUE, so this measures the rejection
        // path: the constraint is caught before anything is written.
        uint64_t txn_id = 1'000'000;
        std::size_t rejected = 0;
        constexpr std::size_t n = 1'000'000;
        const std::size_t before = table.current_offset();
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            txn<app_spec> tx(table, txn_id++);
            tx.insert_customer(customer{ (uint64_t)(i % n_customers) + 1, 5, 0, 0 });
            if (tx.commit(false) != txn_result::committed) ++rejected;
        }
        auto t1 = std::chrono::steady_clock::now();
        record("L2", "rejected by UNIQUE (nothing written)", n, secs(t0, t1));
        if (rejected != n) std::printf("  !! expected every insert to be rejected, got %zu\n", rejected);
        if (table.current_offset() != before)
            std::printf("  !! a rejected insert wrote %zu bytes to the log\n", table.current_offset() - before);
        (void)w;
    }

    void layer3_orders(table_t<app_spec>& table, world& w, bool durable) {
        // The full business transaction: one order, three lines, each line
        // checking two foreign keys and stock, then firing two after-triggers.
        xorshift rng(11);
        uint64_t txn_id = 2'000'000;
        std::size_t committed = 0, tx_records = 0;

        const std::size_t log_before = table.current_offset();
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_orders; ++i) {
            txn<app_spec> tx(table, txn_id++);
            record_id customer = w.customers[rng.range(w.customers.size())];
            record_id o = tx.insert_order(customer);
            for (std::size_t l = 0; l < lines_per_order && o; ++l)
                tx.insert_line(o, w.products[rng.range(w.products.size())], (int64_t)(rng.range(3) + 1));
            if (!tx.failed()) tx.pay_order(o);
            tx_records += tx.op_count() + 1; // + the commit marker
            if (tx.commit(durable) == txn_result::committed) { w.orders.push_back(o); ++committed; }
        }
        auto t1 = std::chrono::steady_clock::now();
        record(durable ? "L4d" : "L3", "place+pay order (1+3 lines)", n_orders, secs(t0, t1));
        // 1 order + 3 lines + 6 trigger updates (stock and total per line)
        // + 2 payment updates + 1 commit marker = 13 records per order.
        const std::size_t bytes = table.current_offset() - log_before;
        std::printf("       -> %zu committed, %.1f records and %.0f bytes of log per order\n",
                    committed, committed ? (double)tx_records / committed : 0.0,
                    committed ? (double)bytes / committed : 0.0);
    }

    void layer4_aborts(table_t<app_spec>& table, const world& w) {
        // Deliberately doomed transactions: three lines land, then a fourth
        // asks for more stock than exists, and the undo log has to reverse an
        // insert plus six trigger updates.
        xorshift rng(13);
        uint64_t txn_id = 3'000'000;
        std::size_t aborted = 0;
        constexpr std::size_t n = 20'000;

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            txn<app_spec> tx(table, txn_id++);
            record_id o = tx.insert_order(w.customers[rng.range(w.customers.size())]);
            for (std::size_t l = 0; l < lines_per_order && o; ++l)
                tx.insert_line(o, w.products[rng.range(w.products.size())], 1);
            tx.insert_line(o, w.products[rng.range(w.products.size())], -1); // CHECK (qty > 0)
            if (tx.commit(false) != txn_result::committed) ++aborted;
        }
        auto t1 = std::chrono::steady_clock::now();
        record("L4", "abort + undo (8 ops reversed)", n, secs(t0, t1));
        if (aborted != n) std::printf("  !! expected every transaction to abort, got %zu\n", aborted);
    }

    void layer4_cascade(table_t<app_spec>& table, world& w) {
        constexpr std::size_t n = 10'000;
        if (w.orders.size() < n) return;
        uint64_t txn_id = 4'000'000;
        std::size_t deleted = 0;

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            txn<app_spec> tx(table, txn_id++);
            if (tx.delete_order(w.orders[i]) && tx.commit(false) == txn_result::committed) ++deleted;
        }
        auto t1 = std::chrono::steady_clock::now();
        record("L4", "delete order ON DELETE CASCADE", n, secs(t0, t1));
        if (deleted != n) std::printf("  !! expected %zu cascading deletes, got %zu\n", n, deleted);
        w.orders.erase(w.orders.begin(), w.orders.begin() + n);
    }

    void queries(table_t<app_spec>& table, const world& w) {
        xorshift rng(17);
        constexpr std::size_t n_lookups = 200'000;
        constexpr std::size_t n_scans = 200;

        volatile record_id sink = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_lookups; ++i)
            sink = find_customer_by_email(table, rng.range(n_customers) + 1);
        auto t1 = std::chrono::steady_clock::now();
        record("Q", "point lookup via UNIQUE index", n_lookups, secs(t0, t1));

        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_scans; ++i)
            sink = scan_customer_by_email(table, rng.range(n_customers) + 1);
        t1 = std::chrono::steady_clock::now();
        record("Q", "same lookup by full scan", n_scans, secs(t0, t1));

        // Far fewer iterations, because this one is not a lookup: a
        // std::multimap has bidirectional iterators, so counting a range
        // means walking it. With 20k products over 1000 price points, a
        // 100-wide range holds about 2000 entries - the honest cost of an
        // ordered index that keeps no counts.
        constexpr std::size_t n_ranges = 2'000;
        volatile std::size_t count_sink = 0;
        std::size_t scanned = 0;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_ranges; ++i) {
            int64_t lo = (int64_t)rng.range(900);
            std::size_t c = count_products_in_price_range(table, lo, lo + 100);
            scanned += c;
            count_sink = c;
        }
        t1 = std::chrono::steady_clock::now();
        record("Q", "range query on ORDERED index", n_ranges, secs(t0, t1));
        std::printf("       -> %.0f entries per range, %.0f entries/sec walked\n",
                    (double)scanned / n_ranges, scanned / secs(t0, t1));

        volatile int64_t total_sink = 0;
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_lookups; ++i)
            total_sink = sum_customer_order_totals(table, w.customers[rng.range(w.customers.size())]);
        t1 = std::chrono::steady_clock::now();
        record("Q", "FK navigation, orders of customer", n_lookups, secs(t0, t1));
        (void)sink; (void)count_sink; (void)total_sink;
    }

    // -----------------------------------------------------------------
    // The claim the whole design rests on: indexes rebuilt by replay are the
    // same indexes, and triggers do NOT fire again while it happens.
    // -----------------------------------------------------------------
    void recovery(const table_t<app_spec>& live) {
        const std::size_t live_customers = live.observer_state().customer_by_email.size();
        const std::size_t live_products = live.observer_state().product_by_sku.size();
        const std::size_t live_price_entries = live.observer_state().products_by_price.size();
        const std::size_t live_order_owners = live.observer_state().orders_by_customer.size();
        const std::size_t live_line_owners = live.observer_state().lines_by_order.size();
        const std::size_t live_indexed = live.observer_state().indexed_records;

        // A stock level that only a trigger ever changed - if replay re-fired
        // the triggers, this would come back wrong.
        int64_t live_stock_total = 0;
        live.for_each<product>([&](record_id id, const product& p) {
            if (live.tag_of(id) == product::tag) live_stock_total += p.stock;
        });

        struct ::stat st{};
        ::stat(app_spec::path.value, &st);

        table_t<app_spec> restored;
        auto t0 = std::chrono::steady_clock::now();
        std::size_t replayed = recover_to_last_commit(restored, (std::size_t)st.st_size);
        auto t1 = std::chrono::steady_clock::now();

        int64_t restored_stock_total = 0;
        restored.for_each<product>([&](record_id id, const product& p) {
            if (restored.tag_of(id) == product::tag) restored_stock_total += p.stock;
        });

        const double s = secs(t0, t1);
        std::printf("\n  recovery: replayed %.1f MB in %.3f s  (%.0f MB/s), rebuilding every index\n",
                    replayed / 1048576.0, s, replayed / 1048576.0 / s);

        auto check = [](const char* what, std::size_t a, std::size_t b) {
            std::printf("    %-34s live %9zu   recovered %9zu   %s\n", what, a, b, a == b ? "ok" : "MISMATCH");
        };
        check("UNIQUE customer_by_email", live_customers, restored.observer_state().customer_by_email.size());
        check("UNIQUE product_by_sku", live_products, restored.observer_state().product_by_sku.size());
        check("ORDERED products_by_price", live_price_entries, restored.observer_state().products_by_price.size());
        check("FK orders_by_customer", live_order_owners, restored.observer_state().orders_by_customer.size());
        check("FK lines_by_order", live_line_owners, restored.observer_state().lines_by_order.size());
        check("records indexed", live_indexed, restored.observer_state().indexed_records);
        std::printf("    %-34s live %9lld   recovered %9lld   %s\n", "total stock (triggers only)",
                    (long long)live_stock_total, (long long)restored_stock_total,
                    live_stock_total == restored_stock_total ? "ok - triggers did NOT re-fire" : "MISMATCH");
    }

    // -----------------------------------------------------------------
    // The other half of "transaction": a crash mid-transaction must leave
    // none of it behind, even though its records were already durable.
    // -----------------------------------------------------------------
    void crash_atomicity() {
        wipe(app_spec::path.value);
        record_id survivor = 0, ghost = 0, ghost_product = 0;
        std::size_t log_size = 0;
        {
            table_t<app_spec> table;

            txn<app_spec> good(table, 1);
            survivor = good.insert_customer(customer{ 1, 100, 0, 0 });
            good.commit(true);

            // A second transaction whose records reach the disk - and then the
            // process dies before the commit marker.
            txn<app_spec> doomed(table, 2);
            ghost = doomed.insert_customer(customer{ 2, 200, 0, 0 });
            ghost_product = doomed.insert_product(product{ 1, 5, 5 });
            table.sync();      // its records ARE durable
            doomed.abandon();  // ...but nothing follows them

            log_size = table.current_offset();
        }

        table_t<app_spec> restored;
        std::size_t replayed = recover_to_last_commit(restored, log_size);

        const bool survivor_ok = restored.get<customer>(survivor) != nullptr;
        const bool ghost_gone = restored.get<customer>(ghost) == nullptr;
        const bool ghost_product_gone = restored.get<product>(ghost_product) == nullptr;
        const bool index_clean = restored.observer_state().customer_by_email.count(2) == 0 &&
                                 restored.observer_state().product_by_sku.count(1) == 0;

        std::printf("  %-5s %-34s replayed %zu of %zu bytes\n", "TXN", "crash before commit marker", replayed, log_size);
        std::printf("       committed transaction survived   %s\n", survivor_ok ? "ok" : "MISSING");
        std::printf("       uncommitted records not replayed %s\n", (ghost_gone && ghost_product_gone) ? "ok" : "LEAKED");
        std::printf("       indexes agree with that          %s\n", index_clean ? "ok" : "MISMATCH");
        wipe(app_spec::path.value);
    }

} // namespace

int main() {
    std::printf("customers=%zu products=%zu orders=%zu lines/order=%zu\n\n",
                n_customers, n_products, n_orders, lines_per_order);
    std::printf("  %-5s %-34s %9s      %8s  %12s\n", "layer", "what", "ops", "time", "throughput");
    std::printf("  %s\n", std::string(84, '-').c_str());

    layer0();
    layer1();
    crash_atomicity();

    wipe(app_spec::path.value);
    {
        table_t<app_spec> table;
        world w = build_catalogue(table, false);
        layer2_uniqueness(table, w);
        layer3_orders(table, w, false);
        layer4_aborts(table, w);
        layer4_cascade(table, w);
        queries(table, w);
        table.sync();
        recovery(table);
    }

    std::printf("\n  --- the same work, durable (sync per transaction) ---\n");
    wipe(app_spec::path.value);
    {
        table_t<app_spec> table;
        world w = build_catalogue(table, true);
        layer3_orders(table, w, true);
    }
    wipe(app_spec::path.value);

    std::printf("\n  cost of each layer, customers inserted:\n");
    auto find = [](const char* layer, const char* what) -> const row* {
        for (const row& r : results)
            if (r.layer == layer && r.what.rfind(what, 0) == 0) return &r;
        return nullptr;
    };
    const row* l0 = find("L0", "insert customers");
    const row* l1 = find("L1", "insert customers");
    const row* l4 = find("L4", "insert customers");
    if (l0 && l1 && l4) {
        std::printf("    engine alone                 %12.0f ops/sec\n", l0->ops / l0->seconds);
        std::printf("    + 6 secondary indexes        %12.0f ops/sec   (%.2fx)\n",
                    l1->ops / l1->seconds, (l1->seconds / l1->ops) / (l0->seconds / l0->ops));
        std::printf("    + constraints + transactions %12.0f ops/sec   (%.2fx)\n",
                    l4->ops / l4->seconds, (l4->seconds / l4->ops) / (l0->seconds / l0->ops));
    }
    return 0;
}
