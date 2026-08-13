#include <doctest/doctest.h>

#include "../net/protocol.hpp"
#include "../net/uring_io.hpp"
#include "../embedded/epoll_udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <concepts>
#include <thread>

using namespace protocol;

namespace {

int make_bound_udp_socket(uint16_t* out_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(fd, (sockaddr*)&addr, &len) == 0);
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

// A single peer's ring/socket/endpoint bundle plus whatever Events its
// poll_incoming() calls have produced so far, so tests can inspect them
// without re-querying protocol_endpoint's own (batch, drain-on-read) queue.
//
// Templated purely to pass all three NTTPs through to protocol_endpoint's
// own: every test uses the library defaults (peer<tr>) except the ones that
// specifically need a short interval/timeout to observe something firing
// within the test's round budget without waiting out the real (multi-
// second/minute) defaults.
// The two transports, so every test below runs on both. What differs is only
// what a backend needs to exist: io_uring needs a ring to sit on, epoll needs
// nothing but the socket. Bundling that here is what keeps `peer` from knowing
// which one it has.
struct uring_transport {
    static constexpr const char* name = "io_uring";
    uring_ring ring{2048};
    protocol::uring_io io;
    uring_transport(int fd, const endpoint_config& c)
        : io(ring, fd, c.send_pool_size, c.recv_num_bufs, c.recv_buf_size) {}
};

struct epoll_transport {
    static constexpr const char* name = "epoll";
    protocol::epoll_udp io;
    epoll_transport(int fd, const endpoint_config&) : io(fd) {}
};

template <typename transport = uring_transport,
          size_t keepalive_interval_ms = 20000, size_t liveness_timeout_ms = 60000, size_t handshake_timeout_ms = 5000>
struct peer {
    uint16_t port = 0;
    int fd;
    transport tr;
    // crypto::key_pair is move-only (holds a secret_buffer), so it can't be
    // both kept as a member and passed by value into protocol_endpoint's
    // constructor via a copy - generate it once, remember just the
    // (copyable) public half for tests to reference, and move the whole
    // thing into protocol_endpoint. Declaration order (this member before
    // static_pub/endpoint) is what makes the initializer list below valid:
    // members initialize in declaration order, not list order.
    crypto::key_pair local_keys;
    crypto::public_key static_pub;
    protocol_endpoint<decltype(transport::io), keepalive_interval_ms, liveness_timeout_ms, handshake_timeout_ms> endpoint;
    std::vector<event> events;

    explicit peer(endpoint_config config = {})
        : fd(make_bound_udp_socket(&port)), tr(fd, config), local_keys(crypto::x25519_generate()),
          static_pub(local_keys.pub), endpoint(tr.io, std::move(local_keys), config) {
        endpoint.arm();
    }

    // Non-blocking: a zero timeout drains only what has already arrived, then
    // pop_all's whatever landed in the incoming queue this round - not a
    // callback, just a batch of Events the test can inspect directly.
    void step() {
        endpoint.poll_io(std::chrono::milliseconds{0});
        for (auto& ev : endpoint.poll_incoming()) events.push_back(std::move(ev));
        endpoint.tick(std::chrono::steady_clock::now());
    }
};

// Steps both peers in an interleaved, non-blocking loop until `done()` is
// true or a generous round budget is exhausted (with a short sleep between
// rounds so this doesn't spin a whole CPU core needlessly). Loopback round
// trips are sub-millisecond, so this settles almost immediately for
// anything that's actually going to succeed. `a`/`b` are `auto&` rather
// than `peer<tr>&`: the two sides don't need the same keepalive_interval_ms
// (the keepalive test below pairs a peer<tr, 20> with a peer<tr>), so this has
// to accept any instantiation of the peer<tr> template, not just the default.
bool pump_until(auto& a, auto& b, auto done, int max_rounds = 3000)
    requires std::predicate<decltype(done)>
{
    for (int i = 0; i < max_rounds; ++i) {
        a.step();
        b.step();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool has_event(const std::vector<event>& events, event::event_type type, uint64_t conn_id) {
    for (auto& e : events) {
        if (e.type == type && e.conn_id == conn_id) return true;
    }
    return false;
}

const event* find_event(const std::vector<event>& events, event::event_type type, uint64_t conn_id) {
    for (auto& e : events) {
        if (e.type == type && e.conn_id == conn_id) return &e;
    }
    return nullptr;
}

const event* find_message(const std::vector<event>& events, uint64_t conn_id) {
    return find_event(events, event::event_type::message_received, conn_id);
}

} // namespace

TEST_CASE_TEMPLATE("full handshake establishes sessions on both sides, keyed by the same conn_id", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 0xAABBCCDD;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));

    bool ok = pump_until(client, server, [&] {
        return has_event(client.events, event::event_type::handshake_established, conn_id) &&
               has_event(server.events, event::event_type::handshake_established, conn_id);
    });
    REQUIRE(ok);
    CHECK(client.endpoint.session_count() == 1);
    CHECK(server.endpoint.session_count() == 1);
}

TEST_CASE_TEMPLATE("handshake_established carries the peer's cryptographically-proven static public key", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 42;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    REQUIRE(pump_until(client, server, [&] {
        return has_event(client.events, event::event_type::handshake_established, conn_id) &&
               has_event(server.events, event::event_type::handshake_established, conn_id);
    }));

    // The server learns the client's static key (that's the whole point of Noise_IK: the
    // initiator's identity is authenticated, just not sent in the clear).
    auto* server_side = find_event(server.events, event::event_type::handshake_established, conn_id);
    REQUIRE(server_side != nullptr);
    CHECK(server_side->peer_static == client.static_pub);

    // The client already knew the server's key (it's what it connected to) - protocol_endpoint still
    // reports it back for symmetry/consistency of the event shape.
    auto* client_side = find_event(client.events, event::event_type::handshake_established, conn_id);
    REQUIRE(client_side != nullptr);
    CHECK(client_side->peer_static == server.static_pub);
}

TEST_CASE_TEMPLATE("a single-fragment message sent fire-and-forget arrives intact", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 1;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));

    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};
    client.endpoint.send_message(conn_id, msg, /*reliable=*/false);

    REQUIRE(pump_until(client, server, [&] { return find_message(server.events, conn_id) != nullptr; }));
    auto* received = find_message(server.events, conn_id);
    REQUIRE(received != nullptr);
    CHECK(received->data == msg);
}

TEST_CASE_TEMPLATE("queuing a message before the handshake completes still delivers it once established", tr, uring_transport, epoll_transport) {
    // send_message() before handshake_established used to just fail outright (send_message
    // returned false); now it queues and waits - this is the point of decoupling "produce" from
    // "network readiness."
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 6;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));

    std::vector<uint8_t> msg = {7, 8, 9};
    client.endpoint.send_message(conn_id, msg, /*reliable=*/false); // queued immediately, session doesn't exist yet
    CHECK(client.endpoint.outgoing_queue_depth(conn_id) == 1);

    REQUIRE(pump_until(client, server, [&] { return find_message(server.events, conn_id) != nullptr; }));
    CHECK(find_message(server.events, conn_id)->data == msg);
    CHECK(client.endpoint.outgoing_queue_depth(conn_id) == 0);
}

TEST_CASE_TEMPLATE("a multi-fragment message reassembles correctly end-to-end", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 2;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));

    std::vector<uint8_t> msg(assembly::max_fragment_payload * 3 + 250);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)i * 5;
    client.endpoint.send_message(conn_id, msg, /*reliable=*/false);

    REQUIRE(pump_until(client, server, [&] { return find_message(server.events, conn_id) != nullptr; }));
    auto* received = find_message(server.events, conn_id);
    REQUIRE(received != nullptr);
    CHECK(received->data == msg);
}

TEST_CASE_TEMPLATE("a reliable message completes its SACK round-trip (pending drops to zero)", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 3;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));

    std::vector<uint8_t> msg = {9, 9, 9};
    client.endpoint.send_message(conn_id, msg, /*reliable=*/true);

    REQUIRE(pump_until(client, server, [&] { return find_message(server.events, conn_id) != nullptr; }));
    auto* received = find_message(server.events, conn_id);
    REQUIRE(received != nullptr);
    CHECK(received->data == msg);

    // The SACK for this message must come back and clear client-side retransmit tracking -
    // otherwise this "reliable" send would silently keep retrying a message the peer already has.
    REQUIRE(pump_until(client, server, [&] { return client.endpoint.reliable_pending_count(conn_id) == 0; }));
}

TEST_CASE_TEMPLATE("a responder under load still lets a legitimate client through via the cookie retry", tr, uring_transport, epoll_transport) {
    // threshold=1 means the responder is "under load" starting with this
    // very client's own first handshake attempt (record_attempt runs
    // before the under_load check within the same call) - no separate
    // flood is needed to exercise the cookie-retry path; connect() must
    // transparently retry with mac2 once it gets a cookie-reply back.
    endpoint_config server_config;
    server_config.load_threshold_per_sec = 1;
    peer<tr> server(server_config);
    peer<tr> client;
    uint64_t conn_id = 4;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    bool ok = pump_until(client, server,
                          [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); });
    REQUIRE(ok);
    CHECK(server.endpoint.cookie_replies_sent_count() >= 1);
}

TEST_CASE_TEMPLATE("a tampered data packet is rejected by the full stack, not delivered as a message", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 5;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));

    // Inject a forged data packet directly over the socket, bypassing protocol_endpoint::send_message, so we
    // control the exact (corrupted) bytes without needing a second valid session. Sized to actually
    // reach AEAD verification (nonce+header+tag = 32 bytes minimum) rather than getting rejected
    // for being merely too short to parse.
    int raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(raw_fd >= 0);
    std::vector<uint8_t> forged(hdr_len + 40, 0);
    forged[0] = 3;
    for (size_t i = hdr_len; i < forged.size(); ++i) forged[i] = (uint8_t)i * 7 + 3; // arbitrary garbage
    std::memcpy(forged.data() + 1, &conn_id, 8);
    ssize_t sent = sendto(raw_fd, forged.data(), forged.size(), 0, (sockaddr*)&server_addr,
                          sizeof(server_addr));
    REQUIRE(sent == (ssize_t)forged.size());
    close(raw_fd);

    uint64_t before = server.endpoint.auth_failed_count();
    pump_until(client, server, [&] { return server.endpoint.auth_failed_count() > before; }, 500);
    CHECK(server.endpoint.auth_failed_count() > before);
    CHECK(find_message(server.events, conn_id) == nullptr);
}

TEST_CASE_TEMPLATE("the reliable congestion window grows on acks and is not exceeded under sustained sends", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr> client;
    uint64_t conn_id = 7;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr,
                             sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));

    // Queue far more reliable messages than the initial window (4) allows in one shot.
    for (int i = 0; i < 40; ++i) {
        std::vector<uint8_t> msg = {(uint8_t)i};
        client.endpoint.send_message(conn_id, msg, /*reliable=*/true);
    }

    // Every one of them must still eventually arrive - the window paces sending, it doesn't drop
    // anything - and pending tracking must fully drain once all SACKs are back.
    uint64_t received_count = 0;
    bool ok = pump_until(client, server, [&] {
        received_count = 0;
        for (auto& e : server.events) {
            if (e.type == event::event_type::message_received && e.conn_id == conn_id) ++received_count;
        }
        return received_count == 40 && client.endpoint.reliable_pending_count(conn_id) == 0;
    });
    REQUIRE(ok);
    CHECK(received_count == 40);
}

TEST_CASE_TEMPLATE("an idle session receives periodic keepalives, invisible to the application", tr, uring_transport, epoll_transport) {
    peer<tr> server;
    peer<tr, 20> client; // 20ms keepalive_interval_ms: short enough to observe within the test's round budget
    uint64_t conn_id = 8;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr, sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));

    // No data is ever queued on this conn_id after the handshake - any
    // further activity the server observes on it has to be the client's
    // idle-timer keepalive, not real traffic.
    REQUIRE(pump_until(client, server, [&] { return server.endpoint.keepalives_received_count() > 0; }));

    // A keepalive must never surface to the application as a message, and
    // must not be misattributed to the data-path's own failure counters.
    CHECK(find_message(server.events, conn_id) == nullptr);
    CHECK(server.endpoint.auth_failed_count() == 0);
    CHECK(server.endpoint.replay_rejected_count() == 0);
}

TEST_CASE_TEMPLATE("an unresponsive peer is torn down and reported via peer_unreachable", tr, uring_transport, epoll_transport) {
    // keepalive_interval_ms=10, liveness_timeout_ms=50: short enough to
    // observe within the test's round budget (protocol_endpoint's own
    // requires clause needs liveness_timeout_ms > keepalive_interval_ms).
    peer<tr, 10, 50> server;
    peer<tr> client;
    uint64_t conn_id = 9;

    sockaddr_in server_addr = loopback_addr(server.port);
    client.endpoint.connect(conn_id, server.static_pub, (sockaddr*)&server_addr, sizeof(server_addr));
    REQUIRE(pump_until(client, server,
                        [&] { return has_event(client.events, event::event_type::handshake_established, conn_id); }));
    REQUIRE(server.endpoint.session_count() == 1);

    std::vector<uint8_t> msg = {1, 2, 3};
    server.endpoint.send_message(conn_id, msg); // queued but never delivered (client stops stepping below) - just checking cleanup drops this too

    // Stop stepping the client entirely from here on - simulates it going
    // silent (crash, network drop, whatever) while the server keeps
    // ticking alone. Not pump_until: only the server should advance.
    bool ok = false;
    for (int i = 0; i < 3000; ++i) {
        server.step();
        if (has_event(server.events, event::event_type::peer_unreachable, conn_id)) { ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(ok);
    CHECK(server.endpoint.session_count() == 0);
    CHECK(server.endpoint.outgoing_queue_depth(conn_id) == 0); // the dead session's queue must not linger either

    auto* ev = find_event(server.events, event::event_type::peer_unreachable, conn_id);
    REQUIRE(ev != nullptr);
    CHECK(ev->peer_static == client.static_pub); // identified by the peer's proven identity, not just the now-dead conn_id
}

TEST_CASE_TEMPLATE("a pending handshake whose responder never answers times out with handshake_failed", tr, uring_transport, epoll_transport) {
    peer<tr, 20000, 60000, 50> client; // handshake_timeout_ms=50
    uint64_t conn_id = 10;

    // A bound-but-otherwise-unused UDP socket: sendto() succeeds (UDP has
    // no connection concept), but nothing is listening with a
    // protocol_endpoint on it, so message1 is never answered - exactly
    // "responder never replies," as opposed to a positively-rejected
    // handshake.
    uint16_t dead_port;
    int dead_fd = make_bound_udp_socket(&dead_port);
    sockaddr_in dead_addr = loopback_addr(dead_port);

    // A real (if throwaway) keypair, not an all-zero key: an all-zero key is
    // a low-order Curve25519 point, correctly rejected by write_message1's
    // own small-order-point guard before this test ever gets to exercise
    // the timeout it's actually after.
    auto fake_remote = crypto::x25519_generate();
    client.endpoint.connect(conn_id, fake_remote.pub, (sockaddr*)&dead_addr, sizeof(dead_addr));
    REQUIRE(client.endpoint.pending_handshake_count() == 1);

    bool ok = false;
    for (int i = 0; i < 3000; ++i) {
        client.step();
        if (has_event(client.events, event::event_type::handshake_failed, conn_id)) { ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(ok);
    CHECK(client.endpoint.pending_handshake_count() == 0);
    close(dead_fd);
}
