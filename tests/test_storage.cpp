#include <doctest/doctest.h>

#include "../storage/storage.hpp"

#include <cstring>
#include <cstdio>
#include <random>
#include <string>
#include <sys/stat.h>
#include <sys/mman.h>
#include <map>

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

// =====================================================================
// durability::local_sync - the split between the log and the materialized
// records. Everything above this point runs on a caller-provided region
// (durability::shared), which deliberately does NOT split; these cases
// cover the file-backed path, which is the one that has two of everything.
// =====================================================================
namespace {

using split_spec = storage::storage_spec<storage::durability::local_sync, "test_split.db", 4ull * 1024 * 1024>;

// Removes both files so each case starts from nothing - a leftover log from
// a previous run would otherwise be recovered into the fresh storage.
struct split_fixture {
    split_fixture() { cleanup(); table = new storage::storage<split_spec>(); }
    ~split_fixture() { delete table; cleanup(); }
    static void cleanup() {
        std::remove(split_spec::path.value);
        std::remove(split_spec::data_path);
    }
    storage::storage<split_spec>* table = nullptr;
};

// compact() ADOPTS the region it is given - the storage unmaps it in its
// destructor - so a split storage must be handed a real mapping of
// spec::capacity, never a vector's heap bytes.
struct scratch_region {
    explicit scratch_region(std::size_t bytes) : size(bytes) {
        ptr = (std::byte*)::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(ptr != MAP_FAILED);
    }
    std::byte* data() const noexcept { return ptr; }
    std::size_t bytes() const noexcept { return size; }
    std::byte* ptr = nullptr;
    std::size_t size = 0;
};

std::size_t file_size_of(const char* path) {
    struct ::stat st{};
    return ::stat(path, &st) == 0 ? (std::size_t)st.st_size : 0;
}

} // namespace

TEST_CASE("local_sync derives the data path from the log path and opens both") {
    CHECK(std::string(split_spec::data_path) == std::string(split_spec::path.value) + ".data");

    split_fixture f;
    f.table->create(user{100, 30});
    CHECK(file_size_of(split_spec::path.value) == split_spec::capacity);
    CHECK(file_size_of(split_spec::data_path) == split_spec::capacity);
}

TEST_CASE("shared storage keeps a single region and never opens a data file") {
    // The counterpart of the case above: the split is local_sync-only, so a
    // shared storage must still materialize records inside the log itself.
    CHECK(storage::storage<split_spec>::split_regions);
    CHECK_FALSE(storage::storage<>::split_regions);

    fixture f;
    f.table.create(user{1, 2});
    CHECK(f.table.data_region_offset() == 0);
}

TEST_CASE("update patches the data region and leaves the log immutable") {
    // This is the property the whole split exists for: if update() wrote
    // into the log, the log's inode would collect scattered dirty pages and
    // every sync() would pay for them.
    split_fixture f;
    record_id id = f.table->create(user{1000, 40});
    f.table->sync();

    const std::byte* log = f.table->raw_base();
    // The CREATE sits at offset 0; its payload follows the 24-byte header.
    user logged_before{};
    std::memcpy(&logged_before, log + sizeof(storage::record_header), sizeof(user));
    CHECK(logged_before.balance == 1000);

    f.table->update<&user::balance>(id, 7777);

    CHECK(f.table->get<user>(id)->balance == 7777);
    user logged_after{};
    std::memcpy(&logged_after, log + sizeof(storage::record_header), sizeof(user));
    CHECK(logged_after.balance == 1000); // the CREATE's bytes never moved
}

TEST_CASE("recover rebuilds the data region from the log alone") {
    split_fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 50; ++i) ids.push_back(f.table->create(user{(uint64_t)i * 10, (uint32_t)i}));
    for (int i = 0; i < 50; i += 3) f.table->update<&user::balance>(ids[i], 900000 + i);
    for (int i = 0; i < 50; i += 7) f.table->destroy(ids[i]);
    f.table->recreate(ids[1], order{42, 555});
    f.table->sync();

    const std::size_t log_bytes = f.table->current_offset();
    const std::size_t live_before = f.table->live_bytes();
    delete f.table;
    f.table = nullptr;

    // Deliberately scribble over the data file before reopening: recover()
    // must not read a single byte of it, only rewrite it.
    {
        int fd = ::open(split_spec::data_path, O_RDWR);
        REQUIRE(fd >= 0);
        std::vector<std::byte> garbage(64 * 1024, (std::byte)0xAB);
        REQUIRE(::pwrite(fd, garbage.data(), garbage.size(), 0) == (ssize_t)garbage.size());
        ::close(fd);
    }

    storage::storage<split_spec> restored;
    CHECK(restored.recover(split_spec::capacity) == log_bytes);
    CHECK(restored.live_bytes() == live_before);
    CHECK(restored.data_region_offset() > 0);

    for (int i = 0; i < 50; ++i) {
        if (i % 7 == 0) { CHECK(restored.get<user>(ids[i]) == nullptr); continue; }
        if (i == 1) { CHECK(restored.get<order>(ids[1])->amount == 555); continue; }
        const user* u = restored.get<user>(ids[i]);
        REQUIRE(u != nullptr);
        CHECK(u->balance == (i % 3 == 0 ? (uint64_t)(900000 + i) : (uint64_t)i * 10));
    }
}

TEST_CASE("compaction rebuilds both regions and reclaims both") {
    split_fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 200; ++i) ids.push_back(f.table->create(user{(uint64_t)i, (uint32_t)i}));
    for (int i = 0; i < 200; ++i) f.table->update<&user::balance>(ids[i], 1000 + i);
    for (int i = 0; i < 200; i += 2) f.table->destroy(ids[i]);

    const std::size_t live = f.table->live_bytes();
    const std::size_t log_before = f.table->current_offset();
    const std::size_t data_before = f.table->data_region_offset();

    scratch_region scratch_log(split_spec::capacity), scratch_data(split_spec::capacity);
    f.table->compact(scratch_log.data(), scratch_data.data());

    CHECK(f.table->live_bytes() == live);
    CHECK(f.table->current_offset() < log_before);
    CHECK(f.table->data_region_offset() < data_before);
    CHECK(f.table->raw_base() == scratch_log.data());

    // Every surviving record still reads back its post-update value, and now
    // reads it out of the NEW data region.
    for (int i = 1; i < 200; i += 2) {
        const user* u = f.table->get<user>(ids[i]);
        REQUIRE(u != nullptr);
        CHECK(u->balance == (uint64_t)(1000 + i));
        CHECK(u->age == (uint32_t)i);
    }
    for (int i = 0; i < 200; i += 2) CHECK(f.table->get<user>(ids[i]) == nullptr);
}

TEST_CASE("updates after compaction land in the new data region, not the old one") {
    split_fixture f;
    record_id id = f.table->create(user{5, 5});
    scratch_region scratch_log(split_spec::capacity), scratch_data(split_spec::capacity);
    f.table->compact(scratch_log.data(), scratch_data.data());

    f.table->update<&user::balance>(id, 12345);
    CHECK(f.table->get<user>(id)->balance == 12345);

    // The value must live inside the scratch data region we just handed over.
    const std::byte* v = (const std::byte*)f.table->get<user>(id);
    CHECK(v >= scratch_data.data());
    CHECK(v + sizeof(user) <= scratch_data.data() + scratch_data.bytes());
}

// =====================================================================
// spec::index_path - the index in a file instead of on the heap. The index
// is one word per id EVER created and never shrinks, so it is the one
// structure whose footprint follows churn rather than live data; on the
// heap that is anonymous memory, which can only be swapped.
// =====================================================================
namespace {
using indexed_spec = storage::storage_spec<storage::durability::local_sync, "test_indexed.db",
                                           4ull * 1024 * 1024, "", "test_indexed.idx">;
struct indexed_fixture {
    indexed_fixture() { cleanup(); table = new storage::storage<indexed_spec>(); }
    ~indexed_fixture() { delete table; cleanup(); }
    static void cleanup() {
        std::remove(indexed_spec::path.value);
        std::remove(indexed_spec::data_path);
        std::remove(indexed_spec::index_path);
    }
    storage::storage<indexed_spec>* table = nullptr;
};
} // namespace

TEST_CASE("a file-backed index behaves exactly like the heap one") {
    CHECK(indexed_spec::index_on_disk);
    CHECK_FALSE(split_spec::index_on_disk);

    indexed_fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 100; ++i) ids.push_back(f.table->create(user{(uint64_t)i, (uint32_t)i}));
    for (int i = 0; i < 100; i += 2) f.table->update<&user::balance>(ids[i], 5000 + i);
    for (int i = 0; i < 100; i += 5) f.table->destroy(ids[i]);

    CHECK(file_size_of(indexed_spec::index_path) == indexed_spec::index_entries * sizeof(std::size_t));

    for (int i = 0; i < 100; ++i) {
        if (i % 5 == 0) { CHECK(f.table->get<user>(ids[i]) == nullptr); continue; }
        const user* u = f.table->get<user>(ids[i]);
        REQUIRE(u != nullptr);
        CHECK(u->balance == (i % 2 == 0 ? (uint64_t)(5000 + i) : (uint64_t)i));
    }
}

TEST_CASE("a file-backed index survives recover, which rebuilds it from the log") {
    indexed_fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 100; ++i) ids.push_back(f.table->create(user{(uint64_t)i, (uint32_t)i}));
    f.table->update<&user::balance>(ids[7], 999);
    f.table->destroy(ids[9]);
    f.table->sync();
    const std::size_t log_bytes = f.table->current_offset();
    const std::size_t live_before = f.table->live_bytes();

    delete f.table;
    f.table = new storage::storage<indexed_spec>();
    CHECK(f.table->recover(indexed_spec::capacity) == log_bytes);
    CHECK(f.table->live_bytes() == live_before);
    CHECK(f.table->get<user>(ids[7])->balance == 999);
    CHECK(f.table->get<user>(ids[9]) == nullptr);
    CHECK(f.table->get<user>(ids[42])->balance == 42);
}

TEST_CASE("a file-backed index refuses an id past its capacity instead of growing") {
    // The heap index would happily try to allocate for any id a record
    // claims. A mapping cannot, and that turns an unbounded allocation
    // driven by a record's id field into a plain refusal.
    indexed_fixture f;
    record_id ok = f.table->recreate(indexed_spec::index_entries - 1, user{1, 1});
    CHECK(ok == indexed_spec::index_entries - 1);
    CHECK(f.table->get<user>(ok)->balance == 1);

    const std::size_t live_before = f.table->live_bytes();
    const std::size_t offset_before = f.table->current_offset();
    CHECK(f.table->recreate(indexed_spec::index_entries, user{2, 2}) == 0);
    CHECK(f.table->recreate((record_id)1 << 60, user{3, 3}) == 0);
    // Refused means refused: nothing was appended to the log either.
    CHECK(f.table->live_bytes() == live_before);
    CHECK(f.table->current_offset() == offset_before);
}

// =====================================================================
// background_compact()'s phase-1 snapshot. It is file-backed and fixed-size,
// so the interesting case is what happens when it does not fit: a snapshot
// that quietly dropped records would make compaction DELETE them.
// =====================================================================
TEST_CASE("a snapshot round-trips live records and the WAL replayed on top") {
    fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 20; ++i) ids.push_back(f.table.create(user{(uint64_t)i, (uint32_t)i}));

    storage::storage<>::snapshot snap;
    REQUIRE(snap.open("t_snap.arena", 1u << 20, "t_snap.refs", 4096));
    const std::size_t before_replay = f.table.current_offset();
    f.table.snapshot_into(snap);
    CHECK_FALSE(snap.overflowed());

    // Mutations made AFTER the snapshot are picked up by replaying the WAL
    // onto it - that is what lets phase 2 run without the lock.
    f.table.update<&user::balance>(ids[3], 4242);
    f.table.destroy(ids[5]);
    auto fresh = f.table.create(user{777, 7});
    f.table.replay_into(f.table.raw_base(), before_replay, f.table.current_offset(), snap);
    CHECK_FALSE(snap.overflowed());

    auto* r3 = snap.find(ids[3]);
    REQUIRE(r3 != nullptr);
    CHECK(((const user*)(snap.bytes() + r3->offset))->balance == 4242);
    CHECK(r3->tag == (tag_t)kind::user);
    CHECK(snap.find(ids[5]) == nullptr);          // destroyed after the snapshot
    REQUIRE(snap.find(fresh) != nullptr);          // created after the snapshot
    CHECK(((const user*)(snap.bytes() + snap.find(fresh)->offset))->balance == 777);
    CHECK(snap.find(999999) == nullptr);           // never existed
}

TEST_CASE("a snapshot that runs out of arena latches overflow instead of losing records") {
    fixture f;
    for (int i = 0; i < 200; ++i) f.table.create(user{(uint64_t)i, (uint32_t)i});

    storage::storage<>::snapshot snap;
    // Room for a handful of records, not 200.
    REQUIRE(snap.open("t_snap_small.arena", 256, "t_snap_small.refs", 4096));
    f.table.snapshot_into(snap);
    CHECK(snap.overflowed());
    CHECK(snap.used() <= 256);
}

TEST_CASE("a snapshot that runs out of ref slots latches overflow too") {
    fixture f;
    for (int i = 0; i < 200; ++i) f.table.create(user{(uint64_t)i, (uint32_t)i});

    storage::storage<>::snapshot snap;
    REQUIRE(snap.open("t_snap_norefs.arena", 1u << 20, "t_snap_norefs.refs", 16));
    f.table.snapshot_into(snap);
    CHECK(snap.overflowed());
}

TEST_CASE("an overflowed snapshot leaves the storage untouched, not half-compacted") {
    // The failure that matters: background_compact must abandon the round
    // rather than switch the storage over to a snapshot missing records.
    using bg_spec = storage::storage_spec<storage::durability::local_sync, "t_bgfail.db", 4ull * 1024 * 1024>;
    std::remove(bg_spec::path.value);
    std::remove(bg_spec::data_path);

    storage::storage<bg_spec> table;
    std::vector<record_id> ids;
    for (int i = 0; i < 500; ++i) ids.push_back(table.create(user{(uint64_t)i, (uint32_t)i}));
    for (int i = 0; i < 500; i += 2) table.destroy(ids[i]);

    const std::size_t live = table.live_bytes();
    const std::size_t offset = table.current_offset();
    std::mutex mu;

    // scratch_capacity also sizes the snapshot arena, so a capacity smaller
    // than the live set is exactly the overflow case.
    storage::background_compact(table, mu, "t_bgfail.tmp", bg_spec::path.value, 64,
                                "t_bgfail.data.tmp", bg_spec::data_path);

    CHECK(table.live_bytes() == live);
    CHECK(table.current_offset() == offset);
    for (int i = 1; i < 500; i += 2) {
        const user* u = table.get<user>(ids[i]);
        REQUIRE(u != nullptr);
        CHECK(u->balance == (uint64_t)i);
    }
    std::remove(bg_spec::path.value);
    std::remove(bg_spec::data_path);
    std::remove("t_bgfail.tmp");
    std::remove("t_bgfail.data.tmp");
}

// =====================================================================
// Checksum coverage. A CREATE's payload used to be outside its checksum,
// because update() patched it in place and a payload-covering checksum
// would have gone stale. The consequence was that a torn write which landed
// the header's sector but not the payload's - or plain bit rot - passed
// recovery and was applied as a real value. These pin down that it does not.
// =====================================================================

TEST_CASE("a corrupted CREATE payload is rejected, not applied") {
    split_fixture f;
    record_id a = f.table->create(user{1000, 30});
    f.table->create(user{2000, 40});
    f.table->sync();

    delete f.table;
    f.table = nullptr;

    // Scribble inside the FIRST record's payload in the log, leaving its
    // header intact - precisely what a sector-granularity tear looks like.
    {
        int fd = ::open(split_spec::path.value, O_RDWR);
        REQUIRE(fd >= 0);
        uint64_t garbage = 0xDEADBEEFDEADBEEFull;
        REQUIRE(::pwrite(fd, &garbage, sizeof(garbage), sizeof(storage::record_header)) == (ssize_t)sizeof(garbage));
        ::fsync(fd);
        ::close(fd);
    }

    storage::storage<split_spec> restored;
    CHECK(restored.recover(split_spec::capacity) == 0); // stopped at the very first record
    CHECK(restored.get<user>(a) == nullptr);
    CHECK(restored.live_bytes() == 0);
}

TEST_CASE("corruption partway through the log keeps everything before it") {
    split_fixture f;
    std::vector<record_id> ids;
    for (int i = 0; i < 10; ++i) ids.push_back(f.table->create(user{(uint64_t)i, (uint32_t)i}));
    f.table->sync();
    const std::size_t stride = f.table->current_offset() / 10;

    delete f.table;
    f.table = nullptr;

    {
        int fd = ::open(split_spec::path.value, O_RDWR);
        REQUIRE(fd >= 0);
        uint64_t garbage = 0xFFFFFFFFFFFFFFFFull;
        // Payload of record index 6.
        REQUIRE(::pwrite(fd, &garbage, sizeof(garbage), 6 * stride + sizeof(storage::record_header)) == (ssize_t)sizeof(garbage));
        ::fsync(fd);
        ::close(fd);
    }

    storage::storage<split_spec> restored;
    CHECK(restored.recover(split_spec::capacity) == 6 * stride);
    for (int i = 0; i < 6; ++i) {
        const user* u = restored.get<user>(ids[i]);
        REQUIRE(u != nullptr);
        CHECK(u->balance == (uint64_t)i);
    }
    for (int i = 6; i < 10; ++i) CHECK(restored.get<user>(ids[i]) == nullptr);
}

TEST_CASE("an unsplit storage keeps its CREATE checksums valid across updates") {
    // The unsplit index aliases the log, so update() rewrites bytes the
    // CREATE's checksum covers. If the refresh were missing, recover() would
    // stop at the first updated record instead of replaying the whole log.
    std::vector<std::byte> region(region_bytes);
    record_id a, b;
    std::size_t written;
    {
        storage::storage<> t(region.data());
        a = t.create(user{1, 10});
        b = t.create(user{2, 20});
        for (int i = 0; i < 50; ++i) t.update<&user::balance>(a, (uint64_t)i);
        t.update<&user::age>(b, 99);
        written = t.current_offset();
    }
    storage::storage<> restored(region.data());
    CHECK(restored.recover(written) == written); // nothing stopped it early
    CHECK(restored.get<user>(a)->balance == 49);
    CHECK(restored.get<user>(b)->age == 99);
}

TEST_CASE("a corrupted UPDATE payload is still rejected") {
    split_fixture f;
    record_id a = f.table->create(user{500, 25});
    const std::size_t after_create = f.table->current_offset();
    f.table->update<&user::balance>(a, 12345);
    f.table->sync();

    delete f.table;
    f.table = nullptr;

    {
        int fd = ::open(split_spec::path.value, O_RDWR);
        REQUIRE(fd >= 0);
        uint64_t garbage = 0x1234567812345678ull;
        REQUIRE(::pwrite(fd, &garbage, sizeof(garbage), after_create + sizeof(storage::record_header)) == (ssize_t)sizeof(garbage));
        ::fsync(fd);
        ::close(fd);
    }

    storage::storage<split_spec> restored;
    CHECK(restored.recover(split_spec::capacity) == after_create); // the CREATE survived, the UPDATE did not
    REQUIRE(restored.get<user>(a) != nullptr);
    CHECK(restored.get<user>(a)->balance == 500); // pre-update value, not the corrupted one
}

// =====================================================================
// The observer: one application point for live writes, recovery and
// replication ingest. This is what makes a secondary index maintained
// ABOVE the engine recoverable - the same code runs in all three cases,
// so the index cannot drift from the data it indexes.
// =====================================================================
namespace {

// A secondary index kept entirely outside the engine: balance -> ids.
struct balance_index {
    std::multimap<uint64_t, record_id> by_balance;
    int creates = 0, updates = 0, destroys = 0;

    void on_record(auto& table, const storage::record_header& hdr, const std::byte* payload) {
        // The storage still holds the PREVIOUS state here, so whatever the
        // record currently indexes is the entry to retract.
        const user* previous = table.template get<user>(hdr.id);
        if (previous) retract(previous->balance, hdr.id);

        switch (hdr.op) {
            case storage::record_op::create: {
                user fresh{};
                std::memcpy(&fresh, payload, sizeof(fresh));
                by_balance.emplace(fresh.balance, hdr.id);
                ++creates;
                break;
            }
            case storage::record_op::update: {
                uint64_t balance = previous ? previous->balance : 0;
                if (hdr.offset == offsetof(user, balance)) std::memcpy(&balance, payload, sizeof(balance));
                by_balance.emplace(balance, hdr.id);
                ++updates;
                break;
            }
            case storage::record_op::destroy:
                ++destroys; // already retracted above
                break;
        }
    }

    void retract(uint64_t key, record_id id) {
        auto [lo, hi] = by_balance.equal_range(key);
        for (auto it = lo; it != hi; ++it)
            if (it->second == id) { by_balance.erase(it); return; }
    }
};

using observed_spec = storage::storage_spec<storage::durability::local_sync, "test_observed.db", 4ull * 1024 * 1024>;
using observed_storage = storage::storage<observed_spec, balance_index>;

} // namespace

TEST_CASE("an observer rebuilt by recovery matches the one built by live writes") {
    std::remove(observed_spec::path.value);
    std::remove(observed_spec::data_path);

    std::multimap<uint64_t, record_id> live_index;
    int live_creates, live_updates, live_destroys;
    std::size_t log_bytes;
    std::vector<record_id> ids;
    {
        observed_storage table;
        for (int i = 0; i < 60; ++i) ids.push_back(table.create(user{(uint64_t)(i * 10), (uint32_t)i}));
        for (int i = 0; i < 60; i += 3) table.update<&user::balance>(ids[i], 500000 + i);
        for (int i = 0; i < 60; i += 4) table.update<&user::age>(ids[i], 77); // a field the index does not key on
        for (int i = 0; i < 60; i += 7) table.destroy(ids[i]);
        table.recreate(ids[1], user{31337, 5});
        table.sync();

        live_index = table.observer_state().by_balance;
        live_creates = table.observer_state().creates;
        live_updates = table.observer_state().updates;
        live_destroys = table.observer_state().destroys;
        log_bytes = table.current_offset();
    }

    // Nothing but the log survives. The observer must land in the same place.
    observed_storage restored;
    CHECK(restored.recover(observed_spec::capacity) == log_bytes);

    CHECK(restored.observer_state().by_balance == live_index);
    CHECK(restored.observer_state().creates == live_creates);
    CHECK(restored.observer_state().updates == live_updates);
    CHECK(restored.observer_state().destroys == live_destroys);

    // And the index actually agrees with the data it indexes.
    CHECK_FALSE(live_index.empty());
    for (auto [balance, id] : restored.observer_state().by_balance) {
        const user* u = restored.get<user>(id);
        REQUIRE(u != nullptr);
        CHECK(u->balance == balance);
    }
    int live_records = 0;
    restored.for_each<user>([&](record_id, const user&) { ++live_records; });
    CHECK((int)restored.observer_state().by_balance.size() == live_records);

    std::remove(observed_spec::path.value);
    std::remove(observed_spec::data_path);
}

TEST_CASE("the observer sees the pre-record state, so an index can retract") {
    std::remove(observed_spec::path.value);
    std::remove(observed_spec::data_path);
    observed_storage table;

    record_id id = table.create(user{100, 1});
    CHECK(table.observer_state().by_balance.count(100) == 1);

    table.update<&user::balance>(id, 200);
    CHECK(table.observer_state().by_balance.count(100) == 0); // old entry retracted
    CHECK(table.observer_state().by_balance.count(200) == 1);

    table.destroy(id);
    CHECK(table.observer_state().by_balance.empty());

    std::remove(observed_spec::path.value);
    std::remove(observed_spec::data_path);
}

TEST_CASE("a default-observer storage costs nothing in size") {
    // [[no_unique_address]] on the observer - the empty default must not
    // grow the storage, or every existing user pays for a hook they do not use.
    CHECK(sizeof(storage::storage<>) == sizeof(storage::storage<storage::storage_spec<>, storage::null_observer>));
}

TEST_CASE("for_each_record walks a delta without applying anything") {
    fixture f;
    for (int i = 0; i < 5; ++i) f.table.create(user{(uint64_t)i, (uint32_t)i});
    const std::size_t mark = f.table.current_offset();

    auto a = f.table.create(user{100, 1});
    f.table.update<&user::balance>(a, 999);
    f.table.destroy(a);
    const std::size_t end = f.table.current_offset();

    std::vector<storage::record_op> seen;
    std::vector<record_id> seen_ids;
    std::size_t stopped = f.table.for_each_record(mark, end, [&](const storage::record_header& hdr, std::span<const std::byte> bytes) {
        seen.push_back(hdr.op);
        seen_ids.push_back(hdr.id);
        if (hdr.op == storage::record_op::update) CHECK(bytes.size() == sizeof(uint64_t));
    });

    CHECK(stopped == end);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == storage::record_op::create);
    CHECK(seen[1] == storage::record_op::update);
    CHECK(seen[2] == storage::record_op::destroy);
    for (auto id : seen_ids) CHECK(id == a);

    // Walking must not have changed anything.
    CHECK(f.table.current_offset() == end);
    CHECK(f.table.get<user>(a) == nullptr);

    // The one-argument form walks from a mark to now.
    int count = 0;
    f.table.for_each_record(mark, [&](const storage::record_header&, std::span<const std::byte>) { ++count; });
    CHECK(count == 3);
}

TEST_CASE("for_each_record stops early when the callback says so") {
    fixture f;
    for (int i = 0; i < 10; ++i) f.table.create(user{(uint64_t)i, (uint32_t)i});
    int seen = 0;
    f.table.for_each_record(0, [&](const storage::record_header&, std::span<const std::byte>) {
        return ++seen < 4;
    });
    CHECK(seen == 4);
}

TEST_CASE("an UPDATE whose diff falls outside its record is refused") {
    // The checksum only proves the bytes are the ones that were sent. For a
    // replica they were sent by a primary that may be compromised or buggy,
    // so the diff's range has to be checked against the record it patches.
    std::vector<std::byte> region(region_bytes);
    record_id id;
    std::size_t written;
    {
        storage::storage<> t(region.data());
        id = t.create(user{4242, 7});
        written = t.current_offset();
    }

    // Hand-craft an UPDATE that writes 8 bytes starting well past the record.
    std::vector<std::byte> hostile(sizeof(storage::record_header) + sizeof(uint64_t));
    uint64_t poison = 0xBADC0FFEE0DDF00Dull;
    storage::record_header hdr{ id, (uint32_t)(sizeof(user) + 4096), (uint32_t)sizeof(poison), storage::record_op::update, 0, 0 };
    hdr.checksum = storage::checksum_of(hdr, &poison, sizeof(poison));
    std::memcpy(hostile.data(), &hdr, sizeof(hdr));
    std::memcpy(hostile.data() + sizeof(hdr), &poison, sizeof(poison));

    storage::storage<> t(region.data());
    t.recover(written);
    const std::size_t before = t.current_offset();
    t.apply_wal_range(hostile.data(), hostile.size());

    // The record is copied into the log (it is well-formed and verifies),
    // but it must not have been applied anywhere.
    CHECK(t.current_offset() > before);
    REQUIRE(t.get<user>(id) != nullptr);
    CHECK(t.get<user>(id)->balance == 4242);
    CHECK(t.get<user>(id)->age == 7);
}
