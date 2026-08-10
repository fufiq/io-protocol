#pragma once
// app_txn.hpp - constraints, triggers and transactions over app_layer.hpp.
//
// Everything here sits ABOVE apply(), which is the whole design: none of it
// re-runs when the log is replayed. A trigger that decremented stock when a
// line was inserted must not decrement it again at startup - that decrement
// is itself a record in the same log, and replaying the log replays it. Only
// the observer (index maintenance) belongs below apply(); constraints and
// triggers belong here.
//
// Transactions get two separate guarantees, because they are two separate
// problems:
//
//   ABORT while running   -> an undo log. Each op records how to reverse
//                            itself; abort replays them backwards. Those
//                            reversals are ordinary records, exactly as a
//                            logical undo log works in a real engine.
//
//   CRASH mid-transaction -> a commit marker. Every committed transaction
//                            ends with one. recover_to_last_commit() finds
//                            the last marker with for_each_record() and hands
//                            recover() that offset, so a transaction that was
//                            still being written is simply never replayed.
//                            The engine needs to know nothing about any of it.

#include "app_layer.hpp"

#include <cstdio>
#include <string>

namespace app {

    template <typename spec>
    using table_t = storage::storage<spec, schema_index>;

    // How a transaction ended, so a benchmark can count outcomes rather than
    // guess at them.
    enum class txn_result { committed, aborted_constraint, aborted_by_caller };

    template <typename spec>
    class txn {
    public:
        explicit txn(table_t<spec>& table, uint64_t id) : t(table), txn_id(id) {}
        ~txn() { if (!finished) abort(); }

        txn(const txn&) = delete;
        txn& operator=(const txn&) = delete;

        // ---- statements -------------------------------------------------
        // Each returns 0 / false on a constraint violation and marks the
        // transaction failed; the caller can stop immediately, and the
        // destructor unwinds whatever already happened.

        record_id insert_customer(const customer& c) {
            if (c.balance < 0) return fail();                                    // CHECK
            if (idx().customer_by_email.count(c.email_hash)) return fail();      // UNIQUE
            return note_create(t.create(c));
        }

        record_id insert_product(const product& p) {
            if (p.stock < 0 || p.price < 0) return fail();                       // CHECK
            if (idx().product_by_sku.count(p.sku_hash)) return fail();           // UNIQUE
            return note_create(t.create(p));
        }

        record_id insert_order(record_id customer_id) {
            const customer* c = t.template get<customer>(customer_id);
            if (!c || t.tag_of(customer_id) != customer::tag) return fail();     // FOREIGN KEY
            if (c->status != 0) return fail();                                   // BEFORE INSERT trigger
            return note_create(t.create(order{ customer_id, 0, 0, 0 }));
        }

        // The interesting one: three constraints, two after-triggers, and a
        // rollback path that has to undo both triggers as well as the insert.
        record_id insert_line(record_id order_id, record_id product_id, int64_t qty) {
            if (qty <= 0) return fail();                                          // CHECK
            const order* o = t.template get<order>(order_id);
            if (!o || t.tag_of(order_id) != order::tag) return fail();            // FOREIGN KEY
            const product* p = t.template get<product>(product_id);
            if (!p || t.tag_of(product_id) != product::tag) return fail();        // FOREIGN KEY
            if (o->status != 0) return fail();                                    // BEFORE INSERT trigger
            if (p->stock < qty) return fail();                                    // BEFORE INSERT trigger

            const int64_t price = p->price;
            record_id line = note_create(t.create(order_line{ order_id, product_id, qty, price }));
            if (!line) return 0;

            // AFTER INSERT triggers.
            if (!update_field<&product::stock>(product_id, p->stock - qty)) return 0;
            if (!update_field<&order::total>(order_id, o->total + price * qty)) return 0;
            return line;
        }

        bool pay_order(record_id order_id) {
            const order* o = t.template get<order>(order_id);
            if (!o || t.tag_of(order_id) != order::tag) return fail_bool();
            if (o->status != 0) return fail_bool();
            const customer* c = t.template get<customer>(o->customer_id);
            if (!c) return fail_bool();
            if (c->balance < o->total) return fail_bool();                        // CHECK (balance >= 0)

            if (!update_field<&customer::balance>(o->customer_id, c->balance - o->total)) return false;
            return update_field<&order::status>(order_id, 1);                     // AFTER UPDATE trigger
        }

        // ON DELETE RESTRICT, answered from the index rather than a scan.
        bool delete_customer(record_id id) {
            if (t.tag_of(id) != customer::tag) return fail_bool();
            auto it = idx().orders_by_customer.find(id);
            if (it != idx().orders_by_customer.end() && !it->second.empty()) return fail_bool();
            return note_destroy(id);
        }

        // ON DELETE CASCADE: the lines go with the order.
        bool delete_order(record_id id) {
            if (t.tag_of(id) != order::tag) return fail_bool();
            auto it = idx().lines_by_order.find(id);
            std::vector<record_id> lines = (it == idx().lines_by_order.end()) ? std::vector<record_id>{} : it->second;
            for (record_id line : lines)
                if (!note_destroy(line)) return false;
            return note_destroy(id);
        }

        // ---- outcome ----------------------------------------------------

        txn_result commit(bool durable) {
            if (broken) { abort(); return txn_result::aborted_constraint; }
            finished = true;
            // The marker is the last thing written, and only then is anything
            // made durable. A crash before it leaves a transaction that
            // recover_to_last_commit() will not replay.
            t.create(commit_marker{ txn_id, (uint64_t)undo.size() });
            if (durable) t.sync();
            return txn_result::committed;
        }

        void abort() {
            if (finished) return;
            finished = true;
            // Backwards, so a record created and then updated is restored
            // before it is removed.
            for (std::size_t i = undo.size(); i-- > 0;) {
                undo_entry& u = undo[i];
                switch (u.what) {
                    case undo_kind::created:
                        t.destroy(u.id);
                        break;
                    case undo_kind::patched:
                        t.patch(u.id, u.offset, std::span<const std::byte>{ u.bytes, u.len });
                        break;
                    case undo_kind::destroyed:
                        restore(u);
                        break;
                }
            }
            undo.clear();
        }

        // Stop tracking and write nothing further - neither a commit marker
        // nor the undo log. What a crash looks like from the log's point of
        // view: records that were written, with no marker after them.
        void abandon() noexcept { finished = true; undo.clear(); }

        bool failed() const noexcept { return broken; }
        std::size_t op_count() const noexcept { return undo.size(); }

    private:
        enum class undo_kind : uint8_t { created, patched, destroyed };
        struct undo_entry {
            undo_kind what;
            record_id id;
            uint32_t offset = 0;
            uint32_t len = 0;
            tag_t tag = 0;
            std::byte bytes[sizeof(order_line)]{}; // the widest record in the schema
        };

        schema_index& idx() { return t.observer_state(); }

        record_id fail() { broken = true; return 0; }
        bool fail_bool() { broken = true; return false; }

        record_id note_create(record_id id) {
            if (!id) return fail();
            undo.push_back(undo_entry{ undo_kind::created, id });
            return id;
        }

        // Saves the bytes the patch is about to overwrite, then patches.
        template <auto member>
        bool update_field(record_id id, storage::member_value_t<member> value) {
            using owner = storage::member_class_t<member>;
            const owner* current = t.template get<owner>(id);
            if (!current) return fail_bool();

            undo_entry u{ undo_kind::patched, id };
            u.offset = (uint32_t)((const std::byte*)&(current->*member) - (const std::byte*)current);
            u.len = (uint32_t)sizeof(storage::member_value_t<member>);
            std::memcpy(u.bytes, (const std::byte*)current + u.offset, u.len);
            undo.push_back(u);

            t.template update<member>(id, value);
            return true;
        }

        bool note_destroy(record_id id) {
            const tag_t what = t.tag_of(id);
            if (!what) return fail_bool();
            undo_entry u{ undo_kind::destroyed, id };
            u.tag = what;
            u.len = copy_out(id, what, u.bytes);
            undo.push_back(u);
            t.destroy(id);
            return true;
        }

        uint32_t copy_out(record_id id, tag_t what, std::byte* out) {
            switch ((kind)what) {
                case kind::customer:   std::memcpy(out, t.template get<customer>(id), sizeof(customer)); return sizeof(customer);
                case kind::product:    std::memcpy(out, t.template get<product>(id), sizeof(product)); return sizeof(product);
                case kind::order:      std::memcpy(out, t.template get<order>(id), sizeof(order)); return sizeof(order);
                case kind::order_line: std::memcpy(out, t.template get<order_line>(id), sizeof(order_line)); return sizeof(order_line);
                case kind::commit:     return 0;
            }
            return 0;
        }

        void restore(const undo_entry& u) {
            switch ((kind)u.tag) {
                case kind::customer:   { customer v; std::memcpy(&v, u.bytes, sizeof(v)); t.recreate(u.id, v); break; }
                case kind::product:    { product v; std::memcpy(&v, u.bytes, sizeof(v)); t.recreate(u.id, v); break; }
                case kind::order:      { order v; std::memcpy(&v, u.bytes, sizeof(v)); t.recreate(u.id, v); break; }
                case kind::order_line: { order_line v; std::memcpy(&v, u.bytes, sizeof(v)); t.recreate(u.id, v); break; }
                case kind::commit:     break;
            }
        }

        table_t<spec>& t;
        uint64_t txn_id;
        std::vector<undo_entry> undo;
        bool broken = false;
        bool finished = false;
    };

    // =====================================================================
    // Crash-atomic recovery, built entirely on for_each_record() plus the
    // engine's own recover(): find where the last committed transaction
    // ended, then replay only that far. Everything the engine contributes is
    // already there; the transaction semantics are 15 lines on top.
    // =====================================================================
    template <typename spec>
    std::size_t recover_to_last_commit(table_t<spec>& table, std::size_t log_size) {
        // Pass one walks the log without applying anything, looking for
        // markers. Cheap: it verifies checksums and reads headers, and only a
        // commit marker's payload is even looked at.
        std::size_t last_commit_end = 0;
        std::size_t position = 0;
        table.for_each_record(0, log_size, [&](const storage::record_header& hdr, std::span<const std::byte> payload) {
            position += storage::pad8(sizeof(storage::record_header) + payload.size());
            if (hdr.tag == commit_marker::tag) last_commit_end = position;
        });
        // Pass two is the ordinary recovery, bounded by that offset.
        return table.recover(last_commit_end);
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
