#pragma once

// Level 7: the actual protocol. Every earlier level's demo server was one
// slice of exactly this, built in isolation to benchmark and attack-test
// one concern at a time:
//   - level 1:  multishot recvmsg + provided buffers (multishot_udp_receiver)
//   - level 2:  Noise_IK handshake + mac1 flood filter (noise_ik.hpp)
//   - level 3:  fragment reassembly via sliding-window bitmask (packet_assembly.hpp)
//   - level 4:  AEAD with the fragment header as associated data (transport_connection.hpp)
//   - level 5:  replay window + stateless cookie under load (replay_window.hpp, anti_dos.hpp)
//   - level 6:  optional per-message reliability (reliability.hpp)
// protocol_endpoint wires all six together into one coherent, reusable class instead
// of the ad hoc copy-pasted event loops each level's standalone demo used.
//
// Wire format (unified across every prior level's demo, unchanged from
// each):
//   1 = handshake message1: type(1) | conn_id(8) | noise_msg1(with mac1) | mac2(16, zero if none yet)
//   2 = handshake message2: type(1) | conn_id(8) | noise_msg2
//   3 = data packet:        type(1) | conn_id(8) | <connection wire packet>
//   4 = cookie reply:       type(1) | conn_id(8) | cookie(16)
//   5 = SACK batch:         type(1) | conn_id(8) | count(1) | count * 14-byte entries
//   6 = keepalive:          type(1) | conn_id(8) | <connection keepalive wire packet: nonce(8) | AEAD tag(16), no payload>
//
// Packet 1's type+conn_id+mac2 are disguised on the wire (see "handshake
// header obfuscation" below): what a passive observer sees at offsets 0-8 and
// 121-136 of a fresh connection's first packet is not the values above but a
// keystream, indistinguishable from the AEAD ciphertext next to it. This is
// against DPI fingerprinting, not against a peer who already knows this
// project's key material - see net/DPI_RESISTANCE.md for the threat model,
// what this closes, and what it deliberately does not (packet types 2-6 are
// unaffected, for reasons specific to each and stated there).
//
// Cleanup of hanging connections (two distinct cases, both handled in
// tick()): an established session whose peer has gone quiet past
// liveness_timeout is torn down and reported via peer_unreachable - this
// needs actual received-packet tracking (last_received_time), not just
// "haven't sent anything," since a peer can easily receive fine while its
// own packets never arrive back. A pending outbound connect() whose
// responder never answers at all (packet lost, host unreachable, firewall
// drop - connect() was called but message2 never came) times out
// separately via handshake_timeout and reports handshake_failed; this is
// a different failure mode from an established session going quiet and
// was previously not detected at all (a never-answered connect() just sat
// in pending_initiators forever).
//
// Keepalive (type 6) exists purely to stop an idle session's UDP mapping
// from expiring in some NAT/firewall's table (carrier-grade NAT in
// particular can time out an idle binding in well under a minute) - see
// tick()'s idle-check for when it actually fires. It's AEAD-authenticated
// (an unauthenticated ping would be a free forgeable no-op an attacker could
// use to keep a session's replay window ticking or just waste CPU) but
// carries no data of its own and bypasses the reassembler entirely: it must
// never reach the application as a message_received event, so handle_cqe's
// case 6 routes it to handle_keepalive(), not handle_data().
//
// Single-threaded, one protocol_endpoint per UDP socket - exactly the
// model every level was benchmarked under. Horizontal scaling (level 1's
// research note on SO_REUSEPORT + shared-nothing workers) means running N
// independent Endpoints, each owning its own ring/socket/static identity's
// session table, not sharing one protocol_endpoint across threads.
//
// I/O shape: two queues, not callbacks. handle_cqe() only ever files
// results away; poll_incoming() drains everything that arrived since the
// last call in one batch, for the caller to process however it wants (a
// loop, a switch, handed off elsewhere - not stuck inside a callback whose
// call stack is dictated by drain_cqes). send_message() is the mirror
// image: it queues data and returns immediately, never touching the
// network inline - the actual encrypt+send happens in tick(), which is
// also where reliable traffic's congestion window is enforced (see
// reliability.hpp's reliable_sender::can_send()). Queuing before a session
// exists is fine: queued data just waits for handshake_established.

#include "anti_dos.hpp"
#include "noise_ik.hpp"
#include "reliability.hpp"
#include "transport_connection.hpp"
#include "io_backend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace protocol {


// 96 entries * 14B + 1B count + 9B header = 1354B, comfortably under the MTU - see the same
// constant's reasoning in level6_reliability_server.cpp, which this supersedes.
constexpr size_t max_sack_entries_per_packet = 96;

struct endpoint_config {
    uint32_t load_threshold_per_sec = 2000; // anti_dos::load_gate: handshake attempts/sec before cookies are required
    // No reassembly_window field: reassembler's window size is a compile-time template
    // parameter now (see packet_assembly.hpp), not a runtime-configurable one.
    unsigned recv_num_bufs = 4096;          // io_uring backend only: must be a power of two, see multishot_udp.hpp
    unsigned recv_buf_size = 2048;
    unsigned send_pool_size = 1024;

    // Starting point for each connection's reliable-mode congestion window
    // (see reliability::reliable_sender). Defaults match reliable_sender's
    // own conservative defaults - safe for a link of unknown capacity.
    // Raise these only if you already know this deployment's real link
    // capacity from prior measurement and want to skip most of the
    // slow-start ramp; see reliable_sender's constructor comment for why
    // guessing too high on an unknown link risks an early collapse instead
    // of saving time.
    size_t reliable_initial_cwnd = 4;
    size_t reliable_initial_ssthresh = 64;
    // No keepalive_interval field: like reassembler's window size, every
    // real deployment uses the same interval, so it's protocol_endpoint's
    // own compile-time template parameter (see its declaration) instead of
    // a runtime-configurable one here.
};

struct event {
    // Named event_type, not just type: a nested type and a data member
    // can't share one identifier and still let code outside the class
    // spell the type by qualified name (event::type would ambiguously
    // prefer the data member over the nested enum in that context) -
    // this is the one place in the rename pass where "member matches its
    // natural name" and "type matches its natural name" collide, so the
    // type gets the more specific name instead.
    enum class event_type {
        handshake_established, // conn_id is now in send_message()-able state; peer_static is who it is
        handshake_failed,      // a pending outbound connect() was rejected, or its responder never answered at all
        message_received,      // data holds one fully reassembled, authenticated message
        peer_unreachable,      // an established session's peer has gone quiet past liveness_timeout; conn_id/peer_static identify who - the session is already torn down by the time this fires
    };
    event_type type = event_type::handshake_established;
    uint64_t conn_id = 0;
    std::vector<uint8_t> data;

    // Populated for handshake_established: the peer's static public key,
    // cryptographically proven by Noise_IK (the handshake only completes if
    // whoever sent it actually holds the matching private key - see the
    // level-7 design discussion on why this is safe to expose but the
    // *decision* of what to do with it - allow, log, whitelist, rate-limit
    // per-key - is deliberately left to the caller, not decided here).
    crypto::public_key peer_static{};

    // Populated for message_received: this message's transport-level
    // msg_id (monotonic per connection). Reassembly already resolves
    // within-message fragment reordering; it says nothing about whether
    // message 5 might complete before message 4 does. Most callers can
    // ignore this - it exists for the one kind of caller that can't
    // (replication, see replication.hpp): apply messages in msg_id order,
    // buffering anything that arrives ahead of the next expected one.
    uint32_t msg_id = 0;
};

// One protocol endpoint: one UDP socket, one static identity, a table of
// established sessions and in-progress handshakes (both as responder and as
// initiator - the same class plays either role, since a real peer-to-peer
// capable protocol needs both and the bookkeeping is identical either way).
//
// keepalive_interval_ms/liveness_timeout_ms/handshake_timeout_ms are
// template parameters, not endpoint_config fields, for the same reason
// reassembler's window_capacity is (see packet_assembly.hpp): every real
// deployment wants the same values, so what's worth catching at compile
// time (0 would fire a keepalive every single tick, drowning real traffic;
// a liveness_timeout no bigger than the keepalive interval would time out
// a session before it ever got a chance to receive one) is a `requires`
// clause instead of a runtime check. Expressed as milliseconds-as-size_t
// rather than std::chrono::milliseconds NTTPs directly because
// chrono::duration isn't a structural type (its representation member
// isn't guaranteed public), so it can't be used as a non-type template
// parameter itself.
// `io` is the network and nothing else - see io_backend.hpp for the three calls
// that make one. It is a template parameter rather than a base class because
// the alternative is a virtual dispatch on the per-packet path, and because the
// protocol genuinely does not care: it needs bytes in and bytes out.
// No default for io_backend, deliberately. Defaulting it to uring_io meant this
// header had to include uring_io.hpp, which includes liburing.h - so a protocol
// that no longer DEPENDS on io_uring still could not be COMPILED without it,
// which was discovered the first time it was cross-compiled for Android. Say
// which transport you want and include it; there are two.
template <typename io_backend,
          size_t keepalive_interval_ms = 20000, size_t liveness_timeout_ms = 60000, size_t handshake_timeout_ms = 5000>
    requires (keepalive_interval_ms > 0 && liveness_timeout_ms > keepalive_interval_ms && handshake_timeout_ms > 0)
class protocol_endpoint {
public:
    // 20s (the default) comfortably undercuts typical NAT UDP-binding
    // timeouts (commonly 30s-5min, tighter under carrier-grade NAT) while
    // staying infrequent enough not to matter for bandwidth/battery on an
    // otherwise-idle connection.
    static constexpr std::chrono::milliseconds keepalive_interval{keepalive_interval_ms};
    // 60s (the default): ~3 missed keepalive cycles at the default
    // interval before a session is declared dead - tolerates a run of
    // ordinary packet loss without flapping, while still noticing a truly
    // gone peer well before anyone would call it "hung" by hand.
    static constexpr std::chrono::milliseconds liveness_timeout{liveness_timeout_ms};
    // 5s (the default): a handshake round trip on any real path completes
    // in well under a second; this just bounds how long a connect() to a
    // genuinely unreachable address is left pending before handshake_failed
    // fires, distinct from - and much shorter than - liveness_timeout,
    // since an established session going quiet and a handshake that never
    // got a first reply are different failure modes with different normal
    // timescales.
    static constexpr std::chrono::milliseconds handshake_timeout{handshake_timeout_ms};

    protocol_endpoint(io_backend& io, crypto::key_pair local_static, endpoint_config config = {})
        : io(io), local_static(std::move(local_static)), config(config),
          cookie_state(), load_state(config.load_threshold_per_sec) {}

    // Call once at startup.
    void arm() { io.arm(); }

    // Wait up to `timeout` for traffic and process everything that arrives.
    // Replaced handle_cqe(cqe): a completion queue is an io_uring concept and
    // every caller was writing the same submit-then-drain dance around it.
    // Results land in the queue poll_incoming() drains.
    void poll_io(std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
        io.poll(timeout, [this](const datagram& d) { on_datagram(d); });
    }

    // Initiator role: begin a handshake to a known peer. `conn_id` is
    // caller-chosen and must be unique among this endpoint's current
    // sessions/pending handshakes - it's how send_message() and the
    // eventual handshake_established event refer back to this peer.
    // Transparently retries with mac2 if the responder is under load and
    // challenges with a cookie reply (type 4) - the caller only ever sees
    // handshake_established or handshake_failed, never the intermediate hop.
    void connect(uint64_t conn_id, const crypto::public_key& remote_static, const sockaddr* addr, socklen_t addr_len) {
        noise::noise_ik_initiator initiator(local_static, remote_static);
        auto msg1 = initiator.write_message1(nullptr, 0);

        pending_initiator pending{std::move(initiator), msg1, remote_static, {}, addr_len, std::chrono::steady_clock::now()};
        std::memcpy(&pending.addr, addr, addr_len);
        pending_initiators.insert_or_assign(conn_id, std::move(pending));

        std::vector<uint8_t> wire(msg1.size() + anti_dos::cookie_len, 0); // real mac2: zero, no cookie yet on the first try
        std::memcpy(wire.data(), msg1.data(), msg1.size());

        uint8_t obf_type = 1;
        uint64_t obf_conn_id = conn_id;
        crypto::public_key ephemeral;
        std::memcpy(ephemeral.data(), msg1.data(), crypto::key_len);
        xor_msg1_header(obf_type, obf_conn_id, wire.data() + wire.size() - anti_dos::cookie_len, remote_static, ephemeral.data());

        send_wire(obf_type, wire.data(), wire.size(), obf_conn_id, addr, addr_len);
    }

    // Queues `data` for conn_id. Does not touch the network here - actual
    // encryption and sending happens in tick(). Safe to call before the
    // session exists yet (right after connect(), say): queued data simply
    // waits for handshake_established. There is currently no feedback if
    // conn_id never establishes (see the level-7 design notes) - a queued
    // send for a conn_id that's rejected or never used just sits there;
    // callers that need that signal should watch for handshake_failed
    // themselves and stop queuing.
    //
    // `reliable` routes the message through reliability::reliable_sender
    // (SACK'd, retransmitted on loss, subject to its congestion window -
    // see can_send() in reliability.hpp); otherwise it's the fire-and-forget
    // path every level 1-4 benchmark used, sent as soon as tick() runs,
    // with no window at all (there's no ack to grow or shrink one from).
    void send_message(uint64_t conn_id, std::vector<uint8_t> data, bool reliable = false) { outgoing_queues[conn_id].push_back(queued_send{std::move(data), reliable}); }

    // One received datagram, already framed. Called by poll_io() for each one
    // the backend delivers; results land in the queue poll_incoming() drains.
    void on_datagram(const datagram& frag) {
        if (frag.payload_len <= hdr_len) return;

        // Every genuine message1 is exactly msg1_wire_len bytes with its
        // header and mac2 obfuscated (xor_msg1_header, above), so a packet
        // shaped like one is tried that way first. The length match is not
        // proof - it only gates the cost of trying - so a wrong guess falls
        // straight through to the plaintext interpretation below. The real
        // decision is mac1, inside handle_message1: unrelated bytes that
        // happen to be 137 long produce a type byte that, on the roughly
        // 0.8% chance it lands on 1 anyway, cost one wasted HMAC and nothing
        // more, since mac1 rejects everything that isn't a real handshake
        // attempt from someone who knows this responder's static key.
        if (frag.payload_len == msg1_wire_len) {
            uint8_t trial_type = frag.payload[0];
            uint64_t trial_conn_id;
            std::memcpy(&trial_conn_id, frag.payload + 1, 8);
            std::array<uint8_t, msg1_wire_len - hdr_len> trial_body;
            std::memcpy(trial_body.data(), frag.payload + hdr_len, trial_body.size());

            xor_msg1_header(trial_type, trial_conn_id, trial_body.data() + trial_body.size() - anti_dos::cookie_len,
                            local_static.pub, trial_body.data());
            if (trial_type == 1) {
                handle_message1(trial_conn_id, trial_body.data(), trial_body.size(), frag.peer_addr, frag.peer_addr_len);
                return;
            }
        }

        uint8_t type = frag.payload[0];
        uint64_t conn_id;
        std::memcpy(&conn_id, frag.payload + 1, 8);
        const uint8_t* body = frag.payload + hdr_len;
        size_t body_len = frag.payload_len - hdr_len;

        switch (type) {
            case 1: handle_message1(conn_id, body, body_len, frag.peer_addr, frag.peer_addr_len); break;
            case 2: handle_message2(conn_id, body, body_len); break;
            case 3: handle_data(conn_id, body, body_len); break;
            case 4: handle_cookie_reply(conn_id, body, body_len); break;
            case 5: handle_sack(conn_id, body, body_len); break;
            case 6: handle_keepalive(conn_id, body, body_len); break;
            default: break;
        }
    }

    // Drains and returns every event accumulated since the last call, in
    // arrival order - a batch, not a callback. Cheap: an internal
    // vector-move, not a per-element copy.
    std::vector<event> poll_incoming() {
        auto out = std::move(incoming_queue);
        incoming_queue.clear(); // clear the moved-from husk so it's a valid empty vector again, not just unspecified
        return out;
    }

    // Call once per event-loop iteration, after poll_io() has delivered
    // whatever arrived: sends any due SACKs,
    // fires any due retransmits, and flushes whatever send_message() has
    // queued (subject to each reliable session's congestion window). This
    // is where the "batch everything" principle from every earlier level
    // lands for level 7.
    void tick(std::chrono::steady_clock::time_point now) {
        // Manual iterator, not a range-based for: a dead session gets erased
        // mid-loop below, which a range-based for over the same container
        // can't do safely.
        for (auto it = sessions.begin(); it != sessions.end();) {
            uint64_t conn_id = it->first;
            peer_session& session = it->second;

            // Liveness cleanup, checked before anything else: a session
            // that's gone quiet past liveness_timeout is torn down here,
            // not left to accumulate SACKs/retransmits/keepalives for a
            // peer that's already gone. peer_static - not just conn_id -
            // is what lets a caller recognize *who* this was.
            if (now - session.last_received_time > liveness_timeout) {
                incoming_queue.push_back(event{event::event_type::peer_unreachable, conn_id, {}, session.peer_static});
                outgoing_queues.erase(conn_id);
                it = sessions.erase(it);
                continue;
            }

            bool sent_anything = false;

            auto acks = session.conn.drain_dirty_acks();
            for (size_t i = 0; i < acks.size(); i += max_sack_entries_per_packet) {
                size_t n = std::min(max_sack_entries_per_packet, acks.size() - i);
                std::vector<uint8_t> body(1 + n * reliability::sack_wire_len);
                body[0] = (uint8_t)n;
                for (size_t j = 0; j < n; ++j) {
                    auto entry = reliability::encode_sack(acks[i + j]);
                    std::memcpy(body.data() + 1 + j * reliability::sack_wire_len, entry.data(), entry.size());
                }
                send_wire(5, body.data(), body.size(), conn_id, (sockaddr*)&session.addr, session.addr_len);
                sent_anything = true;
            }

            if (session.reliable.has_value()) {
                auto retransmits = session.reliable->collect_retransmits(now);
                for (auto& pkt : retransmits) send_wire(3, pkt.data(), pkt.size(), conn_id, (sockaddr*)&session.addr, session.addr_len);
                sent_anything |= !retransmits.empty();
            }

            sent_anything |= flush_outgoing_for(conn_id, session);

            // NAT keepalive: only once this session has gone keepalive_interval
            // with nothing of its own to send - a session that's already
            // chatty (data, SACKs, retransmits) gets its idle clock reset for
            // free above and never needs a keepalive stacked on top of real
            // traffic.
            if (!sent_anything && now - session.last_send_time >= keepalive_interval) {
                auto ka = session.conn.encrypt_keepalive();
                send_wire(6, ka.data(), ka.size(), conn_id, (sockaddr*)&session.addr, session.addr_len);
                sent_anything = true;
            }

            if (sent_anything) session.last_send_time = now;
            ++it;
        }

        // Pending handshakes whose responder never answered at all (see
        // the file header comment) - a distinct cleanup pass from the one
        // above since these live in a different table with no session to
        // tear down, just the attempt itself.
        for (auto it = pending_initiators.begin(); it != pending_initiators.end();) {
            if (now - it->second.started_at > handshake_timeout) {
                incoming_queue.push_back(event{event::event_type::handshake_failed, it->first, {}, {}});
                it = pending_initiators.erase(it);
            } else {
                ++it;
            }
        }

        cookie_state.maybe_rotate(now);
    }

    size_t session_count() const { return sessions.size(); }
    size_t pending_handshake_count() const { return pending_initiators.size(); }

    // Test/introspection hook: how many fragments of conn_id's reliable
    // traffic are still awaiting a SACK. Returns 0 for an unknown conn_id
    // or one that was never sent anything via send_message(..., true).
    size_t reliable_pending_count(uint64_t conn_id) const {
        auto it = sessions.find(conn_id);
        if (it == sessions.end() || !it->second.reliable.has_value()) return 0;
        return it->second.reliable->pending_count();
    }

    // Introspection: the reliable congestion window's current size (0 if
    // conn_id has no reliable traffic yet). Useful for watching AIMD
    // behave, or for a caller that wants to expose its own metrics.
    size_t reliable_cwnd(uint64_t conn_id) const {
        auto it = sessions.find(conn_id);
        if (it == sessions.end() || !it->second.reliable.has_value()) return 0;
        return it->second.reliable->cwnd();
    }

    // How many messages are queued for conn_id but not yet handed to
    // encrypt/send by tick() - lets a producer keep the queue topped up to
    // a target depth without needing its own separate accounting of what
    // protocol_endpoint has already drained.
    size_t outgoing_queue_depth(uint64_t conn_id) const {
        auto it = outgoing_queues.find(conn_id);
        return it == outgoing_queues.end() ? 0 : it->second.size();
    }

    uint64_t dropped_no_send_buf() const { return dropped_missing_send_buffer_count; }
    uint64_t handshake_rejected_count() const { return handshake_rejected; }
    uint64_t auth_failed_count() const { return auth_failed; }
    // Both were silently swallowed by handle_data() before - no counter, no
    // event, nothing distinguishing them from any other dropped packet.
    // That made a real production bug invisible: a peer's byte-identical
    // retransmit of an already-committed nonce is *correctly* rejected by
    // the replay window (see replay_window.hpp), but if the original
    // delivery's own SACK was ever lost, the sender has no way to learn its
    // data actually arrived - it just keeps retransmitting, and every
    // retry keeps landing here instead of ever being acked. Watch this
    // counter alongside reliable_pending_count(): pending stuck at a fixed
    // value while this climbs is exactly that scenario, not a hang.
    uint64_t replay_rejected_count() const { return replay_rejected; }
    uint64_t reassembly_rejected_count() const { return reassembly_rejected; }
    uint64_t dh_attempts_count() const { return dh_attempts; }
    uint64_t cookie_replies_sent_count() const { return cookie_replies_sent; }
    // Counts every inbound type=6 packet handed to handle_keepalive(),
    // whether or not it actually authenticated - a deployment watching NAT
    // health cares that keepalives are arriving at all; per-packet auth
    // failures are already visible via the underlying connection's own
    // auth_failures()/replay_rejections() if that finer detail is ever
    // needed.
    uint64_t keepalives_received_count() const { return keepalives_received; }

private:
    struct peer_session {
        transport::connection<> conn;
        std::optional<reliability::reliable_sender<>> reliable;
        sockaddr_storage addr{};
        socklen_t addr_len = 0;
        // Last time tick() actually put a packet on the wire for this
        // session (data, SACK, retransmit, or keepalive itself) - the idle
        // clock the keepalive check in tick() measures against. Seeded at
        // session-creation time so a brand-new session doesn't look
        // "idle since the epoch" and fire a redundant keepalive on its very
        // first tick.
        std::chrono::steady_clock::time_point last_send_time{};
        // Last time anything authenticated (data, keepalive - see the
        // handlers below) arrived from this peer - the liveness clock
        // tick()'s cleanup pass measures against. Seeded at session-creation
        // time for the same reason last_send_time is: a brand-new session
        // must not look "already gone quiet" on its very first tick.
        std::chrono::steady_clock::time_point last_received_time{};
        // The peer's cryptographically-proven static public key - the same
        // value the handshake_established event carried. Kept here too
        // because conn_id is caller-chosen and ephemeral (a reconnect gets
        // a brand-new one), while peer_static is the one thing that
        // identifies "the same peer" across a reconnect. Needed for
        // peer_unreachable (so a caller learns *who* went quiet, not just
        // which now-meaningless conn_id) and for anything that wants to
        // recognize a returning peer by identity rather than by conn_id.
        crypto::public_key peer_static{};
    };

    struct pending_initiator {
        noise::noise_ik_initiator initiator;
        std::vector<uint8_t> msg1; // retained so a cookie retry can compute mac2 over it
        crypto::public_key remote_static{}; // carried through to the eventual handshake_established event
        sockaddr_storage addr{};
        socklen_t addr_len = 0;
        // When this attempt last had a reason to still look alive: set by
        // connect() and refreshed by handle_cookie_reply() (a cookie reply
        // proves the responder is there, just under load - that's still
        // progress, not silence). tick()'s cleanup pass measures
        // handshake_timeout against this, not against connect()'s original
        // call time, so a slow-but-progressing cookie retry isn't punished
        // the same as a responder that never answers at all.
        std::chrono::steady_clock::time_point started_at{};
    };

    // The two things a backend's send buffer actually has to hold: a single
    // max-size data packet
    // (nonce + fragment header + largest payload + AEAD tag), and a full SACK batch
    // (see max_sack_entries_per_packet's own comment on the 1354-byte figure). Both are
    // computed from the same constants the wire format itself uses, not re-guessed here -
    // if either grows past what a send buffer can hold, this fails the build instead of
    // silently truncating a packet at runtime.
    static constexpr size_t max_data_wire_packet_len =
        hdr_len + transport::nonce_len + assembly::fragment_header_len + assembly::max_fragment_payload + crypto::aead_tag_len;
    static constexpr size_t max_sack_wire_packet_len = hdr_len + 1 + max_sack_entries_per_packet * reliability::sack_wire_len;
    static constexpr size_t max_keepalive_wire_packet_len = hdr_len + transport::nonce_len + crypto::aead_tag_len;
    static_assert(max_data_wire_packet_len <= 2048 && max_sack_wire_packet_len <= 2048 && max_keepalive_wire_packet_len <= 2048,
                  "a backend send buffer must hold the biggest data, SACK or keepalive packet the wire format can produce");

    // ---- handshake header obfuscation (packet type 1 only) ---------------
    //
    // net/DPI_RESISTANCE.md, item 1: the worst of the four fingerprintable
    // artefacts in this wire format was 16 zero bytes at a fixed offset in
    // the first packet of every flow - mac2, all-zero until a cookie has
    // been issued. This is what removes it, plus the plaintext type byte and
    // conn_id alongside it, without adding a single byte to the wire.
    //
    // SCOPE, stated rather than implied by silence. This covers packet type
    // 1 and nothing else, and each exclusion is a reason, not an oversight:
    //
    //   Type 2 (handshake response) is not covered. The receiver - the
    //   original initiator - would need to try every pending connection's
    //   remote_static to find the one key that deobfuscates correctly, since
    //   conn_id (which key to try) is exactly what obfuscation would hide.
    //   Type 1 has no such problem: a responder has exactly one static key,
    //   so trying "is this an obfuscated msg1" costs one fixed lookup
    //   regardless of how many connections exist.
    //
    //   Types 3 and 6 (data, keepalive) are not covered. Their own AEAD nonce
    //   is a natural, free, per-packet-unique value to drive a keystream
    //   from - but it is unique only within ONE connection's send direction,
    //   restarting at 0 for every new one. Reusing it as a keystream nonce
    //   under a key shared by every connection this endpoint holds (the only
    //   kind of key a receiver could derive without already knowing which
    //   connection a packet belongs to) would let two different connections'
    //   headers be XORed together by anyone who captured both - solvable
    //   with a per-connection key, which reintroduces the exact demux problem
    //   above. Left as a documented gap rather than a rushed fix.
    //
    //   Types 4 and 5 (cookie reply, SACK) are not covered. Neither carries a
    //   nonce of its own, and SACKs are sent unauthenticated by design (see
    //   handle_sack's comment) - inventing one for either would be new
    //   machinery bolted onto packets deliberately kept cheap and stateless.
    //
    // KEY DERIVATION, deliberately not a secret shared only by the two
    // parties in one connection. Both sides derive it from nothing but the
    // RESPONDER's static public key - the responder from its own, the
    // initiator from what Noise_IK already required it to know out-of-band
    // - same construction as mac1_key in noise_ik.hpp, different label. That
    // means anyone with this project's client config can compute it, exactly
    // as anyone with it can already compute mac1. The goal is a passive DPI
    // box seeing a keystream it cannot distinguish from AEAD ciphertext, not
    // confidentiality against an adversary who already has the server's
    // public key - that adversary breaks nothing worse than they already
    // could (mac1 offers the same non-guarantee today).
    static crypto::hash hdr_obfs_key(const crypto::public_key& responder_static_pub) {
        static constexpr char label[] = "hdrobfs-"; // 8 bytes, mac1_label's convention
        std::array<uint8_t, 8 + crypto::key_len> buf;
        std::memcpy(buf.data(), label, 8);
        std::memcpy(buf.data() + 8, responder_static_pub.data(), crypto::key_len);
        return crypto::sha256(buf.data(), buf.size());
    }

    // A stream cipher's nonce must be unique under its key, never secret -
    // and message1's own ephemeral public key already is exactly that: drawn
    // fresh from the CSPRNG on every write_message1() call and never reused.
    // Free: it is sent in the clear either way, obfuscation or not.
    //
    // ONE KNOWN REPEAT, stated rather than buried: a cookie retry
    // (handle_cookie_reply below) resends the SAME msg1 bytes with a freshly
    // computed mac2, so it reuses this SAME nonce - the two packets' obfuscated
    // headers come out byte-identical. That links a retry to its original
    // attempt, which source address and timing already do for free, so it
    // adds nothing an observer did not already have. What must never repeat -
    // the same nonce across TWO DIFFERENT connections - cannot: every
    // connect() call draws its own ephemeral key.
    static crypto::stream_nonce hdr_obfs_nonce(const uint8_t* ephemeral_pub_bytes) {
        auto h = crypto::sha256(ephemeral_pub_bytes, crypto::key_len);
        crypto::stream_nonce n{};
        std::memcpy(n.data(), h.data(), n.size());
        return n;
    }

    // Applies or removes the disguise - the same operation either way, since
    // XOR with a keystream is its own inverse. `type` and `conn_id` are
    // exchanged by value (real in, obfuscated out, or the reverse); `mac2_field`
    // is 16 bytes XORed in place. `ephemeral_pub_bytes` points at the first 32
    // bytes of the packet's own noise message, which is always where message1
    // puts its ephemeral key regardless of which direction this call is going.
    static void xor_msg1_header(uint8_t& type, uint64_t& conn_id, uint8_t* mac2_field,
                                const crypto::public_key& responder_static_pub,
                                const uint8_t* ephemeral_pub_bytes) {
        auto key = hdr_obfs_key(responder_static_pub);
        auto nonce = hdr_obfs_nonce(ephemeral_pub_bytes);
        uint8_t keystream[hdr_len + anti_dos::cookie_len];
        crypto::stream_keystream(keystream, sizeof(keystream), nonce, key);

        type = (uint8_t)(type ^ keystream[0]);
        uint64_t conn_id_mask;
        std::memcpy(&conn_id_mask, keystream + 1, 8);
        conn_id ^= conn_id_mask;
        for (size_t i = 0; i < anti_dos::cookie_len; ++i) mac2_field[i] = (uint8_t)(mac2_field[i] ^ keystream[hdr_len + i]);
    }

    // ephemeral(32) + enc_static(32+16) + enc_payload(0+16) + mac1(16) + mac2(16),
    // plus this layer's own 9-byte header. A genuine message1 is always
    // exactly this long - on_datagram() uses that as a cheap pre-filter
    // before even attempting the deobfuscation trial below.
    static constexpr size_t msg1_noise_len =
        crypto::key_len + (crypto::key_len + crypto::aead_tag_len) + crypto::aead_tag_len + crypto::mac_len;
    static constexpr size_t msg1_wire_len = hdr_len + msg1_noise_len + anti_dos::cookie_len;
    static_assert(msg1_wire_len == 137, "DPI_RESISTANCE.md cites this exact number; if it moves, that doc needs updating too");

    struct queued_send {
        std::vector<uint8_t> data;
        bool reliable;
    };

    void send_wire(uint8_t type, const uint8_t* body, size_t body_len, uint64_t conn_id, const sockaddr* addr, socklen_t addr_len) {
        if (!io.send(type, conn_id, body, body_len, addr, addr_len)) ++dropped_missing_send_buffer_count;
    }

    void handle_message1(uint64_t conn_id, const uint8_t* payload, size_t payload_len, const sockaddr* peer_addr, socklen_t peer_addr_len) {
        constexpr size_t mac2_len = anti_dos::cookie_len;
        if (payload_len <= mac2_len) {
            ++handshake_rejected;
            return;
        }
        size_t noise_len = payload_len - mac2_len;
        const uint8_t* noise_msg_ptr = payload;
        const uint8_t* mac2_ptr = payload + noise_len;

        if (!noise::noise_ik_responder::verify_mac1(local_static.pub, noise_msg_ptr, noise_len)) {
            ++handshake_rejected;
            return;
        }

        auto now = std::chrono::steady_clock::now();
        load_state.record_attempt(now);

        if (load_state.under_load(now)) {
            anti_dos::cookie received_mac2;
            std::memcpy(received_mac2.data(), mac2_ptr, mac2_len);
            bool zero_mac2 = std::all_of(received_mac2.begin(), received_mac2.end(), [](uint8_t b) { return b == 0; });
            const auto* addr_bytes = (const uint8_t*)peer_addr;

            bool mac2_ok = !zero_mac2 && cookie_state.verify_mac2(addr_bytes, peer_addr_len, noise_msg_ptr, noise_len, received_mac2);
            if (!mac2_ok) {
                auto cookie = cookie_state.compute(addr_bytes, peer_addr_len);
                send_wire(4, cookie.data(), cookie.size(), conn_id, peer_addr, peer_addr_len);
                ++cookie_replies_sent;
                return;
            }
        }

        std::vector<uint8_t> noise_msg(noise_msg_ptr, noise_msg_ptr + noise_len);
        try {
            ++dh_attempts;
            noise::noise_ik_responder responder(local_static);
            auto result = responder.read_message1(noise_msg); // cryptographically-proven initiator identity
            auto msg2 = responder.write_message2(nullptr, 0);
            auto keys = responder.finish();

            peer_session session{transport::connection<>(std::move(keys.send), std::move(keys.recv)), std::nullopt, {}, peer_addr_len, now, now, result.initiator_static};
            std::memcpy(&session.addr, peer_addr, peer_addr_len);
            // insert_or_assign needs move-assignment, which peer_session doesn't have (reliable_sender holds
            // a connection& reference member, so optional<reliable_sender> can't be move-assigned) -
            // erase-then-emplace only needs move-construction, which peer_session does support.
            sessions.erase(conn_id);
            sessions.emplace(conn_id, std::move(session));

            send_wire(2, msg2.data(), msg2.size(), conn_id, peer_addr, peer_addr_len);
            incoming_queue.push_back(event{event::event_type::handshake_established, conn_id, {}, result.initiator_static});
        } catch (const noise::handshake_error&) {
            ++handshake_rejected;
        }
    }

    void handle_message2(uint64_t conn_id, const uint8_t* payload, size_t payload_len) {
        auto it = pending_initiators.find(conn_id);
        if (it == pending_initiators.end()) return; // stray/duplicate reply, or already established

        std::vector<uint8_t> msg2(payload, payload + payload_len);
        try {
            it->second.initiator.read_message2(msg2);
            noise::transport_keys keys = it->second.initiator.finish();

            auto now = std::chrono::steady_clock::now();
            crypto::public_key peer_static = it->second.remote_static;
            peer_session session{transport::connection<>(std::move(keys.send), std::move(keys.recv)), std::nullopt, it->second.addr, it->second.addr_len, now, now, peer_static};
            pending_initiators.erase(it);
            sessions.erase(conn_id);
            sessions.emplace(conn_id, std::move(session));
            incoming_queue.push_back(event{event::event_type::handshake_established, conn_id, {}, peer_static});
        } catch (const noise::handshake_error&) {
            pending_initiators.erase(it);
            incoming_queue.push_back(event{event::event_type::handshake_failed, conn_id, {}, {}});
        }
    }

    void handle_cookie_reply(uint64_t conn_id, const uint8_t* payload, size_t payload_len) {
        auto it = pending_initiators.find(conn_id);
        if (it == pending_initiators.end() || payload_len < anti_dos::cookie_len) return;
        it->second.started_at = std::chrono::steady_clock::now(); // the responder is there, just under load - reset the handshake-timeout clock

        anti_dos::cookie cookie_value;
        std::memcpy(cookie_value.data(), payload, anti_dos::cookie_len);
        auto mac2 = anti_dos::compute_mac2(cookie_value, it->second.msg1.data(), it->second.msg1.size());

        std::vector<uint8_t> wire(it->second.msg1.size() + anti_dos::cookie_len);
        std::memcpy(wire.data(), it->second.msg1.data(), it->second.msg1.size());
        std::memcpy(wire.data() + it->second.msg1.size(), mac2.data(), mac2.size());

        uint8_t obf_type = 1;
        uint64_t obf_conn_id = conn_id;
        crypto::public_key ephemeral;
        std::memcpy(ephemeral.data(), it->second.msg1.data(), crypto::key_len);
        xor_msg1_header(obf_type, obf_conn_id, wire.data() + wire.size() - anti_dos::cookie_len,
                        it->second.remote_static, ephemeral.data());

        send_wire(obf_type, wire.data(), wire.size(), obf_conn_id, (sockaddr*)&it->second.addr, it->second.addr_len);
    }

    void handle_data(uint64_t conn_id, const uint8_t* payload, size_t payload_len) {
        auto it = sessions.find(conn_id);
        if (it == sessions.end()) return;

        std::vector<uint8_t> out;
        uint32_t msg_id = 0;
        auto res = it->second.conn.decrypt_and_reassemble(payload, payload_len, out, msg_id);
        // Only counts as liveness evidence once authenticated - an
        // unauthenticated packet under a spoofed source address could
        // otherwise keep a session's liveness clock fed indefinitely with
        // no proof the real peer is still there.
        if (res != transport::result::auth_failed && res != transport::result::replay_rejected) {
            it->second.last_received_time = std::chrono::steady_clock::now();
        }
        switch (res) {
            case transport::result::completed:
                incoming_queue.push_back(event{event::event_type::message_received, conn_id, std::move(out), {}, msg_id});
                break;
            case transport::result::auth_failed: ++auth_failed; break;
            case transport::result::replay_rejected: ++replay_rejected; break;
            case transport::result::reassembly_rejected: ++reassembly_rejected; break;
            case transport::result::incomplete: break;
            case transport::result::duplicate_of_completed:
                // Legitimate retransmit of a message we've already delivered
                // upward (the peer's ack for it was lost) - do NOT re-push a
                // second message_received event for data the caller already got.
                // The reassembler already re-marked its slot dirty, so the next
                // tick()'s SACK drain naturally re-sends the ack this peer is
                // missing; nothing else to do here.
                break;
        }
    }

    void handle_sack(uint64_t conn_id, const uint8_t* payload, size_t payload_len) {
        auto it = sessions.find(conn_id);
        if (it == sessions.end() || !it->second.reliable.has_value() || payload_len < 1) return;
        // Weaker liveness evidence than handle_data/handle_keepalive: SACKs
        // are sent unauthenticated (see tick()'s send path), so in
        // principle a blind attacker who guesses a live conn_id could feed
        // this clock without proving anything. The blast radius of that is
        // just "a dead session's cleanup is delayed" - no data exposure,
        // nothing bypassed - so treating it as liveness evidence anyway is
        // an acceptable tradeoff here, not a silent gap.
        it->second.last_received_time = std::chrono::steady_clock::now();

        uint8_t count = payload[0];
        for (uint8_t i = 0; i < count; ++i) {
            size_t off = 1 + (size_t)i * reliability::sack_wire_len;
            if (off + reliability::sack_wire_len > payload_len) break;
            it->second.reliable->handle_ack(reliability::decode_sack(payload + off));
        }
    }

    // Authenticates an inbound keepalive and does nothing else: no event is
    // pushed to incoming_queue, and this deliberately never touches
    // conn.decrypt_and_reassemble/the reassembler, since a keepalive was
    // never fragmented in the first place. A forged/replayed/corrupt
    // keepalive is silently dropped here - decrypt_keepalive already
    // counted it against the connection's own auth-failure/replay-rejection
    // counters (conn.auth_failures()/conn.replay_rejections()), so there's
    // nothing further for this layer to do with the result.
    void handle_keepalive(uint64_t conn_id, const uint8_t* payload, size_t payload_len) {
        ++keepalives_received;
        auto it = sessions.find(conn_id);
        if (it == sessions.end()) return;
        // Only counts as liveness evidence once authenticated - same
        // reasoning as handle_data.
        if (it->second.conn.decrypt_keepalive(payload, payload_len)) {
            it->second.last_received_time = std::chrono::steady_clock::now();
        }
    }

    // Returns whether anything was actually put on the wire for conn_id -
    // tick() uses this to decide whether the session still counts as "idle"
    // for keepalive purposes (see tick()'s own comment on why a session
    // that already sent something this tick shouldn't also get a redundant
    // keepalive stacked on top).
    bool flush_outgoing_for(uint64_t conn_id, peer_session& session) {
        auto qit = outgoing_queues.find(conn_id);
        if (qit == outgoing_queues.end()) return false;
        auto& queue = qit->second;
        bool sent_anything = false;

        while (!queue.empty()) {
            // Don't even start a new queued item once the send-buffer pool is dry: send_wire()
            // would just silently drop whatever doesn't fit, and unlike a mid-message shortfall
            // (self-healing for reliable sends via the retransmit path; an accepted, pre-existing
            // small loss risk for fire-and-forget ones), starting a brand new item we *know* has no
            // room for is pointless work. Leaving it queued costs nothing - it tries again next tick.
            if (!io.can_send()) break;

            queued_send& item = queue.front();
            if (item.reliable) {
                if (!session.reliable.has_value()) {
                    session.reliable.emplace(session.conn, /*tick=*/std::chrono::milliseconds(20), /*initial_timeout=*/std::chrono::milliseconds(200),
                                              /*max_retries=*/5, config.reliable_initial_cwnd, config.reliable_initial_ssthresh);
                }
                if (!session.reliable->can_send()) break; // congestion window full; try again next tick
                auto packets = session.reliable->send_message(item.data.data(), item.data.size());
                for (auto& pkt : packets) send_wire(3, pkt.data(), pkt.size(), conn_id, (sockaddr*)&session.addr, session.addr_len);
                sent_anything |= !packets.empty();
            } else {
                auto packets = session.conn.encrypt_message(item.data.data(), item.data.size());
                for (auto& pkt : packets) send_wire(3, pkt.data(), pkt.size(), conn_id, (sockaddr*)&session.addr, session.addr_len);
                sent_anything |= !packets.empty();
            }
            queue.pop_front();
        }
        return sent_anything;
    }

    io_backend& io;
    crypto::key_pair local_static;
    endpoint_config config;
    anti_dos::cookie_secret cookie_state;
    anti_dos::load_gate load_state;

    std::unordered_map<uint64_t, peer_session> sessions;
    std::unordered_map<uint64_t, pending_initiator> pending_initiators;
    std::unordered_map<uint64_t, std::deque<queued_send>> outgoing_queues;

    std::vector<event> incoming_queue;

    uint64_t dropped_missing_send_buffer_count = 0;
    uint64_t handshake_rejected = 0;
    uint64_t auth_failed = 0;
    uint64_t replay_rejected = 0;
    uint64_t reassembly_rejected = 0;
    uint64_t dh_attempts = 0;
    uint64_t cookie_replies_sent = 0;
    uint64_t keepalives_received = 0;
};

} // namespace protocol
