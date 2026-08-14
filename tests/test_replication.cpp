// test_replication.cpp - failure/reconnect/ordering tests for replication.hpp.
// The specification these check is replication/REPLICATION.md.
//
// Split into two halves deliberately:
//   - Unit tests drive replica_applier directly with hand-built payloads.
//     No sockets, no timing, fully deterministic - this is the only way to
//     actually exercise out-of-order delivery, since loopback UDP
//     essentially never reorders on its own, so a networked test would
//     silently never hit the reorder path at all.
//   - Networked tests cover what only a real endpoint pair can: disconnect
//     with writes accumulating, reconnect-and-resume, replica restart from
//     its own recovered file, and ack-after-msync.
//
// sync()-before-ack is wired up HERE, in the caller's send_reply callback,
// not in replication.hpp - see test_sync_before_ack below for the exact
// shape and its caveats.
//
// Build: cmake --build build --target replication_tests
// Run:   ctest --test-dir build   (runs this and unit_tests, in both configs)

#include "../replication/replication.hpp"
#include "../embedded/epoll_udp.hpp"

// Whichever transport this platform has. The networked scenarios below do not
// care which - that is the point of the backend split - and on Android there is
// only one, because io_uring is blocked there by seccomp.
#if __has_include(<liburing.h>)
#  include "../net/uring_io.hpp"
using test_io = protocol::uring_io;
// One shape at every call site: io_uring needs a ring to sit on, epoll does
// not, and no scenario below should have to know which it got.
inline test_io make_io(uring_ring& ring, int fd) { return test_io(ring, fd); }
#else
using test_io = protocol::epoll_udp;
// A stand-in so the scenarios below can go on declaring the ring their
// io_uring counterparts sit on. epoll has nothing to sit on and ignores it;
// keeping the declarations identical is what lets one body serve both.
struct uring_ring { explicit uring_ring(unsigned = 0) {} };
inline test_io make_io(auto&, int fd) { return test_io(fd); }
#endif
#include "../storage/field_storage.hpp"
// Fibers are Linux-only (ucontext), and one scenario below uses them to show
// that a synchronous-commit wait is trivial glue. Everything else in this file
// is portable, and on a platform without fibers it is worth running the rest
// rather than none of it.
#if !defined(__ANDROID__) && !defined(__APPLE__)
#  define BC_HAVE_FIBERS 1
#  include "../runtime/fiber.hpp"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;
int checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; std::printf("  FAIL %s:%d: ", __func__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

enum class kind : storage::tag_t { account = 1 };
struct account {
    static constexpr storage::tag_t tag = (storage::tag_t)kind::account;
    uint64_t balance;
};

// --- payload builders (mirror replication.hpp's wire framing) -------------
std::vector<uint8_t> make_wal_batch(uint64_t from_offset, const std::byte* bytes, std::size_t len) {
    std::vector<uint8_t> p(1 + sizeof(uint64_t) + len);
    p[0] = (uint8_t)replication::msg_kind::wal_batch;
    std::memcpy(p.data() + 1, &from_offset, sizeof(from_offset));
    if (len) std::memcpy(p.data() + 1 + sizeof(from_offset), bytes, len);
    return p;
}
std::vector<uint8_t> make_snapshot_batch(const std::byte* bytes, std::size_t len) {
    std::vector<uint8_t> p(1 + len);
    p[0] = (uint8_t)replication::msg_kind::snapshot_batch;
    if (len) std::memcpy(p.data() + 1, bytes, len);
    return p;
}
std::vector<uint8_t> make_snapshot_done(uint64_t resume_offset) {
    std::vector<uint8_t> p(1 + sizeof(uint64_t));
    p[0] = (uint8_t)replication::msg_kind::snapshot_done;
    std::memcpy(p.data() + 1, &resume_offset, sizeof(resume_offset));
    return p;
}

using mem_spec = storage::storage_spec<>; // shared: caller-provided region, no file

// =========================================================================
// TWO ENGINES BEHIND ONE SHAPE.
//
// replica_applier and primary_shipper take the table as `auto&` and never name
// its type, so replication is engine-agnostic by construction. It was never
// actually RUN against the field engine, though, and "should work" is not a
// test - so every scenario below is a template and every one of them runs twice.
//
// The two engines differ in exactly two ways, both documented:
//   - storage.hpp lives in a region the caller owns; the field engine owns
//     files, so it needs a directory. That is what the constructor argument is.
//   - get<T>() returns a pointer from one and an optional from the other, which
//     is why the reads below say `auto` and never `const account*`
//     (FIELD_STORAGE.md §5).
// =========================================================================

struct heap_engine {
    static constexpr const char* name = "storage.hpp";
    std::vector<std::byte> region{4ull * 1024 * 1024};
    storage::storage<mem_spec> t{region.data()};
    explicit heap_engine(const char*) {}
    auto& table() { return t; }
};

using rep_fspec = storage::field_spec<"t_rep.log", 4ull * 1024 * 1024,
                                      "t_rep.idx", 8192, 8192, 8, 64>;

struct field_engine {
    static constexpr const char* name = "field_storage.hpp";
    std::string dir;
    std::unique_ptr<storage::field_storage<rep_fspec>> t;
    explicit field_engine(const char* d) : dir(d) {
        ::mkdir(dir.c_str(), 0755);
        t = std::make_unique<storage::field_storage<rep_fspec>>(
                storage::storage_paths::under<rep_fspec>(dir));
    }
    ~field_engine() {
        t.reset();
        std::remove((dir + "/t_rep.log").c_str());
        std::remove((dir + "/t_rep.idx").c_str());
        ::rmdir(dir.c_str());
    }
    auto& table() { return *t; }
};

// A primary-ish storage plus the offsets of each write, so a test can slice its
// WAL at real record boundaries.
template <typename engine>
struct wal_source : engine {
    std::vector<std::size_t> marks{0};
    using engine::engine;
    void mark() { marks.push_back(this->table().current_offset()); }
};

// Reads that mean the same thing on both engines. `auto` is doing the work:
// one of these is a pointer and the other an optional, and both are contextually
// convertible to bool and both arrow into the record.
template <typename table_t>
bool present(table_t& t, storage::record_id id) {
    auto v = t.template get<account>(id);
    return (bool)v;
}
template <typename table_t>
uint64_t balance_of(table_t& t, storage::record_id id, uint64_t missing = ~0ull) {
    auto v = t.template get<account>(id);
    return v ? v->balance : missing;
}

// =========================================================================
// Unit tests - replica_applier driven directly, no network
// =========================================================================

template <typename engine>
void test_out_of_order_delivery() {
    wal_source<engine> src("t_rep_oo_p");
    auto& stable = src.table();
    auto id1 = stable.create(account{10}); src.mark();
    auto id2 = stable.create(account{20}); src.mark();
    stable.template update<&account::balance>(id1, 99); src.mark();

    engine dst("t_rep_oo_r");
    auto& rtable = dst.table();
    replication::replica_applier applier;
    auto noop = [](const std::vector<uint8_t>&) {};

    auto slice = [&](int i) {
        return make_wal_batch(src.marks[i], stable.raw_base() + src.marks[i], src.marks[i + 1] - src.marks[i]);
    };

    // Deliver 2, then 1 - neither may apply, since msg_id 0 is missing.
    applier.on_message(rtable, 2, slice(2), noop);
    applier.on_message(rtable, 1, slice(1), noop);
    CHECK(applier.applied_offset() == 0, "nothing should apply while msg_id 0 is missing, got offset=%zu", applier.applied_offset());
    CHECK(applier.reorder_buffer_size() == 2, "expected 2 buffered, got %zu", applier.reorder_buffer_size());

    // msg_id 0 arrives - all three must now drain in order, in one go.
    applier.on_message(rtable, 0, slice(0), noop);
    CHECK(applier.applied_offset() == stable.current_offset(),
          "expected fully caught up (%zu), got %zu", stable.current_offset(), applier.applied_offset());
    CHECK(applier.reorder_buffer_size() == 0, "reorder buffer should be drained, got %zu", applier.reorder_buffer_size());

    CHECK(balance_of(rtable, id1) == 99, "id1 should be 99 after in-order replay");
    CHECK(balance_of(rtable, id2) == 20, "id2 should be 20");
}

template <typename engine>
void test_duplicate_message_not_applied_twice() {
    wal_source<engine> src("t_rep_dup_p");
    auto& stable = src.table();
    auto id1 = stable.create(account{5}); src.mark();

    engine dst("t_rep_dup_r");
    auto& rtable = dst.table();
    replication::replica_applier applier;
    auto noop = [](const std::vector<uint8_t>&) {};

    auto batch = make_wal_batch(0, stable.raw_base(), src.marks[1]);
    applier.on_message(rtable, 0, batch, noop);
    std::size_t after_first = applier.applied_offset();

    applier.on_message(rtable, 0, batch, noop); // exact duplicate
    CHECK(applier.applied_offset() == after_first,
          "a duplicate msg_id must not advance applied_offset (%zu -> %zu)", after_first, applier.applied_offset());
    CHECK(balance_of(rtable, id1) == 5, "record should still be intact after a duplicate");

    // A duplicate must not accumulate either: anything at or below
    // next_msg_id is dropped on arrival rather than parked in the buffer,
    // so a peer that retransmits heavily can't grow it without bound.
    CHECK(applier.reorder_buffer_size() == 0,
          "a duplicate must not stay parked in the reorder buffer, got %zu", applier.reorder_buffer_size());
}

template <typename engine>
void test_gap_never_fills() {
    wal_source<engine> src("t_rep_gap_p");
    auto& stable = src.table();
    stable.create(account{1}); src.mark();

    engine dst("t_rep_gap_r");
    auto& rtable = dst.table();
    replication::replica_applier applier;
    auto noop = [](const std::vector<uint8_t>&) {};

    for (uint32_t id = 1; id <= 5; ++id) // msg_id 0 never arrives
        applier.on_message(rtable, id, make_wal_batch(0, stable.raw_base(), src.marks[1]), noop);

    CHECK(applier.applied_offset() == 0, "nothing may apply while the head of the sequence is missing");
    CHECK(applier.reorder_buffer_size() == 5, "all 5 should still be buffered, got %zu", applier.reorder_buffer_size());
}

template <typename engine>
void test_from_offset_mismatch_requests_resync() {
    wal_source<engine> src("t_rep_mis_p");
    auto& stable = src.table();
    stable.create(account{7}); src.mark();

    engine dst("t_rep_mis_r");
    auto& rtable = dst.table();
    replication::replica_applier applier;

    std::vector<uint8_t> reply;
    auto capture = [&](const std::vector<uint8_t>& r) { reply = r; };

    // Claims to start at 4096; the replica has nothing.
    applier.on_message(rtable, 0, make_wal_batch(4096, stable.raw_base(), src.marks[1]), capture);

    CHECK(!reply.empty() && (replication::msg_kind)reply[0] == replication::msg_kind::resync_request,
          "a from_offset mismatch must emit resync_request, got kind=%d", reply.empty() ? -1 : (int)reply[0]);
    CHECK(applier.applied_offset() == 0, "nothing may be applied from a mismatched batch");
}

template <typename engine>
void test_snapshot_replaces_stale_state() {
    // Replica has stale data; a snapshot must wipe it, not merge with it.
    // The stale set must contain ids the snapshot does NOT - otherwise
    // "still present" proves nothing, since ids are assigned per-storage
    // from 1 and would collide by construction.
    wal_source<engine> stale("t_rep_snap_s");
    auto& sttable = stale.table();
    sttable.create(account{111});
    sttable.create(account{222});
    auto ghost = sttable.create(account{666}); // id 3 - the snapshot below only has id 1
    stale.mark();

    engine dst("t_rep_snap_r");
    auto& rtable = dst.table();
    replication::replica_applier applier;
    auto noop = [](const std::vector<uint8_t>&) {};
    applier.on_message(rtable, 0, make_wal_batch(0, sttable.raw_base(), stale.marks[1]), noop);
    CHECK(present(rtable, ghost), "precondition: replica has the stale record");

    // Now a snapshot from a primary that never had `ghost`.
    wal_source<engine> src("t_rep_snap_p");
    auto& stable = src.table();
    auto keep = stable.create(account{42});
    stable.sync();
    auto snap = stable.export_snapshot();

    applier.on_message(rtable, 1, make_snapshot_batch(snap.data(), snap.size()), noop);
    applier.on_message(rtable, 2, make_snapshot_done(stable.current_offset()), noop);

    CHECK(!present(rtable, ghost), "the stale record must be gone after a snapshot reset");
    CHECK(balance_of(rtable, keep) == 42, "the snapshot's own record must be present");
    CHECK(applier.applied_offset() == stable.current_offset(),
          "applied_offset must jump to the primary's offset (%zu), got %zu",
          stable.current_offset(), applier.applied_offset());
}

template <typename engine>
void test_snapshot_split_across_messages() {
    wal_source<engine> src("t_rep_spl_p");
    auto& stable = src.table();
    for (int i = 0; i < 20; ++i) stable.create(account{(uint64_t)i});
    stable.sync();
    auto snap = stable.export_snapshot();
    CHECK(snap.size() > 64, "precondition: snapshot big enough to split");

    engine dst("t_rep_spl_r");
    auto& rtable = dst.table();
    replication::replica_applier applier;
    auto noop = [](const std::vector<uint8_t>&) {};

    std::size_t half = snap.size() / 2;
    applier.on_message(rtable, 0, make_snapshot_batch(snap.data(), half), noop);
    applier.on_message(rtable, 1, make_snapshot_batch(snap.data() + half, snap.size() - half), noop);
    applier.on_message(rtable, 2, make_snapshot_done(stable.current_offset()), noop);

    bool all_present = true;
    for (storage::record_id id = 1; id <= 20; ++id)
        if (balance_of(rtable, id) != id - 1) all_present = false;
    CHECK(all_present, "every record must survive a snapshot split across two messages");
}

// =========================================================================
// Networked tests
// =========================================================================

int make_bound_udp_socket(uint16_t* out_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { perror("bind"); exit(1); }
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

sockaddr_in loopback_addr(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

void step(protocol::protocol_endpoint<test_io>& ep, auto&& on_event) {
    ep.poll_io(std::chrono::milliseconds{0});
    for (auto& ev : ep.poll_incoming()) on_event(ev);
}

using file_spec_p = storage::storage_spec<storage::durability::local_sync, "rt_primary.db", 16ull * 1024 * 1024>;
using file_spec_r = storage::storage_spec<storage::durability::local_sync, "rt_replica.db", 16ull * 1024 * 1024>;

// Disconnect with writes accumulating, then reconnect (new conn_id, same
// identity) and resume incrementally from the last confirmed offset.
void test_disconnect_reconnect_resume() {
    std::remove("rt_primary.db"); std::remove("rt_replica.db");

    uint16_t pport, rport;
    int pfd = make_bound_udp_socket(&pport);
    int rfd = make_bound_udp_socket(&rport);
    uring_ring pring(2048), rring(2048);
    auto pkeys = crypto::x25519_generate();
    crypto::public_key ppub = pkeys.pub;
    test_io pep_io(make_io(pring, pfd));
    protocol::protocol_endpoint<test_io> pep(pep_io, std::move(pkeys));
    test_io rep_io(make_io(rring, rfd));
    protocol::protocol_endpoint<test_io> rep(rep_io, crypto::x25519_generate());
    pep.arm(); rep.arm();

    storage::storage<file_spec_p> ptable;
    storage::storage<file_spec_r> rtable;
    replication::primary_shipper shipper;
    replication::replica_applier applier;

    crypto::public_key rpub{};
    bool r_up = false;
    uint64_t conn = 1;
    sockaddr_in paddr = loopback_addr(pport);
    bool replica_alive = true;

    auto p_on_event = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) {
            rpub = ev.peer_static;
            auto prev = shipper.replicated_offset(rpub);
            shipper.on_replica_connected(rpub, ev.conn_id, prev.value_or(0));
            if (!prev.has_value()) shipper.ship_snapshot(ptable, pep, rpub); // brand new only
        } else if (ev.type == protocol::event::event_type::message_received && !ev.data.empty()) {
            auto k = (replication::msg_kind)ev.data[0];
            if (k == replication::msg_kind::progress_ack) shipper.on_ack(rpub, ev.data);
            else if (k == replication::msg_kind::resync_request) shipper.ship_snapshot(ptable, pep, rpub);
        }
    };
    auto r_on_event = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) {
            r_up = true;
            applier.on_reconnected(); // msg_id restarts at 0 on a fresh connection
        } else if (ev.type == protocol::event::event_type::message_received)
            applier.on_message(rtable, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) { rep.send_message(conn, r, true); });
    };
    auto pump = [&] {
        shipper.tick(ptable, pep);
        step(pep, p_on_event);
        if (replica_alive) step(rep, r_on_event);
        auto now = std::chrono::steady_clock::now();
        pep.tick(now);
        if (replica_alive) rep.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto pump_until = [&](auto&& done, int n = 4000) { for (int i = 0; i < n; ++i) { pump(); if (done()) return true; } return false; };
    auto caught_up = [&] { auto c = shipper.replicated_offset(rpub); return c && *c >= ptable.current_offset(); };

    auto id1 = ptable.create(account{100});
    ptable.sync();
    rep.connect(conn, ppub, (sockaddr*)&paddr, sizeof(paddr));
    CHECK(pump_until([&] { return r_up && caught_up(); }), "initial sync should complete");
    std::size_t confirmed_before_drop = shipper.replicated_offset(rpub).value_or(0);

    // Replica goes away. Primary keeps writing.
    replica_alive = false;
    shipper.on_replica_disconnected(rpub);
    ptable.update<&account::balance>(id1, 200);
    auto id2 = ptable.create(account{300});
    ptable.sync();
    for (int i = 0; i < 50; ++i) pump(); // primary ticks alone; nothing may be shipped
    CHECK(shipper.replicated_offset(rpub).value_or(0) == confirmed_before_drop,
          "a disconnected replica's confirmed offset must not move");

    // Reconnect: brand-new conn_id, same identity. Resume state must be
    // found by public key and picked up incrementally (no re-snapshot).
    replica_alive = true;
    conn = 2;
    rep.connect(conn, ppub, (sockaddr*)&paddr, sizeof(paddr));
    CHECK(pump_until([&] { return caught_up(); }, 6000), "should catch up after reconnect");

    const account* a1 = rtable.get<account>(id1);
    const account* a2 = rtable.get<account>(id2);
    CHECK(a1 && a1->balance == 200, "writes made while disconnected must arrive after reconnect");
    CHECK(a2 && a2->balance == 300, "records created while disconnected must arrive too");

    close(pfd); close(rfd);
    std::remove("rt_primary.db"); std::remove("rt_replica.db");
}

// The replica syncs its own copy to disk BEFORE acking - wired here in the
// caller's callback, deliberately not in replication.hpp.
void test_sync_before_ack() {
    std::remove("rt_primary.db"); std::remove("rt_replica.db");

    uint16_t pport, rport;
    int pfd = make_bound_udp_socket(&pport);
    int rfd = make_bound_udp_socket(&rport);
    uring_ring pring(2048), rring(2048);
    auto pkeys = crypto::x25519_generate();
    crypto::public_key ppub = pkeys.pub;
    test_io pep_io(make_io(pring, pfd));
    protocol::protocol_endpoint<test_io> pep(pep_io, std::move(pkeys));
    test_io rep_io(make_io(rring, rfd));
    protocol::protocol_endpoint<test_io> rep(rep_io, crypto::x25519_generate());
    pep.arm(); rep.arm();

    storage::storage<file_spec_p> ptable;
    storage::storage<file_spec_r> rtable;
    replication::primary_shipper shipper;
    replication::replica_applier applier;

    crypto::public_key rpub{};
    bool r_up = false;
    uint64_t conn = 1;
    sockaddr_in paddr = loopback_addr(pport);
    int syncs_before_ack = 0;

    auto p_on_event = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) {
            rpub = ev.peer_static;
            shipper.on_replica_connected(rpub, ev.conn_id, 0);
            shipper.ship_snapshot(ptable, pep, rpub);
        } else if (ev.type == protocol::event::event_type::message_received && !ev.data.empty()) {
            auto k = (replication::msg_kind)ev.data[0];
            if (k == replication::msg_kind::progress_ack) shipper.on_ack(rpub, ev.data);
            else if (k == replication::msg_kind::resync_request) shipper.ship_snapshot(ptable, pep, rpub);
        }
    };
    auto r_on_event = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) { r_up = true; applier.on_reconnected(); }
        else if (ev.type == protocol::event::event_type::message_received) {
            applier.on_message(rtable, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) {
                // THE POINT OF THIS TEST: make the ack mean "durable on my
                // disk too," not just "in my memory." Only on progress_ack
                // - a resync_request means nothing was applied, so there's
                // nothing to flush.
                if (!r.empty() && (replication::msg_kind)r[0] == replication::msg_kind::progress_ack) {
                    rtable.sync();
                    ++syncs_before_ack;
                }
                rep.send_message(conn, r, true);
            });
        }
    };
    auto pump = [&] {
        shipper.tick(ptable, pep);
        step(pep, p_on_event);
        step(rep, r_on_event);
        auto now = std::chrono::steady_clock::now();
        pep.tick(now); rep.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto pump_until = [&](auto&& done, int n = 4000) { for (int i = 0; i < n; ++i) { pump(); if (done()) return true; } return false; };
    auto caught_up = [&] { auto c = shipper.replicated_offset(rpub); return c && *c >= ptable.current_offset(); };

    auto id1 = ptable.create(account{11});
    ptable.sync();
    rep.connect(conn, ppub, (sockaddr*)&paddr, sizeof(paddr));
    CHECK(pump_until([&] { return r_up && caught_up(); }), "initial sync should complete");

    ptable.update<&account::balance>(id1, 22);
    ptable.sync();
    CHECK(pump_until([&] { return caught_up(); }), "incremental update should replicate");

    CHECK(syncs_before_ack >= 2, "replica should have msync'd before each ack, got %d", syncs_before_ack);

    // The replica's own file must independently contain the data: reopen it
    // fresh and recover() - this is what "acked after msync" is actually
    // promising, so verify it rather than trusting the call count.
    struct stat st{};
    CHECK(::stat("rt_replica.db", &st) == 0, "replica file should exist");
    {
        storage::storage<file_spec_r> reopened;
        reopened.recover((std::size_t)st.st_size);
        const account* a = reopened.get<account>(id1);
        CHECK(a && a->balance == 22, "a freshly recovered replica file must hold the acked value (got %llu)",
              a ? (unsigned long long)a->balance : 0);
    }

    close(pfd); close(rfd);
    std::remove("rt_primary.db"); std::remove("rt_replica.db");
}

// Replica process "crashes" and restarts: reopens its file, recover()s, then
// reconnects. Documents what actually happens to a restarted replica.
void test_replica_restart_from_own_file() {
    std::remove("rt_primary.db"); std::remove("rt_replica.db");

    uint16_t pport, rport;
    int pfd = make_bound_udp_socket(&pport);
    uring_ring pring(2048);
    auto pkeys = crypto::x25519_generate();
    crypto::public_key ppub = pkeys.pub;
    test_io pep_io(make_io(pring, pfd));
    protocol::protocol_endpoint<test_io> pep(pep_io, std::move(pkeys));
    pep.arm();

    storage::storage<file_spec_p> ptable;
    replication::primary_shipper shipper;
    crypto::public_key rpub{};
    sockaddr_in paddr = loopback_addr(pport);

    auto id1 = ptable.create(account{1});
    auto id2 = ptable.create(account{2});
    ptable.sync();

    // --- first life ---
    {
        int rfd = make_bound_udp_socket(&rport);
        uring_ring rring(2048);
        test_io rep_io(make_io(rring, rfd));
    protocol::protocol_endpoint<test_io> rep(rep_io, crypto::x25519_generate());
        rep.arm();
        storage::storage<file_spec_r> rtable;
        replication::replica_applier applier;
        bool r_up = false;
        uint64_t conn = 1;

        auto p_on_event = [&](protocol::event& ev) {
            if (ev.type == protocol::event::event_type::handshake_established) {
                rpub = ev.peer_static;
                shipper.on_replica_connected(rpub, ev.conn_id, 0);
                shipper.ship_snapshot(ptable, pep, rpub);
            } else if (ev.type == protocol::event::event_type::message_received && !ev.data.empty()) {
                auto k = (replication::msg_kind)ev.data[0];
                if (k == replication::msg_kind::progress_ack) shipper.on_ack(rpub, ev.data);
                else if (k == replication::msg_kind::resync_request) shipper.ship_snapshot(ptable, pep, rpub);
            }
        };
        auto r_on_event = [&](protocol::event& ev) {
            if (ev.type == protocol::event::event_type::handshake_established) { r_up = true; applier.on_reconnected(); }
            else if (ev.type == protocol::event::event_type::message_received)
                applier.on_message(rtable, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) {
                    rtable.sync(); // durable before ack, so the restart below has something to recover
                    rep.send_message(conn, r, true);
                });
        };
        rep.connect(conn, ppub, (sockaddr*)&paddr, sizeof(paddr));
        bool ok = false;
        for (int i = 0; i < 4000 && !ok; ++i) {
            shipper.tick(ptable, pep);
            step(pep, p_on_event);
            step(rep, r_on_event);
            auto now = std::chrono::steady_clock::now();
            pep.tick(now); rep.tick(now);
            auto c = shipper.replicated_offset(rpub);
            ok = r_up && c && *c >= ptable.current_offset();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(ok, "first-life sync should complete");
        close(rfd);
    } // replica "crashes" here

    // --- restart: reopen the file and recover ---
    struct stat st{};
    CHECK(::stat("rt_replica.db", &st) == 0, "replica file should survive the crash");
    storage::storage<file_spec_r> rtable2;
    rtable2.recover((std::size_t)st.st_size);
    const account* rec1 = rtable2.get<account>(id1);
    CHECK(rec1 && rec1->balance == 1, "recovered replica file should still hold its data");

    // What a restarted replica actually faces: replica_applier's
    // next_apply_offset lives only in memory, so it comes back as 0, while
    // the primary still tracks this identity as caught up. Shipping
    // incrementally from the primary's position can never line up with a
    // fresh applier - so the restart MUST be treated as needing a
    // resnapshot. Verify that path works rather than pretending resume does.
    shipper.on_replica_disconnected(rpub);

    int rfd2 = make_bound_udp_socket(&rport);
    uring_ring rring2(2048);
    test_io rep2_io(make_io(rring2, rfd2));
    protocol::protocol_endpoint<test_io> rep2(rep2_io, crypto::x25519_generate());
    rep2.arm();
    replication::replica_applier applier2;
    crypto::public_key rpub2{};
    bool r_up2 = false;
    uint64_t conn2 = 7;

    ptable.update<&account::balance>(id2, 999);
    ptable.sync();

    auto p_on_event2 = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) {
            rpub2 = ev.peer_static;
            shipper.on_replica_connected(rpub2, ev.conn_id, 0);
            shipper.ship_snapshot(ptable, pep, rpub2); // restarted replica: resnapshot, not resume
        } else if (ev.type == protocol::event::event_type::message_received && !ev.data.empty()) {
            auto k = (replication::msg_kind)ev.data[0];
            if (k == replication::msg_kind::progress_ack) shipper.on_ack(rpub2, ev.data);
            else if (k == replication::msg_kind::resync_request) shipper.ship_snapshot(ptable, pep, rpub2);
        }
    };
    auto r_on_event2 = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) { r_up2 = true; applier2.on_reconnected(); }
        else if (ev.type == protocol::event::event_type::message_received)
            applier2.on_message(rtable2, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) { rep2.send_message(conn2, r, true); });
    };
    rep2.connect(conn2, ppub, (sockaddr*)&paddr, sizeof(paddr));
    bool ok2 = false;
    for (int i = 0; i < 6000 && !ok2; ++i) {
        shipper.tick(ptable, pep);
        step(pep, p_on_event2);
        step(rep2, r_on_event2);
        auto now = std::chrono::steady_clock::now();
        pep.tick(now); rep2.tick(now);
        auto c = shipper.replicated_offset(rpub2);
        ok2 = r_up2 && c && *c >= ptable.current_offset();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(ok2, "restarted replica should converge via resnapshot");
    const account* a2 = rtable2.get<account>(id2);
    CHECK(a2 && a2->balance == 999, "restarted replica must pick up writes made while it was down (got %llu)",
          a2 ? (unsigned long long)a2->balance : 0);

    close(pfd); close(rfd2);
    std::remove("rt_primary.db"); std::remove("rt_replica.db");
}

// =========================================================================
// Synchronous replication, built ON TOP of the counting primitives.
// replication.hpp deliberately implements no "sync mode" - this is what
// the caller writes instead, and these tests are the reference for it.
// =========================================================================

// A two-replica rig, so quorum means something (2 of 2, or 1 of 2 when one
// is dead) rather than being a synonym for "the one replica".
struct sync_rig {
    uint16_t pport{}, r1port{}, r2port{};
    int pfd, r1fd, r2fd;
    uring_ring pring{2048}, r1ring{2048}, r2ring{2048};
    crypto::public_key ppub;
    // The backends are members too, and declared BEFORE the endpoints that
    // hold references to them - members initialize in declaration order.
    test_io pio{make_io(pring, pfd)}, r1io{make_io(r1ring, r1fd)}, r2io{make_io(r2ring, r2fd)};
    protocol::protocol_endpoint<test_io> pep, r1ep, r2ep;
    storage::storage<file_spec_p> ptable;
    replication::primary_shipper shipper;
    crypto::public_key r1pub{}, r2pub{};
    bool r1_alive = true, r2_alive = true;

    // Replica tables live in caller-provided regions so two of them can
    // coexist without needing two distinct compile-time file paths.
    std::vector<std::byte> r1region{4ull * 1024 * 1024}, r2region{4ull * 1024 * 1024};
    storage::storage<mem_spec> r1table{r1region.data()}, r2table{r2region.data()};
    replication::replica_applier r1applier, r2applier;

    static crypto::key_pair gen() { return crypto::x25519_generate(); }

    sync_rig(crypto::key_pair pk)
        : pfd(make_bound_udp_socket(&pport)), r1fd(make_bound_udp_socket(&r1port)), r2fd(make_bound_udp_socket(&r2port)),
          ppub(pk.pub),
          pep(pio, std::move(pk)), r1ep(r1io, gen()), r2ep(r2io, gen()) {
        pep.arm(); r1ep.arm(); r2ep.arm();
    }
    ~sync_rig() { close(pfd); close(r1fd); close(r2fd); }

    void connect_replicas() {
        sockaddr_in paddr = loopback_addr(pport);
        r1ep.connect(1, ppub, (sockaddr*)&paddr, sizeof(paddr));
        r2ep.connect(2, ppub, (sockaddr*)&paddr, sizeof(paddr));
    }

    void pump() {
        shipper.tick(ptable, pep);
        step(pep, [&](protocol::event& ev) {
            crypto::public_key* which = ev.conn_id == 1 ? &r1pub : &r2pub;
            if (ev.type == protocol::event::event_type::handshake_established) {
                *which = ev.peer_static;
                shipper.on_replica_connected(*which, ev.conn_id, 0);
                shipper.ship_snapshot(ptable, pep, *which);
            } else if (ev.type == protocol::event::event_type::message_received && !ev.data.empty()) {
                auto k = (replication::msg_kind)ev.data[0];
                if (k == replication::msg_kind::progress_ack) shipper.on_ack(*which, ev.data);
                else if (k == replication::msg_kind::resync_request) shipper.ship_snapshot(ptable, pep, *which);
            }
        });
        if (r1_alive) step(r1ep, [&](protocol::event& ev) {
            if (ev.type == protocol::event::event_type::handshake_established) r1applier.on_reconnected();
            else if (ev.type == protocol::event::event_type::message_received)
                r1applier.on_message(r1table, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) { r1ep.send_message(1, r, true); });
        });
        if (r2_alive) step(r2ep, [&](protocol::event& ev) {
            if (ev.type == protocol::event::event_type::handshake_established) r2applier.on_reconnected();
            else if (ev.type == protocol::event::event_type::message_received)
                r2applier.on_message(r2table, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) { r2ep.send_message(2, r, true); });
        });
        auto now = std::chrono::steady_clock::now();
        pep.tick(now);
        if (r1_alive) r1ep.tick(now);
        if (r2_alive) r2ep.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};

// THE synchronous-commit primitive, written at the call site rather than
// in the library: block until `required` of `keys` confirm target_offset,
// or give up. Returns false on timeout - that's the failure mode, and it
// lives here instead of inside storage's create()/update().
bool wait_for_quorum(sync_rig& rig, std::size_t target_offset,
                     std::span<const crypto::public_key> keys, std::size_t required,
                     std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        rig.pump();
        if (rig.shipper.quorum_reached(target_offset, keys, required)) return true;
    }
    return false;
}

void test_sync_commit_reaches_quorum() {
    std::remove("rt_primary.db");
    sync_rig rig(crypto::x25519_generate());
    rig.connect_replicas();

    auto id1 = rig.ptable.create(account{1000});
    rig.ptable.sync();

    // Let both replicas come up and bootstrap first.
    bool up = false;
    for (int i = 0; i < 4000 && !up; ++i) {
        rig.pump();
        up = rig.r1pub != crypto::public_key{} && rig.r2pub != crypto::public_key{};
    }
    CHECK(up, "both replicas should connect");

    // The synchronous write: do it locally (instant, infallible), then
    // explicitly wait for 2-of-2 to confirm the resulting position.
    rig.ptable.update<&account::balance>(id1, 4242);
    rig.ptable.sync();
    std::size_t target = rig.ptable.current_offset();

    crypto::public_key keys[] = {rig.r1pub, rig.r2pub};
    bool committed = wait_for_quorum(rig, target, keys, /*required=*/2, std::chrono::milliseconds(5000));
    CHECK(committed, "2-of-2 quorum should be reached");

    CHECK(rig.shipper.replicas_confirmed_through(target, keys) == 2,
          "both replicas should be confirmed through target, got %zu",
          rig.shipper.replicas_confirmed_through(target, keys));

    const account* a1 = rig.r1table.get<account>(id1);
    const account* a2 = rig.r2table.get<account>(id1);
    CHECK(a1 && a1->balance == 4242, "replica 1 must hold the committed value");
    CHECK(a2 && a2->balance == 4242, "replica 2 must hold the committed value");
    std::remove("rt_primary.db");
}

void test_sync_commit_times_out_without_quorum() {
    std::remove("rt_primary.db");
    sync_rig rig(crypto::x25519_generate());
    rig.connect_replicas();

    auto id1 = rig.ptable.create(account{1});
    rig.ptable.sync();
    bool up = false;
    for (int i = 0; i < 4000 && !up; ++i) {
        rig.pump();
        up = rig.r1pub != crypto::public_key{} && rig.r2pub != crypto::public_key{};
    }
    CHECK(up, "both replicas should connect");

    // Kill replica 2. A 2-of-2 requirement can no longer be met.
    rig.r2_alive = false;
    rig.shipper.on_replica_disconnected(rig.r2pub);

    rig.ptable.update<&account::balance>(id1, 77);
    rig.ptable.sync();
    std::size_t target = rig.ptable.current_offset();
    crypto::public_key keys[] = {rig.r1pub, rig.r2pub};

    bool committed = wait_for_quorum(rig, target, keys, 2, std::chrono::milliseconds(600));
    CHECK(!committed, "2-of-2 must NOT be reachable with one replica down");

    // ...but 1-of-2 must still succeed, and the surviving replica must
    // actually have the data - this is the whole point of quorum being a
    // caller-chosen threshold rather than a hardcoded rule.
    bool majority = wait_for_quorum(rig, target, keys, 1, std::chrono::milliseconds(5000));
    CHECK(majority, "1-of-2 should still be reachable with one replica alive");
    const account* a1 = rig.r1table.get<account>(id1);
    CHECK(a1 && a1->balance == 77, "the surviving replica must hold the value");
    std::remove("rt_primary.db");
}

// Same wait, but yielding a fiber instead of spinning - showing the glue is
// trivial and that replication.hpp needs no fiber dependency of its own.
#ifdef BC_HAVE_FIBERS
void test_sync_commit_from_a_fiber() {
    std::remove("rt_primary.db");
    sync_rig rig(crypto::x25519_generate());
    rig.connect_replicas();

    auto id1 = rig.ptable.create(account{5});
    rig.ptable.sync();
    bool up = false;
    for (int i = 0; i < 4000 && !up; ++i) {
        rig.pump();
        up = rig.r1pub != crypto::public_key{} && rig.r2pub != crypto::public_key{};
    }
    CHECK(up, "both replicas should connect");

    rig.ptable.update<&account::balance>(id1, 31337);
    rig.ptable.sync();
    std::size_t target = rig.ptable.current_offset();
    crypto::public_key keys[] = {rig.r1pub, rig.r2pub};

    bool committed = false, fiber_finished = false;
    fiber::scheduler sched;
    sched.spawn([&] {
        // Business-logic shape: issue the write, then yield until it's
        // durable on a quorum. Nothing about this function's signature
        // changes to accommodate the wait - that's the property stackful
        // fibers give that a stackless coroutine wouldn't.
        while (!rig.shipper.quorum_reached(target, keys, 2)) fiber::yield();
        committed = true;
        fiber_finished = true;
    });

    for (int i = 0; i < 5000 && !fiber_finished; ++i) {
        rig.pump();        // the event loop keeps the network moving...
        sched.run_once();  // ...and the fiber re-checks each pass
    }
    CHECK(committed, "the fiber should observe quorum and resume");
    // run_once() reaps a finished fiber on the pass AFTER it returns, so
    // one more turn is needed before the scheduler reports itself empty.
    sched.run_once();
    CHECK(!sched.has_pending(), "the fiber should have been reaped");
    std::remove("rt_primary.db");
}
#endif

// A restarted replica CAN resume incrementally if the caller persisted and
// restored the replication checkpoint - the counterpart to
// test_replica_restart_from_own_file, which shows what happens when it
// doesn't.
template <typename engine>
void test_checkpoint_restore_enables_incremental_resume() {
    wal_source<engine> src("t_rep_ckr_p");
    auto& stable = src.table();
    stable.create(account{1});
    stable.create(account{2});
    src.mark();

    engine dst("t_rep_ckr_r");
    auto& rtable = dst.table();
    auto noop = [](const std::vector<uint8_t>&) {};

    std::size_t saved_checkpoint = 0;
    {
        replication::replica_applier applier;
        applier.on_message(rtable, 0, make_wal_batch(0, stable.raw_base(), src.marks[1]), noop);
        saved_checkpoint = applier.applied_offset(); // what the caller would persist
        CHECK(saved_checkpoint == src.marks[1], "precondition: applier caught up");
    } // applier destroyed - replica "restarts"

    // More writes happen while it's down.
    auto id3 = stable.create(account{3});
    src.mark();

    // Fresh applier + restored checkpoint => incremental resume works, no
    // resnapshot needed.
    replication::replica_applier restarted;
    restarted.set_applied_offset(saved_checkpoint);
    restarted.on_message(rtable, 0,
                         make_wal_batch(src.marks[1], stable.raw_base() + src.marks[1], src.marks[2] - src.marks[1]),
                         noop);

    CHECK(restarted.applied_offset() == stable.current_offset(),
          "restored checkpoint should allow incremental catch-up (%zu vs %zu)",
          restarted.applied_offset(), stable.current_offset());
    CHECK(balance_of(rtable, id3) == 3, "the write made while down must apply after resume");
}

} // namespace

int main() {
    // Every applier scenario runs against BOTH engines. The label carries which
    // one, so a failure says immediately whether it is the replication logic or
    // the storage underneath.
    struct { const char* name; void (*fn)(); } tests[] = {
        {"out_of_order_delivery [storage.hpp]", test_out_of_order_delivery<heap_engine>},
        {"out_of_order_delivery [field]", test_out_of_order_delivery<field_engine>},
        {"duplicate_message_not_applied_twice [storage.hpp]", test_duplicate_message_not_applied_twice<heap_engine>},
        {"duplicate_message_not_applied_twice [field]", test_duplicate_message_not_applied_twice<field_engine>},
        {"gap_never_fills [storage.hpp]", test_gap_never_fills<heap_engine>},
        {"gap_never_fills [field]", test_gap_never_fills<field_engine>},
        {"from_offset_mismatch_requests_resync [storage.hpp]", test_from_offset_mismatch_requests_resync<heap_engine>},
        {"from_offset_mismatch_requests_resync [field]", test_from_offset_mismatch_requests_resync<field_engine>},
        {"snapshot_replaces_stale_state [storage.hpp]", test_snapshot_replaces_stale_state<heap_engine>},
        {"snapshot_replaces_stale_state [field]", test_snapshot_replaces_stale_state<field_engine>},
        {"snapshot_split_across_messages [storage.hpp]", test_snapshot_split_across_messages<heap_engine>},
        {"snapshot_split_across_messages [field]", test_snapshot_split_across_messages<field_engine>},
        {"disconnect_reconnect_resume", test_disconnect_reconnect_resume},
        {"sync_before_ack", test_sync_before_ack},
        {"replica_restart_from_own_file", test_replica_restart_from_own_file},
        {"sync_commit_reaches_quorum", test_sync_commit_reaches_quorum},
        {"sync_commit_times_out_without_quorum", test_sync_commit_times_out_without_quorum},
#ifdef BC_HAVE_FIBERS
        {"sync_commit_from_a_fiber", test_sync_commit_from_a_fiber},
#endif
        {"checkpoint_restore_enables_incremental_resume [storage.hpp]", test_checkpoint_restore_enables_incremental_resume<heap_engine>},
        {"checkpoint_restore_enables_incremental_resume [field]", test_checkpoint_restore_enables_incremental_resume<field_engine>},
    };
    for (auto& t : tests) {
        int before = failures;
        std::printf("[%s]\n", t.name);
        t.fn();
        std::printf("  %s\n", failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
