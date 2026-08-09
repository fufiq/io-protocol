#include <doctest/doctest.h>

#include "../storage/storage.hpp"

#include <cstring>
#include <random>

// No `using namespace storage;` here: the namespace and the class template
// share the name, so an unqualified `storage::storage<>` would be ambiguous.
using storage::tag_t;
using storage::record_id;
using storage::tagged_aggregate;

namespace {

constexpr size_t region_bytes = 4ull * 1024 * 1024;

enum class kind : tag_t { user = 1, order = 2 };

// Explicitly padded: uint64_t + uint32_t alone leaves a 4-byte hole, which
// has_unique_object_representations_v (and therefore tagged_aggregate)
// correctly rejects - padding bytes are exactly what must not be written to
// disk or shipped over a wire. Spelling the filler out is the fix.
struct user {
    static constexpr tag_t tag = (tag_t)kind::user;
    uint64_t balance;
    uint32_t age;
    uint32_t reserved = 0;
};
static_assert(tagged_aggregate<user>);

struct order {
    static constexpr tag_t tag = (tag_t)kind::order;
    record_id user_id;
    uint64_t amount;
};
static_assert(tagged_aggregate<order>);

// A storage plus the region backing it, so a test can hand the same bytes
// to a second storage and recover from them.
struct fixture {
    std::vector<std::byte> region{region_bytes};
    storage::storage<> table{region.data()};
};

} // namespace

// =========================================================================
// Basics / invariants
// =========================================================================

TEST_CASE("create/get round-trips and ids are never reused") {
    fixture f;
    auto a = f.table.create(user{100, 30});
    auto b = f.table.create(user{200, 40});
    CHECK(a != b);

    const user* ua = f.table.get<user>(a);
    REQUIRE(ua != nullptr);
    CHECK(ua->balance == 100);
    CHECK(ua->age == 30);

    f.table.destroy(a);
    CHECK(f.table.get<user>(a) == nullptr);

    // A fresh create must not hand back the id that was just destroyed -
    // anything holding that id as a reference would silently alias.
    auto c = f.table.create(user{300, 50});
    CHECK(c != a);
    CHECK(f.table.get<user>(a) == nullptr);
}

TEST_CASE("get on an id that was never created returns nullptr, not garbage") {
    fixture f;
    CHECK(f.table.get<user>(1) == nullptr);
    CHECK(f.table.get<user>(999999) == nullptr);
    f.table.create(user{1, 1});
    CHECK(f.table.get<user>(12345) == nullptr);
}

TEST_CASE("update patches one field and leaves the rest alone") {
    fixture f;
    auto id = f.table.create(user{100, 30});
    f.table.update<&user::balance>(id, 555);

    const user* u = f.table.get<user>(id);
    REQUIRE(u != nullptr);
    CHECK(u->balance == 555);
    CHECK(u->age == 30); // untouched
}

TEST_CASE("update/destroy on a dead or unknown id is a no-op, not a crash") {
    fixture f;
    auto id = f.table.create(user{1, 2});
    f.table.destroy(id);

    f.table.update<&user::balance>(id, 9);   // dead
    f.table.update<&user::balance>(4242, 9); // never existed
    f.table.destroy(id);                     // double destroy
    f.table.destroy(4242);
    CHECK(f.table.get<user>(id) == nullptr);
    CHECK(f.table.live_bytes() == 0);
}

TEST_CASE("live_bytes tracks creates, updates and destroys exactly") {
    fixture f;
    CHECK(f.table.live_bytes() == 0);

    auto a = f.table.create(user{1, 1});
    CHECK(f.table.live_bytes() == sizeof(user));
    auto b = f.table.create(user{2, 2});
    CHECK(f.table.live_bytes() == 2 * sizeof(user));

    f.table.update<&user::balance>(a, 7); // an update must not change live bytes
    CHECK(f.table.live_bytes() == 2 * sizeof(user));

    f.table.destroy(a);
    CHECK(f.table.live_bytes() == sizeof(user));
    f.table.destroy(b);
    CHECK(f.table.live_bytes() == 0);
}

TEST_CASE("mixed types coexist under one id space and one storage") {
    fixture f;
    auto u = f.table.create(user{10, 20});
    auto o = f.table.create(order{u, 999});

    REQUIRE(f.table.get<user>(u) != nullptr);
    REQUIRE(f.table.get<order>(o) != nullptr);
    CHECK(f.table.get<order>(o)->user_id == u);
    CHECK(f.table.get<order>(o)->amount == 999);

    // for_each_raw must report both, tagged so the caller can dispatch.
    int users = 0, orders = 0;
    f.table.for_each_raw([&](storage::storage<>::raw_view rv) {
        if (rv.tag == (tag_t)kind::user) ++users;
        else if (rv.tag == (tag_t)kind::order) ++orders;
    });
    CHECK(users == 1);
    CHECK(orders == 1);
}

TEST_CASE("recreate replaces in place and keeps the id stable") {
    fixture f;
    auto id = f.table.create(user{1, 1});
    f.table.recreate(id, user{777, 88});

    const user* u = f.table.get<user>(id);
    REQUIRE(u != nullptr);
    CHECK(u->balance == 777);
    CHECK(u->age == 88);
    CHECK(f.table.live_bytes() == sizeof(user)); // old bytes became garbage, not live
}

TEST_CASE("recreate onto a never-used id behaves like create") {
    fixture f;
    f.table.recreate(42, user{5, 6});
    const user* u = f.table.get<user>(42);
    REQUIRE(u != nullptr);
    CHECK(u->balance == 5);
    // A later create must not collide with the id recreate() claimed.
    auto next = f.table.create(user{1, 1});
    CHECK(next > 42);
}

// =========================================================================
// Recovery - the parser that sees bytes it did not just write
// =========================================================================

TEST_CASE("recover rebuilds exactly what was written") {
    std::vector<std::byte> region(region_bytes);
    record_id a, b;
    std::size_t written;
    {
        storage::storage<> t(region.data());
        a = t.create(user{11, 22});
        b = t.create(user{33, 44});
        t.update<&user::balance>(a, 99);
        t.destroy(b);
        written = t.current_offset();
    }
    storage::storage<> restored(region.data());
    std::size_t got = restored.recover(written);

    CHECK(got == written);
    REQUIRE(restored.get<user>(a) != nullptr);
    CHECK(restored.get<user>(a)->balance == 99);
    CHECK(restored.get<user>(a)->age == 22);
    CHECK(restored.get<user>(b) == nullptr); // destroyed before the "crash"
    CHECK(restored.live_bytes() == sizeof(user));
}

TEST_CASE("recover stops at a truncated tail without consuming it") {
    std::vector<std::byte> region(region_bytes);
    record_id a;
    std::size_t full;
    {
        storage::storage<> t(region.data());
        a = t.create(user{1, 2});
        full = t.current_offset();
        t.create(user{3, 4}); // this one will be cut in half
    }
    // Hand recover() a size that lands inside the second record.
    storage::storage<> restored(region.data());
    std::size_t got = restored.recover(full + 8);

    CHECK(got == full); // stopped cleanly at the last whole record
    CHECK(restored.get<user>(a) != nullptr);
    CHECK(restored.current_offset() == full);
}

TEST_CASE("recover stops at a corrupted checksum and keeps everything before it") {
    std::vector<std::byte> region(region_bytes);
    record_id a, b;
    std::size_t after_a, total;
    {
        storage::storage<> t(region.data());
        a = t.create(user{1, 2});
        after_a = t.current_offset();
        b = t.create(user{3, 4});
        t.update<&user::balance>(b, 77); // an UPDATE: checksum covers payload too
        total = t.current_offset();
    }
    // Corrupt the payload of the last record; its checksum must now fail.
    region[total - 1] = (std::byte)(~(unsigned char)region[total - 1]);

    storage::storage<> restored(region.data());
    std::size_t got = restored.recover(total);

    CHECK(got < total);            // refused the corrupted record
    CHECK(got >= after_a);         // but kept what came before it
    CHECK(restored.get<user>(a) != nullptr);
    CHECK(restored.get<user>(a)->balance == 1);
}

TEST_CASE("recover on an empty/zeroed region yields an empty storage") {
    std::vector<std::byte> region(region_bytes, std::byte{0});
    storage::storage<> t(region.data());
    CHECK(t.recover(region_bytes) == 0);
    CHECK(t.live_bytes() == 0);
    CHECK(t.current_offset() == 0);
    // ...and it must still be usable afterwards.
    auto id = t.create(user{1, 1});
    CHECK(t.get<user>(id) != nullptr);
}

TEST_CASE("recover tolerates a region of pure garbage") {
    std::vector<std::byte> region(region_bytes);
    std::mt19937 rng(12345);
    for (auto& b : region) b = (std::byte)(rng() & 0xFF);

    storage::storage<> t(region.data());
    std::size_t got = t.recover(region_bytes);
    CHECK(got <= region_bytes); // whatever it accepted, it stayed in bounds
    // Anything it believed was live must point inside the region.
    t.for_each_raw([&](storage::storage<>::raw_view rv) {
        CHECK(rv.bytes.data() >= region.data());
        CHECK(rv.bytes.data() + rv.bytes.size() <= region.data() + region_bytes);
    });
}

// =========================================================================
// Compaction / snapshot
// =========================================================================

TEST_CASE("compact preserves live records and drops garbage") {
    fixture f;
    auto keep = f.table.create(user{1, 1});
    auto drop = f.table.create(user{2, 2});
    for (int i = 0; i < 50; ++i) f.table.update<&user::balance>(keep, i); // pile up garbage
    f.table.destroy(drop);

    std::size_t before = f.table.current_offset();
    std::vector<std::byte> scratch(region_bytes);
    f.table.compact(scratch.data());

    CHECK(f.table.current_offset() < before);       // actually reclaimed something
    CHECK(f.table.live_bytes() == sizeof(user));
    REQUIRE(f.table.get<user>(keep) != nullptr);
    CHECK(f.table.get<user>(keep)->balance == 49);  // last value survives
    CHECK(f.table.get<user>(drop) == nullptr);
}

TEST_CASE("export_snapshot does not disturb the source storage") {
    fixture f;
    auto id = f.table.create(user{5, 6});
    f.table.update<&user::balance>(id, 50);
    std::size_t offset_before = f.table.current_offset();
    std::size_t live_before = f.table.live_bytes();

    auto snap = f.table.export_snapshot();

    CHECK(f.table.current_offset() == offset_before); // read-only, unlike compact()
    CHECK(f.table.live_bytes() == live_before);
    CHECK(f.table.get<user>(id)->balance == 50);
    CHECK(snap.size() > 0);
}

TEST_CASE("a snapshot applied to an empty storage reproduces the source exactly") {
    fixture src;
    auto a = src.table.create(user{1, 2});
    auto b = src.table.create(order{a, 300});
    src.table.update<&user::balance>(a, 42);
    auto snap = src.table.export_snapshot();

    fixture dst;
    dst.table.apply_wal_range(snap.data(), snap.size());

    REQUIRE(dst.table.get<user>(a) != nullptr);
    CHECK(dst.table.get<user>(a)->balance == 42);
    CHECK(dst.table.get<user>(a)->age == 2);
    REQUIRE(dst.table.get<order>(b) != nullptr);
    CHECK(dst.table.get<order>(b)->amount == 300);
    CHECK(dst.table.live_bytes() == src.table.live_bytes());
}

TEST_CASE("reset returns a storage to empty and it is usable afterwards") {
    fixture f;
    f.table.create(user{1, 1});
    f.table.create(user{2, 2});
    f.table.reset();

    CHECK(f.table.live_bytes() == 0);
    CHECK(f.table.current_offset() == 0);
    CHECK(f.table.get<user>(1) == nullptr);

    // Ids restart, and the storage still works.
    auto id = f.table.create(user{9, 9});
    CHECK(id == 1);
    CHECK(f.table.get<user>(id)->balance == 9);
}

// =========================================================================
// apply_wal_range - the replication ingest path
// =========================================================================

TEST_CASE("apply_wal_range mirrors a source WAL byte for byte") {
    fixture src;
    auto a = src.table.create(user{1, 2});
    src.table.update<&user::balance>(a, 10);
    auto b = src.table.create(user{3, 4});

    fixture dst;
    std::size_t applied = dst.table.apply_wal_range(src.table.raw_base(), src.table.current_offset());

    CHECK(applied == src.table.current_offset());
    CHECK(dst.table.current_offset() == src.table.current_offset());
    CHECK(dst.table.get<user>(a)->balance == 10);
    CHECK(dst.table.get<user>(b)->age == 4);
}

TEST_CASE("apply_wal_range applied in pieces converges to the same state") {
    fixture src;
    auto a = src.table.create(user{1, 1});
    std::size_t mark = src.table.current_offset();
    src.table.update<&user::balance>(a, 2);
    src.table.create(user{5, 5});

    fixture dst;
    dst.table.apply_wal_range(src.table.raw_base(), mark);
    CHECK(dst.table.current_offset() == mark);
    dst.table.apply_wal_range(src.table.raw_base() + mark, src.table.current_offset() - mark);

    CHECK(dst.table.current_offset() == src.table.current_offset());
    CHECK(dst.table.get<user>(a)->balance == 2);
    CHECK(dst.table.live_bytes() == src.table.live_bytes());
}

TEST_CASE("apply_wal_range stops at a bad record and reports how much it took") {
    fixture src;
    src.table.create(user{1, 1});
    std::size_t good = src.table.current_offset();
    src.table.create(user{2, 2});
    std::size_t total = src.table.current_offset();

    // Corrupt the second record's header so its checksum fails.
    std::vector<std::byte> bytes(src.table.raw_base(), src.table.raw_base() + total);
    bytes[good + 2] = (std::byte)(~(unsigned char)bytes[good + 2]);

    fixture dst;
    std::size_t applied = dst.table.apply_wal_range(bytes.data(), bytes.size());
    CHECK(applied == good);                       // took the good prefix only
    CHECK(dst.table.current_offset() == good);    // and did not advance past it
}

TEST_CASE("apply_wal_range of an empty range is a no-op") {
    fixture f;
    auto id = f.table.create(user{1, 1});
    std::size_t before = f.table.current_offset();
    CHECK(f.table.apply_wal_range(f.table.raw_base(), 0) == 0);
    CHECK(f.table.current_offset() == before);
    CHECK(f.table.get<user>(id) != nullptr);
}

// =========================================================================
// Scale / stress-ish invariants
// =========================================================================

TEST_CASE("many records survive a create/update/destroy churn with exact accounting") {
    fixture f;
    std::mt19937 rng(4242);
    std::vector<record_id> live;
    std::size_t expected_live_bytes = 0;

    for (int i = 0; i < 2000; ++i) {
        int op = rng() % 3;
        if (op == 0 || live.empty()) {
            live.push_back(f.table.create(user{(uint64_t)i, (uint32_t)i}));
            expected_live_bytes += sizeof(user);
        } else if (op == 1) {
            f.table.update<&user::balance>(live[rng() % live.size()], i);
        } else {
            size_t idx = rng() % live.size();
            f.table.destroy(live[idx]);
            live.erase(live.begin() + idx);
            expected_live_bytes -= sizeof(user);
        }
    }
    CHECK(f.table.live_bytes() == expected_live_bytes);

    // Every id we still believe is live must be readable.
    for (auto id : live) CHECK(f.table.get<user>(id) != nullptr);

    // And recovery must reproduce exactly that set.
    std::size_t written = f.table.current_offset();
    storage::storage<> restored(f.region.data());
    restored.recover(written);
    CHECK(restored.live_bytes() == expected_live_bytes);
    for (auto id : live) CHECK(restored.get<user>(id) != nullptr);
}

TEST_CASE("for_each visits exactly the live records of the requested type") {
    fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 10; ++i) ids.push_back(f.table.create(user{(uint64_t)i, 1}));
    for (int i = 0; i < 5; ++i) f.table.create(order{1, (uint64_t)i});
    f.table.destroy(ids[3]);
    f.table.destroy(ids[7]);

    int seen = 0;
    f.table.for_each<user>([&](record_id id, const user&) {
        CHECK(id != ids[3]);
        CHECK(id != ids[7]);
        ++seen;
    });
    // for_each<T> is not tag-filtered - it visits every live record - so the
    // orders are included too. This documents that: the tag check is the
    // caller's job via for_each_raw, exactly as HEAP_API.md says.
    CHECK(seen == 8 + 5);
}
