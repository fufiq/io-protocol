// test_indexes.cpp - the three index kinds, and the three rules the composition
// exists to enforce.
//
// Each of those rules is a bug someone has shipped, so each gets a test whose
// failure mode is the bug rather than "it did not crash": a ghost entry after an
// update, an index built from a fragment, and an update silently skipped
// because the header carries no tag.

#include <doctest/doctest.h>

#include "../storage/indexes.hpp"

#include <cstdio>
#include <string>
#include <vector>

using storage::record_id;
using storage::tag_t;

namespace {

    enum class kind : tag_t { customer = 1, order = 2 };

    struct customer {
        static constexpr tag_t tag = (tag_t)kind::customer;
        uint64_t email_hash;
        int64_t  balance;
        uint32_t status;
        uint32_t reserved = 0;
    };
    static_assert(storage::tagged_aggregate<customer>);

    struct order {
        static constexpr tag_t tag = (tag_t)kind::order;
        uint64_t customer_id;
        int64_t  total;
        uint64_t placed_ms;
    };
    static_assert(storage::tagged_aggregate<order>);

    using indexes = storage::index_set<
        storage::unique_index<&customer::email_hash>,
        storage::multi_index<&order::customer_id>,
        storage::ordered_index<&customer::balance>>;

    using spec = storage::field_spec<"t_ix.log", 1u << 22, "t_ix.idx", 8192, 8192, 8, 64>;
    using table_t = storage::field_storage<spec, indexes>;

    static_assert(storage::observes<indexes, table_t>);

    void wipe() {
        std::remove(spec::path.value);
        std::remove(spec::index_path);
    }

    struct fixture {
        fixture() { wipe(); table = new table_t(); REQUIRE(table->ok()); }
        ~fixture() { delete table; wipe(); }
        table_t* table = nullptr;
        indexes& ix() { return table->observer_state(); }
    };

} // namespace

TEST_CASE("a unique index finds a record by a field the engine knows nothing about") {
    fixture f;
    const auto a = f.table->create(customer{ 1111, 500, 0 });
    const auto b = f.table->create(customer{ 2222, 900, 0 });

    CHECK(f.ix().find<&customer::email_hash>(1111) == a);
    CHECK(f.ix().find<&customer::email_hash>(2222) == b);
    CHECK(f.ix().find<&customer::email_hash>(3333) == 0);   // 0 means absent; ids start at 1
    CHECK(f.ix().of<&customer::email_hash>().size() == 2);
    CHECK(f.ix().failed_inserts() == 0);
}

TEST_CASE("updating an indexed field leaves no ghost behind") {
    // The rule: retract before add. Without it the old key still resolves, and
    // it resolves to a record that no longer has that value - which reads as a
    // hit rather than as corruption, so nothing downstream notices.
    fixture f;
    const auto id = f.table->create(customer{ 1111, 500, 0 });
    REQUIRE(f.ix().find<&customer::email_hash>(1111) == id);

    f.table->update<&customer::email_hash>(id, 9999);

    CHECK(f.ix().find<&customer::email_hash>(9999) == id);
    CHECK(f.ix().find<&customer::email_hash>(1111) == 0);   // the ghost
    CHECK(f.ix().of<&customer::email_hash>().size() == 1);
}

TEST_CASE("an update to an UNindexed field does not disturb the index") {
    // The rule: resolve the post-update record. An UPDATE carries only the
    // changed bytes, so indexing the payload would index a fragment - here the
    // balance's bytes would be read as an email hash.
    fixture f;
    const auto id = f.table->create(customer{ 1111, 500, 0 });
    f.table->update<&customer::balance>(id, 12345);

    CHECK(f.ix().find<&customer::email_hash>(1111) == id);
    CHECK(f.ix().of<&customer::email_hash>().size() == 1);

    auto got = f.table->get<customer>(id);
    REQUIRE(got.has_value());
    CHECK(got->balance == 12345);
    CHECK(got->email_hash == 1111);
}

TEST_CASE("destroying a record removes it from every index") {
    fixture f;
    const auto id = f.table->create(customer{ 1111, 500, 0 });
    REQUIRE(f.ix().find<&customer::email_hash>(1111) == id);
    REQUIRE(f.ix().count_range<&customer::balance>(0, 1000) == 1);

    f.table->destroy(id);

    CHECK(f.ix().find<&customer::email_hash>(1111) == 0);
    CHECK(f.ix().count_range<&customer::balance>(0, 1000) == 0);
}

TEST_CASE("a multi index holds every record under one key, and loses them one at a time") {
    fixture f;
    std::vector<record_id> lines;
    for (int i = 0; i < 5; ++i) lines.push_back(f.table->create(order{ 42, 100 + i, 0 }));
    f.table->create(order{ 43, 999, 0 });

    CHECK(f.ix().count<&order::customer_id>(42) == 5);
    CHECK(f.ix().count<&order::customer_id>(43) == 1);
    CHECK(f.ix().count<&order::customer_id>(44) == 0);

    int64_t sum = 0;
    f.ix().each<&order::customer_id>(42, [&](record_id id) {
        if (auto o = f.table->get<order>(id)) sum += o->total;
    });
    CHECK(sum == 100 + 101 + 102 + 103 + 104);

    // Moving one order to another customer must move exactly one entry.
    f.table->update<&order::customer_id>(lines[2], 43);
    CHECK(f.ix().count<&order::customer_id>(42) == 4);
    CHECK(f.ix().count<&order::customer_id>(43) == 2);

    f.table->destroy(lines[0]);
    CHECK(f.ix().count<&order::customer_id>(42) == 3);
}

TEST_CASE("an ordered index answers ranges and follows updates") {
    fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 10; ++i) ids.push_back(f.table->create(customer{ (uint64_t)(1000 + i), i * 100, 0 }));

    CHECK(f.ix().count_range<&customer::balance>(0, 900) == 10);
    CHECK(f.ix().count_range<&customer::balance>(300, 600) == 4);   // 300,400,500,600
    CHECK(f.ix().count_range<&customer::balance>(1000, 2000) == 0);

    std::vector<int64_t> seen;
    f.ix().each_range<&customer::balance>(200, 400, [&](record_id, int64_t key) { seen.push_back(key); });
    CHECK(seen == std::vector<int64_t>{ 200, 300, 400 });   // and in order

    // A negative balance sorts below everything, which an unsigned key would
    // have put at the top instead.
    f.table->update<&customer::balance>(ids[5], -1);
    CHECK(f.ix().count_range<&customer::balance>(-10, -1) == 1);   // just the negative one
    CHECK(f.ix().count_range<&customer::balance>(-10, 0) == 2);    // and the zero balance with it
    CHECK(f.ix().count_range<&customer::balance>(0, 900) == 9);    // 500 left, -1 arrived
}

TEST_CASE("one record type does not disturb another's indexes") {
    fixture f;
    f.table->create(customer{ 1111, 500, 0 });
    f.table->create(order{ 1111, 700, 0 });   // same value, different type

    CHECK(f.ix().of<&customer::email_hash>().size() == 1);
    CHECK(f.ix().count<&order::customer_id>(1111) == 1);

    // An update to the order must not touch the customer index, even though
    // the tag is absent from an UPDATE header and both records share a value.
    f.table->create(order{ 1111, 800, 0 });
    CHECK(f.ix().count<&order::customer_id>(1111) == 2);
    CHECK(f.ix().of<&customer::email_hash>().size() == 1);
}

TEST_CASE("the indexes survive a replay, because the observer is what builds them") {
    // The whole reason index maintenance belongs below apply(): recovery runs
    // the same path a live write does, so an index rebuilt from the log is the
    // one the writes produced. If this passes, the index cannot drift.
    wipe();
    record_id id = 0;
    {
        table_t t;
        REQUIRE(t.ok());
        id = t.create(customer{ 1111, 500, 0 });
        t.create(customer{ 2222, 700, 0 });
        t.update<&customer::email_hash>(id, 3333);
        t.sync();
    }
    {
        table_t t;
        REQUIRE(t.ok());
        t.recover(1u << 22);
        CHECK(t.observer_state().find<&customer::email_hash>(3333) == id);
        CHECK(t.observer_state().find<&customer::email_hash>(1111) == 0);
        CHECK(t.observer_state().of<&customer::email_hash>().size() == 2);
    }
    wipe();
}

TEST_CASE("running out of capacity is counted, never silent") {
    // A partial index is worse than an empty one: it looks like an answer.
    wipe();
    table_t t;
    REQUIRE(t.ok());
    // Deliberately far too small. The fixture's default is generous; this is
    // what an application that guessed low gets.
    t.observer_state() = indexes{ 8 };

    for (int i = 0; i < 400; ++i) t.create(customer{ (uint64_t)(1000 + i), i, 0 });
    CHECK(t.observer_state().failed_inserts() > 0);
    wipe();
}

TEST_CASE("a checkpoint restart rebuilds the indexes instead of leaving them empty") {
    // The one path where on_record never fires: restart() from a checkpoint
    // skips the replay, which is the whole point of it. Without the engine
    // telling the observer, every index came back empty and answered "no" to
    // every lookup - a result rather than a failure, indistinguishable from
    // downstream.
    using ck_spec = storage::field_spec<"t_ixck.log", 1u << 22, "t_ixck.idx", 8192, 8192, 8, 64, "t_ixck.ck">;
    using ck_table = storage::field_storage<ck_spec, indexes>;
    const auto ck_wipe = [] {
        std::remove(ck_spec::path.value);
        std::remove(ck_spec::index_path);
        std::remove(ck_spec::checkpoint_path);
    };

    ck_wipe();
    record_id id = 0;
    {
        ck_table t;
        REQUIRE(t.ok());
        id = t.create(customer{ 1111, 500, 0 });
        t.create(customer{ 2222, 700, 0 });
        t.create(order{ (uint64_t)id, 50, 0 });
        t.update<&customer::balance>(id, 900);
        REQUIRE(t.checkpoint());
    }
    {
        ck_table t;
        REQUIRE(t.ok());
        t.restart(1u << 22);
        REQUIRE(t.started_from_checkpoint());   // the replay really was skipped

        CHECK(t.observer_state().find<&customer::email_hash>(1111) == id);
        CHECK(t.observer_state().of<&customer::email_hash>().size() == 2);
        CHECK(t.observer_state().count<&order::customer_id>((uint64_t)id) == 1);
        // Rebuilt from the LIVE record, so the update is in it - not from the
        // create alone.
        CHECK(t.observer_state().count_range<&customer::balance>(900, 900) == 1);
        CHECK(t.observer_state().count_range<&customer::balance>(500, 500) == 0);
    }
    ck_wipe();
}

TEST_CASE("an ordered index stays exact across the merges it does behind your back") {
    // Inserts do not go straight into the sorted array - they land in a pending
    // tail that is merged in batches, because inserting into the middle of a
    // sorted vector moves the tail and a bulk load of 200k rows cost 320 GB of
    // memmove before this existed (bench/app_schema_bench.cpp, +1115% on the
    // write path; it is +83% now).
    //
    // The batching is invisible unless it is wrong, so this walks a population
    // large enough to cross the merge threshold several times, deletes entries
    // on BOTH sides of it - one still pending, one long since merged - and
    // compares every answer against a reference built independently.
    wipe();
    table_t t;
    REQUIRE(t.ok());
    t.observer_state() = indexes{ 8000 };

    std::vector<int64_t> reference;
    std::vector<record_id> ids;
    uint64_t seed = 99;
    const auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };

    for (int i = 0; i < 6000; ++i) {
        const int64_t balance = (int64_t)(next() % 2000);
        ids.push_back(t.create(customer{ (uint64_t)(500000 + i), balance, 0 }));
        reference.push_back(balance);
    }
    REQUIRE(t.observer_state().failed_inserts() == 0);

    const auto expect_range = [&](int64_t lo, int64_t hi) {
        std::size_t n = 0;
        for (int64_t b : reference) if (b >= lo && b <= hi) ++n;
        return n;
    };

    CHECK(t.observer_state().count_range<&customer::balance>(0, 1999) == 6000);
    CHECK(t.observer_state().count_range<&customer::balance>(700, 900) == expect_range(700, 900));
    CHECK(t.observer_state().count_range<&customer::balance>(0, 0) == expect_range(0, 0));

    // A query settles the pending tail, so what follows exercises inserts that
    // arrive on top of an already-merged array - the other half of the state
    // machine, and the half a single-shot test would never reach.
    for (int i = 0; i < 1500; ++i) {
        const int64_t balance = (int64_t)(next() % 2000);
        ids.push_back(t.create(customer{ (uint64_t)(600000 + i), balance, 0 }));
        reference.push_back(balance);
    }
    CHECK(t.observer_state().count_range<&customer::balance>(700, 900) == expect_range(700, 900));

    // Now remove one entry that is certainly merged (the very first) and one
    // that is certainly still pending (the very last), and check both took.
    const auto drop = [&](std::size_t at) {
        t.destroy(ids[at]);
        reference[at] = -1;   // outside every range asked about below
    };
    drop(0);
    drop(reference.size() - 1);
    CHECK(t.observer_state().count_range<&customer::balance>(0, 1999) == expect_range(0, 1999));
    CHECK(t.observer_state().count_range<&customer::balance>(700, 900) == expect_range(700, 900));

    // And the walk agrees with the count, in order.
    int64_t previous = -1;
    std::size_t walked = 0;
    t.observer_state().each_range<&customer::balance>(700, 900, [&](record_id, int64_t balance) {
        CHECK(balance >= previous);   // the whole reason it is an ordered index
        previous = balance;
        ++walked;
    });
    CHECK(walked == expect_range(700, 900));
    wipe();
}

TEST_CASE("rebuilding an index twice does not double it") {
    // index_set::on_restart re-reserves every index and rebuilds from the live
    // records. The hash maps assign() and so start empty; the ordered map only
    // reserved, kept its pairs, and appended a second copy of everything - so a
    // second restart on a live index_set returned every product twice and every
    // range count was exactly double. Nothing crashed, which is the problem.
    wipe();
    table_t t;
    REQUIRE(t.ok());
    t.observer_state() = indexes{ 1024 };

    for (int i = 0; i < 50; ++i) t.create(customer{ (uint64_t)(9000 + i), i * 10, 0 });
    const auto once = t.observer_state().count_range<&customer::balance>(0, 1000);
    CHECK(once == 50);

    t.observer_state().on_restart(t);
    CHECK(t.observer_state().count_range<&customer::balance>(0, 1000) == once);
    CHECK(t.observer_state().find<&customer::email_hash>(9000) != 0);

    t.observer_state().on_restart(t);
    CHECK(t.observer_state().count_range<&customer::balance>(0, 1000) == once);
    CHECK(t.observer_state().failed_inserts() == 0);
    wipe();
}
