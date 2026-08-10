#pragma once
// app_txn.hpp - constraints, triggers and transactions over app_layer.hpp.
//
// Everything here sits ABOVE apply(), which is the whole design: none of it
// re-runs when the log is replayed. A trigger that decremented stock when a
// line was inserted must not decrement it again at startup - that decrement is
// itself a record in the same log, and replaying the log replays it. Only the
// observer (index maintenance) belongs below apply(); constraints and triggers
// belong here.
//
// TRANSACTIONS ARE STAGED, NOT UNDONE.
//
// Nothing a transaction does reaches the log until it commits. Statements
// write into an in-memory overlay, constraints are checked against the
// committed index plus that overlay, and commit() appends the whole batch in
// one contiguous run and sets the boundary flag on its last record.
//
// The alternative - apply eagerly and keep an undo log to reverse on abort -
// is what this file used to do, and it is worse in four ways that are easy to
// state:
//
//   * an aborted transaction wrote records. Its compensating writes were real
//     log entries, so a workload that aborts often paid full price for work
//     that was thrown away, and the log carried both halves forever.
//   * the log interleaved uncommitted state with committed state, so anything
//     reading it - recovery, replication, a tool - saw writes that never
//     happened and had to reason about which were later reversed.
//   * atomicity leaned on the reversal actually running. A crash mid-abort
//     left it half-applied.
//   * every statement paid to record how to undo itself, whether or not the
//     transaction would ever abort.
//
// Staging costs the opposite thing: the batch is held in memory until commit,
// and reads inside the transaction consult the overlay first. That is bounded
// by the size of one transaction, which is the right thing to bound.
//
// Crash atomicity then needs nothing extra. Records reach the log only at
// commit and the last of them carries txn_commit_flag, so a crash mid-flush
// leaves a run of records with no flag after them, and
// recover_to_last_commit() stops before it.

#include "app_layer.hpp"

#include <cstdio>
#include <optional>
#include <string>

namespace app {

    template <typename spec>
    using table_t = storage::storage<spec, schema_index>;

    // How a transaction ended, so a benchmark can count outcomes rather than
    // guess at them.
    enum class txn_result { committed, aborted_constraint, aborted_by_caller };

    // The widest record in the schema; every staged value fits in one of these.
    inline constexpr std::size_t max_record_bytes = sizeof(order_line);

    template <typename spec>
    class txn {
    public:
        explicit txn(table_t<spec>& table, uint64_t id) : t(table), txn_id(id) {}
        ~txn() = default; // an unflushed transaction simply never happened

        txn(const txn&) = delete;
        txn& operator=(const txn&) = delete;

        // ---- statements -------------------------------------------------
        // Each returns 0 / false on a constraint violation and marks the
        // transaction failed. Nothing has been written either way.

        record_id insert_customer(const customer& c) {
            if (c.balance < 0) return fail();                       // CHECK
            if (email_taken(c.email_hash)) return fail();           // UNIQUE
            return stage_create(customer::tag, &c, sizeof(c));
        }

        record_id insert_product(const product& p) {
            if (p.stock < 0 || p.price < 0) return fail();          // CHECK
            if (sku_taken(p.sku_hash)) return fail();               // UNIQUE
            return stage_create(product::tag, &p, sizeof(p));
        }

        record_id insert_order(record_id customer_id) {
            const auto c = view<customer>(customer_id);
            if (!c) return fail();                                  // FOREIGN KEY
            if (c->status != 0) return fail();                      // BEFORE INSERT trigger
            const order o{ customer_id, 0, 0, 0 };
            return stage_create(order::tag, &o, sizeof(o));
        }

        // Three constraints and two after-triggers, all against the overlay:
        // the order referenced here was very likely created by this same
        // transaction and does not exist in the storage yet.
        record_id insert_line(record_id order_id, record_id product_id, int64_t qty) {
            if (qty <= 0) return fail();                            // CHECK
            const auto o = view<order>(order_id);
            if (!o) return fail();                                  // FOREIGN KEY
            const auto p = view<product>(product_id);
            if (!p) return fail();                                  // FOREIGN KEY
            if (o->status != 0) return fail();                      // BEFORE INSERT trigger
            if (p->stock < qty) return fail();                      // BEFORE INSERT trigger

            const int64_t price = p->price;
            const int64_t new_stock = p->stock - qty;
            const int64_t new_total = o->total + price * qty;
            const order_line l{ order_id, product_id, qty, price };
            record_id line = stage_create(order_line::tag, &l, sizeof(l));
            if (!line) return 0;

            // AFTER INSERT triggers.
            if (!update<&product::stock>(product_id, new_stock)) return 0;
            if (!update<&order::total>(order_id, new_total)) return 0;
            return line;
        }

        bool pay_order(record_id order_id) {
            const auto o = view<order>(order_id);
            if (!o || o->status != 0) return fail_bool();
            const auto c = view<customer>(o->customer_id);
            if (!c) return fail_bool();
            if (c->balance < o->total) return fail_bool();          // CHECK (balance >= 0)

            const record_id owner = o->customer_id;
            const int64_t remaining = c->balance - o->total;
            if (!update<&customer::balance>(owner, remaining)) return false;
            return update<&order::status>(order_id, 1);             // AFTER UPDATE trigger
        }

        // A field update as a statement. The diff is staged, and the overlay
        // is patched so later statements in the same transaction see it.
        template <auto member>
        bool update(record_id id, storage::member_value_t<member> value) {
            using owner = storage::member_class_t<member>;
            slot* s = resolve(id);   // may grow the overlay; nothing older is held across it
            if (!s || s->dead || s->tag != owner::tag) return fail_bool();

            // The member's offset within its class, computed the same way the
            // engine does it, without dereferencing the unaligned array.
            const uint32_t offset = (uint32_t)((std::size_t)&(((owner const volatile*)0)->*member));
            std::memcpy(s->bytes + offset, &value, sizeof(value));

            staged& op = ops.emplace_back();
            op.op = storage::record_op::update;
            op.id = id;
            op.offset = offset;
            op.len = (uint32_t)sizeof(value);
            std::memcpy(op.bytes, &value, sizeof(value));
            return true;
        }

        // ON DELETE RESTRICT, answered from the index rather than a scan.
        bool delete_customer(record_id id) {
            slot* s = resolve(id);
            if (!s || s->dead || s->tag != customer::tag) return fail_bool();
            auto it = idx().orders_by_customer.find(id);
            if (it != idx().orders_by_customer.end() && !it->second.empty()) return fail_bool();
            return stage_destroy(id);
        }

        // ON DELETE CASCADE: the lines go with the order.
        bool delete_order(record_id id) {
            slot* s = resolve(id);
            if (!s || s->dead || s->tag != order::tag) return fail_bool();
            auto it = idx().lines_by_order.find(id);
            std::vector<record_id> lines = (it == idx().lines_by_order.end()) ? std::vector<record_id>{} : it->second;
            for (record_id line : lines)
                if (!stage_destroy(line)) return false;
            return stage_destroy(id);
        }

        // ---- outcome ----------------------------------------------------

        // Appends the whole batch and flags its last record. Only here does
        // anything become visible - to the storage, to the observer, to
        // recovery, or to a replica.
        txn_result commit(bool durable) {
            if (broken) { abandon(); return txn_result::aborted_constraint; }

            for (const staged& op : ops) {
                switch (op.op) {
                    case storage::record_op::create:
                        t.create_raw(op.id, op.tag, std::span<const std::byte>{ op.bytes, op.len });
                        break;
                    case storage::record_op::update:
                        t.patch(op.id, op.offset, std::span<const std::byte>{ op.bytes, op.len });
                        break;
                    case storage::record_op::destroy:
                        t.destroy(op.id);
                        break;
                }
            }
            // A transaction that staged nothing flags nothing: doing so would
            // re-flag the PREVIOUS transaction's last record.
            if (!ops.empty()) t.set_last_record_flags(txn_commit_flag);
            if (durable) t.sync();

            abandon();
            return txn_result::committed;
        }

        // Throw the transaction away. Costs nothing and writes nothing -
        // which is the point of staging.
        void abandon() noexcept { ops.clear(); overlay.clear(); }

        bool failed() const noexcept { return broken; }
        std::size_t op_count() const noexcept { return ops.size(); }

    private:
        struct staged {
            storage::record_op op = storage::record_op::create;
            record_id id = 0;
            tag_t tag = 0;
            uint32_t offset = 0;
            uint32_t len = 0;
            // alignas because these bytes are read back as a record. Without
            // it the array lands wherever the preceding fields leave it, and
            // UBSan reports every access through the typed pointer as
            // misaligned - which it is.
            alignas(8) std::byte bytes[max_record_bytes]{};
        };

        // What a record looks like to THIS transaction: the committed value
        // with every staged change already folded in.
        struct slot {
            record_id id = 0;
            tag_t tag = 0;
            uint32_t len = 0;
            bool dead = false;
            alignas(8) std::byte bytes[max_record_bytes]{};
        };

        schema_index& idx() { return t.observer_state(); }
        const schema_index& idx() const { return t.observer_state(); }

        record_id fail() { broken = true; return 0; }
        bool fail_bool() { broken = true; return false; }

        // Linear search on purpose: a transaction touches a handful of
        // records, and scanning a contiguous vector beats a hash map by a wide
        // margin at that size.
        slot* find_slot(record_id id) noexcept {
            for (slot& s : overlay)
                if (s.id == id) return &s;
            return nullptr;
        }

        // The overlay entry for `id`, faulted in from the storage if this
        // transaction has not touched it yet. nullptr if no such record.
        slot* resolve(record_id id) {
            if (slot* s = find_slot(id)) return s;
            const tag_t what = t.tag_of(id);
            if (!what) return nullptr;
            slot& s = overlay.emplace_back();
            s.id = id;
            s.tag = what;
            s.len = copy_committed(id, what, s.bytes);
            return &s;
        }

        // A read that sees this transaction's own writes.
        //
        // BY VALUE, not by pointer. A pointer would point into `overlay`, and
        // the very next statement - another view(), a stage_create() - can
        // grow that vector and move it. UBSan and ASan caught exactly this:
        // insert_line() held a view of the order while looking up the product,
        // and pay_order() held a view of the order while looking up its
        // customer. A record here is at most 32 bytes, so copying it out is
        // cheaper than the bug is subtle.
        template <typename T>
        std::optional<T> view(record_id id) {
            slot* s = resolve(id);
            if (!s || s->dead || s->tag != T::tag) return std::nullopt;
            T value;
            std::memcpy(&value, s->bytes, sizeof(T));
            return value;
        }

        record_id stage_create(tag_t tag, const void* value, uint32_t len) {
            const record_id id = t.reserve_id();
            staged& op = ops.emplace_back();
            op.op = storage::record_op::create;
            op.id = id;
            op.tag = tag;
            op.len = len;
            std::memcpy(op.bytes, value, len);

            slot& s = overlay.emplace_back();
            s.id = id;
            s.tag = tag;
            s.len = len;
            std::memcpy(s.bytes, value, len);
            return id;
        }

        bool stage_destroy(record_id id) {
            slot* s = resolve(id);
            if (!s || s->dead) return fail_bool();
            s->dead = true;
            staged& op = ops.emplace_back();
            op.op = storage::record_op::destroy;
            op.id = id;
            return true;
        }

        uint32_t copy_committed(record_id id, tag_t what, std::byte* out) const {
            switch ((kind)what) {
                case kind::customer:   std::memcpy(out, t.template get<customer>(id), sizeof(customer)); return sizeof(customer);
                case kind::product:    std::memcpy(out, t.template get<product>(id), sizeof(product)); return sizeof(product);
                case kind::order:      std::memcpy(out, t.template get<order>(id), sizeof(order)); return sizeof(order);
                case kind::order_line: std::memcpy(out, t.template get<order_line>(id), sizeof(order_line)); return sizeof(order_line);
            }
            return 0;
        }

        // UNIQUE has to consider the committed index AND this transaction's
        // own staged rows - two inserts of the same email inside one
        // transaction must collide even though neither is in the index yet.
        bool email_taken(uint64_t email) const {
            if (idx().customer_by_email.count(email)) return true;
            for (const slot& s : overlay)
                if (!s.dead && s.tag == customer::tag && ((const customer*)s.bytes)->email_hash == email) return true;
            return false;
        }
        bool sku_taken(uint64_t sku) const {
            if (idx().product_by_sku.count(sku)) return true;
            for (const slot& s : overlay)
                if (!s.dead && s.tag == product::tag && ((const product*)s.bytes)->sku_hash == sku) return true;
            return false;
        }

        table_t<spec>& t;
        uint64_t txn_id;
        std::vector<staged> ops;
        std::vector<slot> overlay;
        bool broken = false;
    };

    // =====================================================================
    // Crash-atomic recovery, built entirely on for_each_record() plus the
    // engine's own recover(): find where the last committed transaction
    // ended, then replay only that far. The engine contributes both halves
    // already; the transaction semantics are a dozen lines on top.
    // =====================================================================
    template <typename spec>
    std::size_t recover_to_last_commit(table_t<spec>& table, std::size_t log_size) {
        // Pass one walks the log without applying anything, looking for the
        // boundary flag. Cheap: it verifies checksums and reads headers, and
        // never looks at a payload.
        std::size_t last_commit_end = 0;
        std::size_t position = 0;
        table.for_each_record(0, log_size, [&](const storage::record_header& hdr, std::span<const std::byte> payload) {
            position += storage::pad8(sizeof(storage::record_header) + payload.size());
            if (hdr.flags & txn_commit_flag) last_commit_end = position;
        });
        // Pass two is the ordinary recovery, bounded by that offset.
        return table.recover(last_commit_end);
    }

    // Puts the secondary indexes back after a checkpoint restart, in one pass
    // over the live records. The engine's own state came back without
    // replaying anything; this is the part that still has to be paid for,
    // and it costs live data rather than log.
    template <typename spec>
    std::size_t rebuild_indexes(table_t<spec>& table) {
        schema_index fresh;
        std::size_t seen = 0;
        table.for_each_raw([&](typename table_t<spec>::raw_view rv) {
            fresh.index_existing(rv.id, rv.tag, rv.bytes.data());
            ++seen;
        });
        table.observer_state() = std::move(fresh);
        return seen;
    }

    // ---- queries, to show what the indexes are for ----------------------

    template <typename spec>
    record_id find_customer_by_email(const table_t<spec>& table, uint64_t email_hash) {
        const auto& m = table.observer_state().customer_by_email;
        auto it = m.find(email_hash);
        return it == m.end() ? 0 : it->second;
    }

    template <typename spec>
    record_id scan_customer_by_email(const table_t<spec>& table, uint64_t email_hash) {
        record_id found = 0;
        table.template for_each<customer>([&](record_id id, const customer& c) {
            if (!found && c.email_hash == email_hash && table.tag_of(id) == customer::tag) found = id;
        });
        return found;
    }

    template <typename spec>
    std::size_t count_products_in_price_range(const table_t<spec>& table, int64_t lo, int64_t hi) {
        const auto& m = table.observer_state().products_by_price;
        return (std::size_t)std::distance(m.lower_bound(lo), m.upper_bound(hi));
    }

    template <typename spec>
    int64_t sum_customer_order_totals(const table_t<spec>& table, record_id customer_id) {
        const auto& m = table.observer_state().orders_by_customer;
        auto it = m.find(customer_id);
        if (it == m.end()) return 0;
        int64_t total = 0;
        for (record_id o : it->second)
            if (const order* rec = table.template get<order>(o)) total += rec->total;
        return total;
    }

} // namespace app
