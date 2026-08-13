// replication_demo.cpp - end-to-end exercise of replication.hpp:
//   phase 1: primary writes initial data before any replica exists
//   phase 2: replica A connects, gets a snapshot bootstrap, converges
//   phase 3: more writes, incremental catch-up to A verified
//   phase 4: replica B connects LATER (after more data exists) - proves
//            multi-replica fan-out: both A and B converge independently,
//            B via its own snapshot while A keeps catching up incrementally
//   phase 5: replica-side desync detection - C is registered with a stale
//            mid-WAL resume offset, sees from_offset not match what it
//            actually has, and self-heals via resync_request -> snapshot
//   phase 6: primary-side desync detection - D is registered as being
//            AHEAD of the primary (impossible by construction; the
//            signature of a primary-side compaction having rewritten
//            offsets underneath it). tick() alone would silently never
//            ship to it again; replicas_needing_resync() surfaces it
//
// Note the shape of the event handling below: each replica has ONE handler
// used in every phase, including the handshake loop. That's not incidental
// tidiness - the primary ships a new replica's snapshot the instant its own
// handshake completes (the responder side completes first), so those data
// packets can land in the very same poll_incoming() batch as the replica's
// own handshake_established event. A handshake loop that only looks for
// handshake_established and ignores everything else silently drops them,
// and the replica then waits forever for msg_ids that already came and
// went. Route replication traffic from the first poll after connect(), not
// from "once the handshake loop is done."
//
// Not wired into CMake yet - same standalone-demo convention as storage's
// own business_demo_heap.cpp/stress_heap.cpp, pending the repo-layout
// decision replication.hpp's own header comment mentions.
//
// Build: g++ -std=c++26 -O2 -Wall -Wextra -Isrc -luring -lsodium src/replication_demo.cpp -o replication_demo

#include "../replication/replication.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

namespace {

enum class kind : storage::tag_t { account = 1 };
struct account {
    static constexpr storage::tag_t tag = (storage::tag_t)kind::account;
    uint64_t balance;
};

using primary_spec = storage::storage_spec<storage::durability::local_sync, "replication_demo_primary.db", 16ull * 1024 * 1024>;
using replica_a_spec = storage::storage_spec<storage::durability::local_sync, "replication_demo_a.db", 16ull * 1024 * 1024>;
using replica_b_spec = storage::storage_spec<storage::durability::local_sync, "replication_demo_b.db", 16ull * 1024 * 1024>;
using replica_c_spec = storage::storage_spec<storage::durability::local_sync, "replication_demo_c.db", 16ull * 1024 * 1024>;
using replica_d_spec = storage::storage_spec<storage::durability::local_sync, "replication_demo_d.db", 16ull * 1024 * 1024>;

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

void step(uring_ring& ring, protocol::protocol_endpoint<>& ep, auto&& on_event) {
    ring.submit_and_wait(0);
    ring.drain_cqes([&](io_uring_cqe* cqe) { ep.handle_cqe(cqe); });
    for (auto& ev : ep.poll_incoming()) on_event(ev);
}

} // namespace

int main() {
    for (const char* f : {"replication_demo_primary.db", "replication_demo_a.db", "replication_demo_b.db", "replication_demo_c.db", "replication_demo_d.db"})
        std::remove(f);

    uint16_t primary_port, a_port, b_port, c_port, d_port;
    int primary_fd = make_bound_udp_socket(&primary_port);
    int a_fd = make_bound_udp_socket(&a_port);
    int b_fd = make_bound_udp_socket(&b_port);
    int c_fd = make_bound_udp_socket(&c_port);
    int d_fd = make_bound_udp_socket(&d_port);

    uring_ring primary_ring(2048), a_ring(2048), b_ring(2048), c_ring(2048), d_ring(2048);
    auto primary_keys = crypto::x25519_generate();
    crypto::public_key primary_pub = primary_keys.pub;

    protocol::protocol_endpoint<> primary_ep(primary_ring, primary_fd, std::move(primary_keys));
    protocol::protocol_endpoint<> a_ep(a_ring, a_fd, crypto::x25519_generate());
    protocol::protocol_endpoint<> b_ep(b_ring, b_fd, crypto::x25519_generate());
    protocol::protocol_endpoint<> c_ep(c_ring, c_fd, crypto::x25519_generate());
    protocol::protocol_endpoint<> d_ep(d_ring, d_fd, crypto::x25519_generate());
    primary_ep.arm(); a_ep.arm(); b_ep.arm(); c_ep.arm(); d_ep.arm();

    storage::storage<primary_spec> primary_table;
    storage::storage<replica_a_spec> a_table;
    storage::storage<replica_b_spec> b_table;
    storage::storage<replica_c_spec> c_table;
    storage::storage<replica_d_spec> d_table;

    replication::primary_shipper shipper;
    replication::replica_applier a_applier;
    replication::replica_applier b_applier;
    replication::replica_applier c_applier;
    replication::replica_applier d_applier;

    crypto::public_key a_pub{}, b_pub{}, c_pub{}, d_pub{};
    bool a_up = false, b_up = false, c_up = false, d_up = false;
    uint64_t a_conn = 1, b_conn = 2, c_conn = 3, d_conn = 4;
    sockaddr_in primary_addr = loopback_addr(primary_port);

    // Whether a brand-new replica should be snapshot-bootstrapped on
    // connect (phases 2/4) or deliberately left to discover its own bad
    // resume offset the hard way (phase 5).
    bool snapshot_on_connect = true;
    std::size_t forced_resume_offset = 0;

    // ONE handler per side, used in every phase - see the file header on
    // why splitting "handshake loop" and "steady state" handling is a trap.
    auto primary_on_event = [&](protocol::event& ev) {
        if (ev.type == protocol::event::event_type::handshake_established) {
            crypto::public_key* which = ev.conn_id == a_conn ? &a_pub : ev.conn_id == b_conn ? &b_pub : ev.conn_id == c_conn ? &c_pub : &d_pub;
            *which = ev.peer_static;
            shipper.on_replica_connected(*which, ev.conn_id, forced_resume_offset);
            if (snapshot_on_connect) shipper.ship_snapshot(primary_table, primary_ep, *which);
        } else if (ev.type == protocol::event::event_type::message_received && !ev.data.empty()) {
            crypto::public_key* which = ev.conn_id == a_conn ? &a_pub : ev.conn_id == b_conn ? &b_pub : ev.conn_id == c_conn ? &c_pub : &d_pub;
            auto k = (replication::msg_kind)ev.data[0];
            if (k == replication::msg_kind::progress_ack) shipper.on_ack(*which, ev.data);
            else if (k == replication::msg_kind::resync_request) shipper.ship_snapshot(primary_table, primary_ep, *which);
        }
    };

    auto make_replica_handler = [](auto& applier, auto& table, auto& ep, uint64_t conn, bool& up_flag) {
        return [&applier, &table, &ep, conn, &up_flag](protocol::event& ev) {
            if (ev.type == protocol::event::event_type::handshake_established) up_flag = true;
            else if (ev.type == protocol::event::event_type::message_received) {
                applier.on_message(table, ev.msg_id, ev.data, [&](const std::vector<uint8_t>& r) { ep.send_message(conn, r, true); });
            }
        };
    };
    auto a_on_event = make_replica_handler(a_applier, a_table, a_ep, a_conn, a_up);
    auto b_on_event = make_replica_handler(b_applier, b_table, b_ep, b_conn, b_up);
    auto c_on_event = make_replica_handler(c_applier, c_table, c_ep, c_conn, c_up);
    auto d_on_event = make_replica_handler(d_applier, d_table, d_ep, d_conn, d_up);

    auto pump = [&] {
        // Primary-side desync detection: a replica tracked as being ahead
        // of us can never be shipped anything again (see
        // replicas_needing_resync) - resnapshot it. Detection is the
        // library's job, this decision to act on it is the caller's.
        for (auto& key : shipper.replicas_needing_resync(primary_table))
            shipper.ship_snapshot(primary_table, primary_ep, key);

        shipper.tick(primary_table, primary_ep);
        step(primary_ring, primary_ep, primary_on_event);
        step(a_ring, a_ep, a_on_event);
        step(b_ring, b_ep, b_on_event);
        step(c_ring, c_ep, c_on_event);
        step(d_ring, d_ep, d_on_event);
        auto now = std::chrono::steady_clock::now();
        primary_ep.tick(now); a_ep.tick(now); b_ep.tick(now); c_ep.tick(now); d_ep.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto pump_until = [&](auto&& done, int max_rounds = 4000) {
        for (int i = 0; i < max_rounds; ++i) { pump(); if (done()) return true; }
        return false;
    };
    auto caught_up = [&](const crypto::public_key& key) {
        auto c = shipper.replicated_offset(key);
        return c.has_value() && *c >= primary_table.current_offset();
    };

    // --- Phase 1: primary writes BEFORE any replica exists -----------------
    auto id1 = primary_table.create(account{1000});
    auto id2 = primary_table.create(account{2000});
    primary_table.update<&account::balance>(id1, 900);
    primary_table.sync();
    std::printf("[phase1] primary has initial data, offset=%zu\n", primary_table.current_offset());

    // --- Phase 2: replica A connects, snapshot bootstrap, converges --------
    a_ep.connect(a_conn, primary_pub, (sockaddr*)&primary_addr, sizeof(primary_addr));
    bool phase2_ok = pump_until([&] { return a_up && caught_up(a_pub); });
    const account* a_r1 = a_table.get<account>(id1);
    phase2_ok = phase2_ok && a_r1 && a_r1->balance == 900;
    std::printf(phase2_ok ? "[phase2] replica A snapshot-bootstrapped OK (balance=%llu)\n" : "[phase2] FAIL\n",
                a_r1 ? (unsigned long long)a_r1->balance : 0);

    // --- Phase 3: more writes, incremental catch-up to A ------------------
    primary_table.update<&account::balance>(id2, 2500);
    primary_table.sync();
    bool phase3_ok = pump_until([&] { return caught_up(a_pub); });
    const account* a_r2 = a_table.get<account>(id2);
    phase3_ok = phase3_ok && a_r2 && a_r2->balance == 2500;
    std::printf(phase3_ok ? "[phase3] incremental catch-up to A OK (balance=%llu)\n" : "[phase3] FAIL\n",
                a_r2 ? (unsigned long long)a_r2->balance : 0);

    // --- Phase 4: replica B connects LATER - multi-replica fan-out --------
    b_ep.connect(b_conn, primary_pub, (sockaddr*)&primary_addr, sizeof(primary_addr));
    bool b_boot = pump_until([&] { return b_up && caught_up(b_pub); });
    primary_table.update<&account::balance>(id1, 111);
    primary_table.sync();
    bool both = pump_until([&] { return caught_up(a_pub) && caught_up(b_pub); });

    const account* a_r1f = a_table.get<account>(id1);
    const account* b_r1f = b_table.get<account>(id1);
    const account* b_r2f = b_table.get<account>(id2);
    bool phase4_ok = b_boot && both &&
                     a_r1f && a_r1f->balance == 111 &&
                     b_r1f && b_r1f->balance == 111 &&
                     b_r2f && b_r2f->balance == 2500; // B bootstrapped after phase 3, so it must already reflect it
    std::printf(phase4_ok ? "[phase4] multi-replica fan-out OK: A and B converged independently\n" : "[phase4] FAIL\n");

    // --- Phase 5: replica-side desync detection ---------------------------
    // C is registered with a resume offset that's BEHIND the primary but
    // doesn't match anything C actually has (a stale mid-WAL checkpoint).
    // The primary happily ships from there; C sees from_offset != what it
    // expects, asks for a resync, and self-heals.
    snapshot_on_connect = false;
    forced_resume_offset = 32;
    c_ep.connect(c_conn, primary_pub, (sockaddr*)&primary_addr, sizeof(primary_addr));
    bool phase5_ok = pump_until([&] { return c_up && caught_up(c_pub); }, 6000);
    const account* c_r1 = c_table.get<account>(id1);
    const account* c_r2 = c_table.get<account>(id2);
    phase5_ok = phase5_ok && c_r1 && c_r1->balance == 111 && c_r2 && c_r2->balance == 2500;
    std::printf(phase5_ok ? "[phase5] replica-side desync self-healed via resync_request (C converged)\n"
                          : "[phase5] FAIL\n");

    // --- Phase 6: primary-side desync detection ---------------------------
    // D is registered as being AHEAD of the primary - impossible by
    // construction, and the signature of a primary-side compaction having
    // rewritten offsets underneath a replica. tick() would silently never
    // ship to it again; replicas_needing_resync() is what makes that
    // visible, and pump() acts on it by resnapshotting.
    forced_resume_offset = 9999;
    d_ep.connect(d_conn, primary_pub, (sockaddr*)&primary_addr, sizeof(primary_addr));
    bool phase6_ok = pump_until([&] { return d_up && caught_up(d_pub); }, 6000);
    const account* d_r1 = d_table.get<account>(id1);
    const account* d_r2 = d_table.get<account>(id2);
    phase6_ok = phase6_ok && d_r1 && d_r1->balance == 111 && d_r2 && d_r2->balance == 2500;
    std::printf(phase6_ok ? "[phase6] primary-side desync detected and healed (D was tracked ahead of the primary)\n"
                          : "[phase6] FAIL\n");

    close(primary_fd); close(a_fd); close(b_fd); close(c_fd); close(d_fd);
    for (const char* f : {"replication_demo_primary.db", "replication_demo_a.db", "replication_demo_b.db", "replication_demo_c.db", "replication_demo_d.db"})
        std::remove(f);

    bool all_ok = phase2_ok && phase3_ok && phase4_ok && phase5_ok && phase6_ok;
    std::printf(all_ok ? "ALL REPLICATION CHECKS OK\n" : "FAILURES FOUND\n");
    return all_ok ? 0 : 1;
}
