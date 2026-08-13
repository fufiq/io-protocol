// replica_node.cpp - one binary, two roles, so a phone can be a replica of a
// primary running somewhere else.
//
//   replica_node --primary --port 9000 [--records N] [--seconds S]
//   replica_node --replica --host 10.0.2.2 --port 9000 --peer-key <64 hex>
//
// It exists to test the three halves of this project together on hardware that
// is not the development machine: the field engine, replication, and the epoll
// transport. Everything before this ran each of them somewhere - the engine on
// an emulator, replication on loopback, epoll against the protocol suite - but
// never all three at once, and never across a real network boundary.
//
// EPOLL ON BOTH SIDES, including the host. Not because the host needs it, but
// because a replica on Android has no choice - io_uring is blocked there by
// seccomp - and running the primary the same way keeps the thing being tested
// to one variable. The io_uring backend is exercised by the protocol suite.
//
// 10.0.2.2 is the host as seen from inside an Android emulator; on a real
// device it is whatever address the primary is actually reachable at. The
// replica initiates, because a NAT'd phone cannot be dialled.

#include "../embedded/epoll_udp.hpp"
#include "../replication/replication.hpp"
#include "../storage/field_storage.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

    struct account {
        static constexpr storage::tag_t tag = 1;
        uint64_t id;
        uint64_t balance;
        uint64_t updated_ms;
        uint64_t reserved;
    };

    using primary_spec = storage::field_spec<"rn_primary.log", 64ull << 20,
                                             "rn_primary.idx", 200'000, 100'000, 8, sizeof(account)>;
    using replica_spec = storage::field_spec<"rn_replica.log", 64ull << 20,
                                             "rn_replica.idx", 200'000, 100'000, 8, sizeof(account)>;

    int bind_udp(uint16_t port, uint16_t* actual) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { std::perror("socket"); std::exit(1); }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) { std::perror("bind"); std::exit(1); }
        socklen_t len = sizeof(a);
        ::getsockname(fd, (sockaddr*)&a, &len);
        if (actual) *actual = ntohs(a.sin_port);
        return fd;
    }

    std::string to_hex(const crypto::public_key& k) {
        static const char* d = "0123456789abcdef";
        std::string out;
        for (uint8_t b : k) { out += d[b >> 4]; out += d[b & 15]; }
        return out;
    }

    bool from_hex(const std::string& s, crypto::public_key& out) {
        if (s.size() != out.size() * 2) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < out.size(); ++i) {
            const int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }

    uint64_t now_ms() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ---------------------------------------------------------------- primary
    int run_primary(uint16_t port, int records, int seconds) {
        uint16_t bound = 0;
        const int fd = bind_udp(port, &bound);
        protocol::epoll_udp io(fd);
        auto keys = crypto::x25519_generate();
        const crypto::public_key pub = keys.pub;
        protocol::protocol_endpoint<protocol::epoll_udp> ep(io, std::move(keys));
        ep.arm();

        std::remove("rn_primary.log");
        std::remove("rn_primary.idx");
        storage::field_storage<primary_spec> table;
        if (!table.ok()) { std::printf("primary: could not map\n"); return 1; }

        replication::primary_shipper shipper;
        std::vector<crypto::public_key> known;

        std::printf("primary listening on %u\n", bound);
        std::printf("peer-key %s\n", to_hex(pub).c_str());
        std::fflush(stdout);

        // ONE handler, used from the first poll - a replica's snapshot can land
        // in the same batch as its own handshake event, and a loop that watches
        // only for handshakes drops it.
        auto on_event = [&](protocol::event& ev) {
            using t = protocol::event::event_type;
            if (ev.type == t::handshake_established) {
                known.push_back(ev.peer_static);
                shipper.on_replica_connected(ev.peer_static, ev.conn_id, 0);
                shipper.ship_snapshot(table, ep, ev.peer_static);
                std::printf("replica connected, snapshot shipped (%zu live records)\n", (std::size_t)records);
                std::fflush(stdout);
            } else if (ev.type == t::message_received && !ev.data.empty()) {
                const auto kind = (replication::msg_kind)ev.data[0];
                for (auto& k : known) {
                    if (kind == replication::msg_kind::progress_ack) shipper.on_ack(k, ev.data);
                    else if (kind == replication::msg_kind::resync_request) shipper.ship_snapshot(table, ep, k);
                }
            }
        };

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        int written = 0;
        auto next_write = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() < deadline) {
            // A trickle rather than a burst, so the replica is seen catching up
            // rather than being handed everything before it ever connects.
            if (written < records && std::chrono::steady_clock::now() >= next_write) {
                for (int i = 0; i < 100 && written < records; ++i, ++written)
                    table.create(account{ (uint64_t)written + 1, (uint64_t)written * 10, now_ms(), 0 });
                table.sync();
                next_write += std::chrono::milliseconds(50);
            }

            for (auto& key : shipper.replicas_needing_resync(table))
                shipper.ship_snapshot(table, ep, key);
            shipper.tick(table, ep);

            ep.poll_io(std::chrono::milliseconds(10));
            for (auto& ev : ep.poll_incoming()) on_event(ev);
            ep.tick(std::chrono::steady_clock::now());
        }

        std::printf("primary done: %d records, offset %zu\n", written, table.current_offset());
        return 0;
    }

    // ---------------------------------------------------------------- replica
    int run_replica(const char* host, uint16_t port, const std::string& peer_hex, int seconds) {
        crypto::public_key peer{};
        if (!from_hex(peer_hex, peer)) { std::printf("replica: bad --peer-key\n"); return 1; }

        const int fd = bind_udp(0, nullptr);
        protocol::epoll_udp io(fd);
        protocol::protocol_endpoint<protocol::epoll_udp> ep(io, crypto::x25519_generate());
        ep.arm();

        std::remove("rn_replica.log");
        std::remove("rn_replica.idx");
        storage::field_storage<replica_spec> table;
        if (!table.ok()) { std::printf("replica: could not map\n"); return 1; }

        replication::replica_applier applier;
        const uint64_t conn = 1;

        sockaddr_in primary{};
        primary.sin_family = AF_INET;
        primary.sin_port = htons(port);
        if (::inet_pton(AF_INET, host, &primary.sin_addr) != 1) { std::printf("replica: bad --host\n"); return 1; }

        std::printf("replica dialling %s:%u\n", host, port);
        std::fflush(stdout);
        ep.connect(conn, peer, (sockaddr*)&primary, sizeof(primary));

        bool established = false;
        auto on_event = [&](protocol::event& ev) {
            using t = protocol::event::event_type;
            if (ev.type == t::handshake_established) {
                established = true;
                std::printf("handshake established\n");
                std::fflush(stdout);
            } else if (ev.type == t::message_received) {
                applier.on_message(table, ev.msg_id, ev.data,
                                   [&](const std::vector<uint8_t>& reply) { ep.send_message(conn, reply, true); });
            } else if (ev.type == t::handshake_failed) {
                std::printf("handshake FAILED\n");
                std::fflush(stdout);
            }
        };

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::size_t last_reported = 0;
        auto next_report = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() < deadline) {
            ep.poll_io(std::chrono::milliseconds(10));
            for (auto& ev : ep.poll_incoming()) on_event(ev);
            ep.tick(std::chrono::steady_clock::now());

            if (std::chrono::steady_clock::now() >= next_report) {
                std::size_t live = 0;
                table.for_each_raw([&](auto) { ++live; });
                if (live != last_reported) {
                    std::printf("replicated %zu records (applied offset %zu)\n", live, applier.applied_offset());
                    std::fflush(stdout);
                    last_reported = live;
                }
                next_report += std::chrono::milliseconds(250);
            }
        }

        std::size_t live = 0;
        uint64_t checksum = 0;
        table.for_each<account>([&](storage::record_id, const account& a) { ++live; checksum += a.balance; });
        std::printf("replica done: established=%d records=%zu balance_sum=%llu applied_offset=%zu\n",
                    (int)established, live, (unsigned long long)checksum, applier.applied_offset());
        return (established && live > 0) ? 0 : 1;
    }

} // namespace

int main(int argc, char** argv) {
    bool primary = false, replica = false;
    const char* host = "127.0.0.1";
    uint16_t port = 9000;
    std::string peer_hex;
    int records = 2000, seconds = 12;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--primary") primary = true;
        else if (a == "--replica") replica = true;
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (a == "--peer-key" && i + 1 < argc) peer_hex = argv[++i];
        else if (a == "--records" && i + 1 < argc) records = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }

    if (primary == replica) {
        std::printf("usage: replica_node --primary --port P [--records N] [--seconds S]\n"
                    "       replica_node --replica --host H --port P --peer-key HEX [--seconds S]\n");
        return 2;
    }
    return primary ? run_primary(port, records, seconds) : run_replica(host, port, peer_hex, seconds);
}
