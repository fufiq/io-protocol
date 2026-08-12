#include <doctest/doctest.h>
#include <memory>
#include "../storage/prefetch_gather.hpp"
#include <thread>
#include <atomic>
#include <sys/mman.h>

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

TEST_CASE("several threads may read at once while no writer runs") {
    // The claim this pins down is narrower than "single-threaded": the whole
    // read path is const - alive(), record_len() and assemble() only read the
    // slot, walk the node arena and memcpy out of the mapping - so readers do
    // not need a writer to be absent from the PROCESS, only from the moment.
    // Verified under ThreadSanitizer separately; this checks the answers.
    fixture f;
    constexpr int n = 2000;
    for (int i = 0; i < n; ++i) f.table->create(quad{(uint64_t)i, 0, 0, 0});
    for (int i = 0; i < n; ++i) f.table->update<&quad::b>(i + 1, 100 + i);
    for (int i = 0; i < n; i += 3) f.table->update<&quad::c>(i + 1, 200 + i);
    // the writer stops here; everything below is readers only

    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            storage::prefetch_gather g(32);         // one gatherer per thread
            std::vector<record_id> ids;
            for (int i = 0; i < n; ++i) ids.push_back((record_id)(((i * 7919 + t * 13) % n) + 1));
            std::vector<std::byte> out(ids.size() * sizeof(quad));
            g.read_many(*f.table, std::span<const record_id>{ids}, out.data(), sizeof(quad));
            for (std::size_t k = 0; k < ids.size(); ++k) {
                quad q{};
                std::memcpy(&q, out.data() + k * sizeof(quad), sizeof(quad));
                if (q.b != 100 + q.a) ++wrong;
                if (q.c != ((q.a % 3 == 0) ? 200 + q.a : 0)) ++wrong;
                if ((record_id)(q.a + 1) != ids[k]) ++wrong;
                auto v = f.table->get<quad>(ids[k]);   // and the single-record path
                if (!v || v->b != 100 + v->a) ++wrong;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(wrong.load() == 0);
    wipe();
}

TEST_CASE("replication: a WAL range applies verbatim and keeps offsets aligned") {
    // apply_wal_range must append the bytes and replay from the LOCAL copy: a
    // slot's base is an offset into this mapping, so applying straight from the
    // caller's buffer would record an offset into memory about to be reused.
    fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 300; ++i) ids.push_back(f.table->create(quad{(uint64_t)i, 0, 0, 0}));
    for (int i = 0; i < 300; ++i) f.table->update<&quad::b>(ids[i], 400 + i);
    for (int i = 0; i < 300; i += 5) f.table->destroy(ids[i]);

    const std::size_t total = f.table->current_offset();

    std::remove("fieldrep.log"); std::remove("fieldrep.idx");
    using replica_spec = storage::field_spec<"fieldrep.log", 1u << 22, "fieldrep.idx", 4096, 8192, 8, 256>;
    auto replica = std::make_unique<storage::field_storage<replica_spec>>();
    REQUIRE(replica->ok());

    // shipped in two chunks, each cut at a record boundary as a shipper would
    const std::size_t half = f.table->for_each_record(0, total / 2, [](auto, auto) {});
    CHECK(replica->apply_wal_range(f.table->raw_base(), half) == half);
    CHECK(replica->apply_wal_range(f.table->raw_base() + half, total - half) == total - half);
    CHECK(replica->current_offset() == total);          // byte-identical logs

    for (int i = 0; i < 300; ++i) {
        auto a = f.table->get<quad>(ids[i]);
        auto b = replica->get<quad>(ids[i]);
        CHECK(a.has_value() == b.has_value());
        if (a) CHECK(a->b == b->b);
    }

    // a corrupted byte stops the replay there rather than applying past it
    auto snap = f.table->export_snapshot();
    REQUIRE(snap.size() > 64);
    snap[snap.size() / 2] ^= std::byte{0xFF};
    replica->reset();
    const std::size_t applied = replica->apply_wal_range(snap.data(), snap.size());
    CHECK(applied > 0);
    CHECK(applied < snap.size());

    replica.reset();
    std::remove("fieldrep.log"); std::remove("fieldrep.idx");
    wipe();
}

TEST_CASE("replication: a snapshot is state, not history, and does not disturb its source") {
    fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 200; ++i) ids.push_back(f.table->create(quad{(uint64_t)i, 0, 0, 0}));
    for (int i = 0; i < 200; ++i) f.table->update<&quad::b>(ids[i], 900 + i);
    for (int i = 0; i < 200; i += 2) f.table->update<&quad::c>(ids[i], 5);
    REQUIRE(f.table->depth(ids[0]) == 2);

    const std::size_t offset_before = f.table->current_offset();
    const std::size_t live_before = f.table->live_bytes();
    const auto snap = f.table->export_snapshot();

    CHECK(f.table->current_offset() == offset_before);   // unlike compact(), nothing moved
    CHECK(f.table->live_bytes() == live_before);
    CHECK(f.table->depth(ids[0]) == 2);
    CHECK(snap.size() < offset_before);                  // chains collapsed away

    std::remove("fieldrep.log"); std::remove("fieldrep.idx");
    using replica_spec = storage::field_spec<"fieldrep.log", 1u << 22, "fieldrep.idx", 4096, 8192, 8, 256>;
    auto replica = std::make_unique<storage::field_storage<replica_spec>>();
    REQUIRE(replica->ok());

    // over a dirty replica, which is the desync path: reset then snapshot
    for (int i = 0; i < 500; ++i) replica->create(quad{7, 7, 7, 7});
    for (int i = 0; i < 500; ++i) replica->update<&quad::b>(i + 1, 7);
    REQUIRE(replica->overrides_used() > 0);
    replica->reset();
    CHECK(replica->current_offset() == 0);
    CHECK(replica->live_bytes() == 0);
    CHECK(replica->overrides_used() == 0);
    CHECK_FALSE(replica->alive(1));

    CHECK(replica->apply_wal_range(snap.data(), snap.size()) == snap.size());
    for (int i = 0; i < 200; ++i) {
        auto a = f.table->get<quad>(ids[i]);
        auto b = replica->get<quad>(ids[i]);
        REQUIRE(b.has_value());
        CHECK(a->b == b->b);
        CHECK(a->c == b->c);
        CHECK(replica->depth(ids[i]) == 0);   // arrived collapsed
    }
    CHECK_FALSE(replica->alive(400));         // the dirty replica's own ids are gone

    replica.reset();
    std::remove("fieldrep.log"); std::remove("fieldrep.idx");
    wipe();
}

TEST_CASE("compaction hands over the FILE, not just the mapping") {
    // The fd used to be closed and left at -1, so a compacted storage no longer
    // owned a file: log_fd() gave a closed descriptor to uring_gather and
    // direct_gather, release_log()'s posix_fadvise silently did nothing, and
    // checkpoint() named an offset in a file it could no longer fsync. The
    // mapping was adopted and the file was dropped.
    fixture f;
    for (int i = 0; i < 200; ++i) f.table->create(quad{(uint64_t)i, 0, 0, 0});
    for (int i = 0; i < 200; ++i) f.table->update<&quad::b>(i + 1, 500 + i);
    const int before = f.table->log_fd();
    REQUIRE(before >= 0);

    const std::string path = "fieldcmp_new.log";
    int nfd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    REQUIRE(nfd >= 0);
    REQUIRE(::posix_fallocate(nfd, 0, 1u << 22) == 0);
    void* fresh = ::mmap(nullptr, 1u << 22, PROT_READ | PROT_WRITE, MAP_SHARED, nfd, 0);
    REQUIRE(fresh != MAP_FAILED);

    f.table->compact((std::byte*)fresh, nfd);

    CHECK(f.table->log_fd() == nfd);
    CHECK(::fcntl(f.table->log_fd(), F_GETFD) != -1);   // still open

    for (int i = 0; i < 200; ++i) {
        auto v = f.table->get<quad>(i + 1);
        REQUIRE(v.has_value());
        CHECK(v->a == (uint64_t)i);
        CHECK(v->b == (uint64_t)(500 + i));
        CHECK(f.table->depth(i + 1) == 0);
    }

    // The paths that used to degrade in silence
    f.table->sync();
    f.table->release_log(f.table->current_offset());
    CHECK(f.table->checkpoint());

    std::size_t creates = 0;
    f.table->for_each_record(0, f.table->current_offset(),
        [&](storage::record_header h, std::span<const std::byte>) {
            if (h.op == storage::record_op::create) ++creates;
        });
    CHECK(creates == 200);

    ::munmap(fresh, 1u << 22);
    std::remove(path.c_str());
    wipe();
}

TEST_CASE("compaction with a scratch that is not a file keeps the old behaviour") {
    // -1 is the default and must still be accepted: several callers compact
    // into a vector or an anonymous mapping, where there is no fd to adopt.
    fixture f;
    for (int i = 0; i < 50; ++i) f.table->create(quad{(uint64_t)i, 0, 0, 0});
    std::vector<std::byte> scratch(1u << 20);
    f.table->compact(scratch.data());
    CHECK(f.table->log_fd() == -1);
    for (int i = 0; i < 50; ++i) CHECK(f.table->get<quad>(i + 1)->a == (uint64_t)i);
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

TEST_CASE("readers run inside the sync window while a writer is committing") {
    // The invariant is chosen so that a TORN read cannot pass it. Every update
    // burst sets b, c and d to the same value derived from a, and only then
    // calls sync(). So a record is self-consistent exactly at the moments the
    // window is open, and a reader that mixed a base from one burst with an
    // override from the next would come back with mismatched fields.
    //
    // That is the whole correctness claim: not "the read did not crash" but
    // "the read saw one instant".
    //
    // Its own spec, with room. Under the shared 4 MB fixture the log filled at
    // round 16, appends started being refused, and an update burst then landed
    // PARTIALLY - a=v with b still at the old value. That reports as a torn
    // read and is nothing of the kind, so the capacity is sized here and
    // refused_append_count() is asserted below to keep the two apart.
    using wspec = storage::field_spec<"t_win.log", 1u << 26, "t_win.idx", 4096, 8192, 3, 256>;
    std::remove(wspec::path.value);
    std::remove(wspec::index_path);
    auto held = std::make_unique<storage::field_storage<wspec>>();
    auto& table = *held;
    REQUIRE(table.ok());

    constexpr int n = 1500;
    for (int i = 0; i < n; ++i) table.create(quad{(uint64_t)i, 0, 0, 0});
    table.sync();

    std::atomic<bool> stop{false};
    std::atomic<long> torn{0}, served{0}, refused{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, t] {
            uint64_t x = 0x9E3779B97F4A7C15ull * (uint64_t)(t + 1);
            while (!stop.load(std::memory_order_relaxed)) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                const record_id id = (record_id)(x % n + 1);
                quad q{};
                if (!table.try_get(id, q)) { ++refused; continue; }
                ++served;
                if (q.b != q.a || q.c != q.a || q.d != q.a) ++torn;
            }
        });
    }

    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < n; ++i) {
            const uint64_t v = (uint64_t)(round * n + i);
            const record_id id = (record_id)(i + 1);
            table.update<&quad::a>(id, v);
            table.update<&quad::b>(id, v);
            table.update<&quad::c>(id, v);
            table.update<&quad::d>(id, v);
        }
        table.sync();          // the only moment readers are allowed in
    }
    stop.store(true);
    for (auto& t : readers) t.join();

    CHECK(torn.load() == 0);
    CHECK(table.read_window_count() >= 40);
    CHECK(table.read_window_open() == false);   // shut again once sync returned
    // Not asserted: that `served` is large. On a machine where msync returns
    // immediately - everything already clean, or a tmpfs - the window is too
    // short to catch and every attempt legitimately refuses. The test is about
    // what a served read contains, not how many there are.
    CHECK(served.load() + refused.load() > 0);
    // If this trips, the log ran out and the "torn" count above is measuring a
    // refused append, not a race.
    CHECK(table.refused_append_count() == 0);
    held.reset();
    std::remove(wspec::path.value);
    std::remove(wspec::index_path);
}

TEST_CASE("a shut window refuses every read") {
    fixture f;
    const record_id id = f.table->create(quad{1, 2, 3, 4});
    f.table->sync();
    CHECK_FALSE(f.table->read_window_open());
    quad q{};
    CHECK_FALSE(f.table->try_get(id, q));   // no commit in flight
    CHECK(f.table->get<quad>(id).has_value()); // the exclusive path is unaffected
    wipe();
}
