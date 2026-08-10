#include <doctest/doctest.h>

#include "../storage/field_storage.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

using storage::record_id;
using storage::tag_t;

namespace {

enum class kind : tag_t { four = 1, two = 2 };

struct quad {
    static constexpr tag_t tag = (tag_t)kind::four;
    uint64_t a, b, c, d;
};
struct pair {
    static constexpr tag_t tag = (tag_t)kind::two;
    uint64_t x, y;
};
static_assert(storage::tagged_aggregate<quad>);
static_assert(storage::tagged_aggregate<pair>);

// max_depth 3: a fourth distinct field pushes past it and collapses.
using fspec = storage::field_spec<"t_field.log", 1u << 22, "t_field.idx", 4096, 4096, 3, 256, "t_field.ck">;

void wipe() {
    std::remove(fspec::path.value);
    std::remove(fspec::index_path);
    std::remove(fspec::checkpoint_path);
}

struct fixture {
    fixture() { wipe(); table = new storage::field_storage<fspec>(); REQUIRE(table->ok()); }
    ~fixture() { delete table; wipe(); }
    storage::field_storage<fspec>* table = nullptr;
};

} // namespace

TEST_CASE("create and read back through the gather") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    quad got{};
    REQUIRE(f.table->get(id, got));
    CHECK(got.a == 1); CHECK(got.b == 2); CHECK(got.c == 3); CHECK(got.d == 4);
    CHECK(f.table->depth(id) == 0);        // nothing overridden yet
    CHECK(f.table->tag_of(id) == quad::tag);
    CHECK(f.table->live_bytes() == sizeof(quad));
}

TEST_CASE("an update overrides one field and leaves the rest alone") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    f.table->update<&quad::b>(id, 99);

    quad got{};
    REQUIRE(f.table->get(id, got));
    CHECK(got.a == 1); CHECK(got.b == 99); CHECK(got.c == 3); CHECK(got.d == 4);
    CHECK(f.table->depth(id) == 1);
    CHECK(f.table->live_bytes() == sizeof(quad)); // an override is not new live data
}

TEST_CASE("updating the same field again replaces its node instead of growing the chain") {
    // This is what keeps a chain bounded by DISTINCT fields rather than by
    // update count - without it a hot counter would deepen forever.
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    for (int i = 0; i < 50; ++i) f.table->update<&quad::a>(id, (uint64_t)i);

    CHECK(f.table->depth(id) == 1);
    CHECK(f.table->overrides_used() == 1);
    CHECK(f.table->collapse_count() == 0); // never crossed max_depth
    quad got{};
    REQUIRE(f.table->get(id, got));
    CHECK(got.a == 49);
    CHECK(got.b == 2);
}

TEST_CASE("crossing max_depth collapses the record automatically") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    f.table->update<&quad::a>(id, 10);
    f.table->update<&quad::b>(id, 20);
    f.table->update<&quad::c>(id, 30);
    CHECK(f.table->depth(id) == 3);          // still at the limit
    CHECK(f.table->collapse_count() == 0);

    f.table->update<&quad::d>(id, 40);       // a fourth field crosses it
    CHECK(f.table->depth(id) == 0);          // chain dropped
    CHECK(f.table->collapse_count() == 1);

    quad got{};
    REQUIRE(f.table->get(id, got));
    CHECK(got.a == 10); CHECK(got.b == 20); CHECK(got.c == 30); CHECK(got.d == 40);
    CHECK(f.table->live_bytes() == sizeof(quad));
}

TEST_CASE("an explicit collapse is a no-op on a record with no chain") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    const std::size_t before = f.table->current_offset();
    f.table->collapse(id);
    CHECK(f.table->current_offset() == before);
    CHECK(f.table->collapse_count() == 0);
}

TEST_CASE("collapsed nodes are reused, not leaked") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    for (int round = 0; round < 10; ++round) {
        f.table->update<&quad::a>(id, (uint64_t)round);
        f.table->update<&quad::b>(id, (uint64_t)round);
        f.table->update<&quad::c>(id, (uint64_t)round);
        f.table->update<&quad::d>(id, (uint64_t)round); // collapses each round
    }
    CHECK(f.table->collapse_count() == 10);
    // Four nodes at a time, freed and taken again - the high-water mark must
    // not climb with the number of rounds.
    CHECK(f.table->overrides_used() <= 4);
}

TEST_CASE("destroy removes the record and frees its chain") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    f.table->update<&quad::a>(id, 7);
    f.table->update<&quad::b>(id, 8);
    f.table->destroy(id);

    quad got{};
    CHECK_FALSE(f.table->get(id, got));
    CHECK(f.table->tag_of(id) == 0);
    CHECK(f.table->live_bytes() == 0);
    CHECK_FALSE(f.table->alive(id));
    // The nodes went back on the free list.
    auto other = f.table->create(quad{9, 9, 9, 9});
    f.table->update<&quad::a>(other, 1);
    f.table->update<&quad::b>(other, 2);
    CHECK(f.table->overrides_used() <= 2);
}

TEST_CASE("recreate replaces the record and drops its overrides") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    f.table->update<&quad::a>(id, 111);
    CHECK(f.table->depth(id) == 1);

    f.table->recreate(id, pair{5, 6});
    CHECK(f.table->depth(id) == 0);
    CHECK(f.table->tag_of(id) == pair::tag);
    pair got{};
    REQUIRE(f.table->get(id, got));
    CHECK(got.x == 5); CHECK(got.y == 6);
    quad wrong{};
    CHECK_FALSE(f.table->get(id, wrong)); // the length no longer matches quad
}

TEST_CASE("patch checks its range and refuses to leave the record") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    const std::size_t before = f.table->current_offset();

    uint64_t v = 42;
    auto bytes = std::as_bytes(std::span{&v, 1});
    f.table->patch(id, sizeof(quad), bytes);       // starts at the end
    f.table->patch(id, sizeof(quad) - 4, bytes);   // overruns by 4
    f.table->patch(id, 4096, bytes);               // far outside
    f.table->patch(9999, 0, bytes);                // no such record
    CHECK(f.table->current_offset() == before);    // nothing was logged

    f.table->patch(id, offsetof(quad, c), bytes);  // this one fits
    quad got{};
    REQUIRE(f.table->get(id, got));
    CHECK(got.c == 42);
    CHECK(got.d == 4);
}

TEST_CASE("recover rebuilds the base records and every chain") {
    wipe();
    std::vector<record_id> ids;
    std::size_t written, live;
    {
        storage::field_storage<fspec> t;
        REQUIRE(t.ok());
        for (int i = 0; i < 60; ++i) ids.push_back(t.create(quad{(uint64_t)i, 0, 0, 0}));
        for (int i = 0; i < 60; i += 2) t.update<&quad::b>(ids[i], 500 + i);
        for (int i = 0; i < 60; i += 3) t.update<&quad::c>(ids[i], 900 + i);
        for (int i = 0; i < 60; i += 7) t.destroy(ids[i]);
        t.recreate(ids[1], pair{77, 88});
        written = t.current_offset();
        live = t.live_bytes();
        t.sync();
    }
    storage::field_storage<fspec> restored;
    REQUIRE(restored.ok());
    CHECK(restored.recover(fspec::capacity) == written);
    CHECK(restored.live_bytes() == live);

    for (int i = 0; i < 60; ++i) {
        if (i % 7 == 0) { CHECK_FALSE(restored.alive(ids[i])); continue; }
        if (i == 1) {
            pair p{};
            REQUIRE(restored.get(ids[1], p));
            CHECK(p.x == 77);
            continue;
        }
        quad got{};
        REQUIRE(restored.get(ids[i], got));
        CHECK(got.a == (uint64_t)i);
        CHECK(got.b == (i % 2 == 0 ? (uint64_t)(500 + i) : 0));
        CHECK(got.c == (i % 3 == 0 ? (uint64_t)(900 + i) : 0));
    }
    wipe();
}

TEST_CASE("recover stops at a corrupted record and keeps everything before it") {
    wipe();
    record_id a, b;
    std::size_t after_a;
    {
        storage::field_storage<fspec> t;
        a = t.create(quad{1, 1, 1, 1});
        after_a = t.current_offset();
        b = t.create(quad{2, 2, 2, 2});
        t.sync();
    }
    {
        int fd = ::open(fspec::path.value, O_RDWR);
        REQUIRE(fd >= 0);
        uint64_t garbage = 0xDEADBEEFDEADBEEFull;
        REQUIRE(::pwrite(fd, &garbage, sizeof(garbage), after_a + sizeof(storage::record_header)) == (ssize_t)sizeof(garbage));
        ::fsync(fd); ::close(fd);
    }
    storage::field_storage<fspec> restored;
    CHECK(restored.recover(fspec::capacity) == after_a);
    CHECK(restored.alive(a));
    CHECK_FALSE(restored.alive(b));
    wipe();
}

TEST_CASE("a clean shutdown restarts without replaying, and matches a replay exactly") {
    wipe();
    std::vector<record_id> ids;
    std::size_t written, live;
    {
        storage::field_storage<fspec> t;
        for (int i = 0; i < 80; ++i) ids.push_back(t.create(quad{(uint64_t)i, 0, 0, 0}));
        for (int i = 0; i < 80; i += 2) t.update<&quad::b>(ids[i], 1000 + i);
        written = t.current_offset();
        live = t.live_bytes();
        REQUIRE(t.checkpoint());
    }
    {
        storage::field_storage<fspec> fast;
        CHECK(fast.restart(fspec::capacity) == written);
        CHECK(fast.started_from_checkpoint());
        CHECK(fast.live_bytes() == live);
        for (int i = 0; i < 80; ++i) {
            quad got{};
            REQUIRE(fast.get(ids[i], got));
            CHECK(got.a == (uint64_t)i);
            CHECK(got.b == (i % 2 == 0 ? (uint64_t)(1000 + i) : 0));
        }
    }
    std::remove(fspec::checkpoint_path);
    {
        storage::field_storage<fspec> slow;
        CHECK(slow.recover(fspec::capacity) == written);
        CHECK_FALSE(slow.started_from_checkpoint());
        CHECK(slow.live_bytes() == live);
        quad got{};
        REQUIRE(slow.get(ids[10], got));
        CHECK(got.b == 1010);
    }
    wipe();
}

TEST_CASE("a checkpoint is refused once anything was written after it") {
    wipe();
    record_id a, b;
    std::size_t after_both;
    {
        storage::field_storage<fspec> t;
        a = t.create(quad{1, 1, 1, 1});
        REQUIRE(t.checkpoint());
        b = t.create(quad{2, 2, 2, 2});
        t.sync();
        after_both = t.current_offset();
    }
    storage::field_storage<fspec> restored;
    CHECK(restored.restart(fspec::capacity) == after_both);
    CHECK_FALSE(restored.started_from_checkpoint());
    CHECK(restored.alive(a));
    CHECK(restored.alive(b)); // the write past the checkpoint survived the replay
    wipe();
}

TEST_CASE("compaction collapses every chain and reclaims the update history") {
    fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 100; ++i) ids.push_back(f.table->create(quad{(uint64_t)i, 0, 0, 0}));
    for (int i = 0; i < 100; ++i) f.table->update<&quad::b>(ids[i], 7000 + i);
    for (int i = 0; i < 100; i += 2) f.table->destroy(ids[i]);

    const std::size_t before = f.table->current_offset();
    const std::size_t live = f.table->live_bytes();
    CHECK(f.table->depth(ids[1]) == 1);

    std::vector<std::byte> scratch(1u << 20);
    f.table->compact(scratch.data());

    CHECK(f.table->live_bytes() == live);
    CHECK(f.table->current_offset() < before);
    CHECK(f.table->overrides_used() == 0);       // every chain went away
    for (int i = 1; i < 100; i += 2) {
        CHECK(f.table->depth(ids[i]) == 0);      // one contiguous access again
        quad got{};
        REQUIRE(f.table->get(ids[i], got));
        CHECK(got.a == (uint64_t)i);
        CHECK(got.b == (uint64_t)(7000 + i));
    }
    for (int i = 0; i < 100; i += 2) CHECK_FALSE(f.table->alive(ids[i]));
}

TEST_CASE("for_each and for_each_raw see assembled records") {
    fixture f;
    auto a = f.table->create(quad{1, 2, 3, 4});
    auto b = f.table->create(pair{5, 6});
    f.table->update<&quad::c>(a, 300);

    int quads = 0;
    f.table->for_each<quad>([&](record_id id, const quad& q) {
        CHECK(id == a);
        CHECK(q.c == 300);   // the override is folded in
        ++quads;
    });
    CHECK(quads == 1);

    int seen_quad = 0, seen_pair = 0;
    f.table->for_each_raw([&](storage::field_storage<fspec>::raw_view rv) {
        if (rv.tag == quad::tag) { ++seen_quad; CHECK(rv.bytes.size() == sizeof(quad)); }
        else if (rv.tag == pair::tag) { ++seen_pair; }
    });
    CHECK(seen_quad == 1);
    CHECK(seen_pair == 1);
    (void)b;
}

TEST_CASE("for_each_record walks the log without applying anything") {
    fixture f;
    auto id = f.table->create(quad{1, 2, 3, 4});
    const std::size_t mark = f.table->current_offset();
    f.table->update<&quad::a>(id, 9);
    f.table->destroy(id);

    std::vector<storage::record_op> ops;
    f.table->for_each_record(mark, [&](const storage::record_header& hdr, std::span<const std::byte>) {
        ops.push_back(hdr.op);
    });
    REQUIRE(ops.size() == 2);
    CHECK(ops[0] == storage::record_op::update);
    CHECK(ops[1] == storage::record_op::destroy);
}

TEST_CASE("running out of override nodes keeps the old value rather than a wrong one") {
    // A tiny node arena, so it is exhausted deliberately. The record must keep
    // reading correctly - just without the update that could not be recorded.
    using tiny = storage::field_spec<"t_field_tiny.log", 1u << 20, "t_field_tiny.idx", 256, 2, 8, 256>;
    std::remove(tiny::path.value); std::remove(tiny::index_path);
    {
        storage::field_storage<tiny> t;
        REQUIRE(t.ok());
        auto a = t.create(quad{1, 2, 3, 4});
        auto b = t.create(quad{5, 6, 7, 8});
        t.update<&quad::a>(a, 10);
        t.update<&quad::b>(a, 20);   // two nodes used, the arena is now full
        t.update<&quad::c>(b, 30);   // refused

        quad got{};
        REQUIRE(t.get(a, got));
        CHECK(got.a == 10); CHECK(got.b == 20);
        REQUIRE(t.get(b, got));
        CHECK(got.c == 7);           // the old value, not garbage
        CHECK(got.a == 5);
    }
    std::remove(tiny::path.value); std::remove(tiny::index_path);
}
