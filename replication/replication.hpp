#pragma once
// replication.hpp - ships a storage<spec>'s WAL to a replica over
// protocol::protocol_endpoint, reusing the WAL's own on-disk record format
// (record_header + payload, exactly as storage::storage<spec>::apply_wal_range
// expects) as the wire payload directly. No second schema to keep in sync
// with the first - the primary reads raw bytes straight out of its own
// mmap'd region (raw_base()/current_offset()) and the replica applies them
// through the exact same checksum-verify-then-apply loop storage.hpp's own
// recover() already uses, just fed by network arrival instead of a single
// startup scan.
//
// NOTE ON THE INCLUDE PATH BELOW: storage.hpp currently lives in a
// separate, unrelated directory tree - there is no shared repo between the
// protocol and storage projects yet. This reaches it via a relative path
// as a pragmatic placeholder so this file is buildable today; revisit once
// the repo-layout question (merge the two, or a shared common/ directory)
// is actually settled.
//
// Ordering: reassembly already resolves within-message fragment
// reordering; it says nothing about whether message 5 might complete
// before message 4 does. Replication is inherently order-sensitive (an
// UPDATE cannot be applied before the CREATE it targets exists), so
// replica_applier below buffers anything that arrives ahead of the next
// expected msg_id (protocol::event::msg_id) and only applies once messages
// are being processed strictly in order. This solves *in-session*
// reordering only - msg_id resets to 0 on every reconnect (a fresh
// connection<> object), so it says nothing about resuming after a drop;
// see primary_shipper's own resume-by-identity handling for that.
//
// Two ways a replica gets (re)synchronized:
//   - Incremental (wal_batch): the common case, raw WAL bytes appended
//     since the last shipped offset.
//   - Snapshot (snapshot_batch + snapshot_done): every live record
//     freshly re-encoded as a CREATE (storage::export_snapshot(), which
//     does NOT disrupt the primary's own ongoing WAL - unlike compact()),
//     sent when a replica is brand new (resume_from == 0) OR when
//     replica_applier detects a from_offset mismatch on an incremental
//     batch (desync - most likely the primary compacted and rewrote its
//     offsets in between) and reports it via resync_request. New-replica
//     bootstrap and post-desync recovery are deliberately the same code
//     path, not two.
//
//   next_apply_offset (replica_applier's bookkeeping for "which primary
//   offset do I expect next") is tracked independently of the replica's
//   own storage<spec>::current_offset() - they only happen to be equal in
//   the pure-incremental case. After a snapshot, the replica's own file is
//   small (just the compacted live set, starting from its own offset 0)
//   while next_apply_offset jumps to wherever the primary's absolute WAL
//   position was at snapshot time - nothing in storage.hpp's apply()/
//   apply_wal_range() cares what absolute offset a record sits at (lookups
//   are by record id, never by position), so this divergence is
//   deliberate, not a gap to paper over.
//
// Mechanism, not policy: this file exposes how far each replica has
// confirmed applying (replicated_offset) and how many of a given set have
// reached a target (replicas_confirmed_through / quorum_reached) -
// deciding what counts as "committed" is the caller's call. Concretely,
// synchronous replication is NOT a mode this file implements: local
// create()/update() stay instant and infallible exactly as they are, and
// "wait until my write reached a quorum" is a separate, explicit,
// timeout-able loop the caller writes on top of the counting primitives
// above (see replication_tests.cpp for both a plain-loop and a
// fiber-based version). That split is deliberate - it keeps storage's
// write API free of a failure mode it otherwise doesn't have, and keeps
// the choice of *when* to pay for durability at the call site instead of
// baked into a storage tier.
//
// One honest consequence: durability::replicated is advisory. It records
// intent ("this storage's durability is expected to come from replicas"),
// but nothing here enforces that a caller actually waits - the tag can't,
// since the waiting lives at the call site by design.
//
// Whether a replica syncs its own copy before acking is likewise the
// caller's: send_reply is invoked right after apply and before the ack
// goes out, so a table.sync() there makes the ack mean "durable on my
// disk," not just "in my memory" (see test_sync_before_ack). Note that
// the storage durability tier alone does NOT do this - local_sync only
// means the storage owns its own file, sync() is always an explicit call.
//
// And resync_request is a signal, not an auto-retry policy: how often to
// react to repeated requests (rate-limiting a flapping replica, say) is
// left to the caller - this file only ever emits the signal once per
// detected mismatch.

#include "../storage/storage.hpp"
#include "../net/protocol.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace replication {

    // Wire framing for messages this layer sends over an already-
    // established protocol_endpoint session - a tiny tag-prefixed
    // envelope, the same idea as storage::tag_t discriminating a stored
    // record's type, just applied to what's sent over the wire instead of
    // what's on disk.
    enum class msg_kind : uint8_t { wal_batch = 1, progress_ack = 2, snapshot_batch = 3, snapshot_done = 4, resync_request = 5 };

    // =====================================================================
    // Replica side: applies inbound replication traffic for one storage
    // instance/connection pair.
    // =====================================================================
    template <typename spec>
    class replica_applier {
    public:
        // Feed every message_received event belonging to the replication
        // connection through here (filtering replication traffic from
        // anything else on the same endpoint is the caller's business
        // logic, not this layer's concern). `send_reply` is called with a
        // ready-to-send payload (a progress_ack after a wal_batch/
        // snapshot_done, or a resync_request on detected desync) - pass it
        // straight to endpoint.send_message(conn_id, payload,
        // /*reliable=*/true).
        void on_message(auto& table, uint32_t msg_id, const std::vector<uint8_t>& data, auto&& send_reply) {
            // Already applied: a retransmit of old news. Dropping it here
            // rather than parking it in `pending` is what keeps the buffer
            // from growing without bound under a peer that retransmits a
            // lot - nothing downstream ever looks below next_msg_id again,
            // so a buffered copy would simply sit there forever.
            // (Wraparound of a uint32 msg_id is out of scope: that's 4
            // billion messages on one connection, and a reconnect resets
            // the numbering long before then - see on_reconnected.)
            if (msg_id < next_msg_id) return;
            pending[msg_id] = data;
            // Drain everything now available in order starting from
            // next_msg_id - covers both "this was the one we were waiting
            // for" and "this filled a gap that unblocks several already-
            // buffered messages at once."
            while (true) {
                auto it = pending.find(next_msg_id);
                if (it == pending.end()) break;
                apply_one(table, it->second, send_reply);
                pending.erase(it);
                ++next_msg_id;
            }
        }

        // Call when this replica's underlying connection was
        // re-established (i.e. on every handshake_established for it,
        // including the first, where it's a harmless no-op).
        //
        // A reconnect builds a fresh transport::connection, whose
        // send_msg_id restarts at 0 - so an applier still expecting msg_id
        // 4 from the previous session would park every message of the new
        // one in the reorder buffer forever, waiting for ids that will
        // never come again. next_apply_offset is deliberately NOT reset:
        // the WAL position stays valid across a reconnect, it's only the
        // transport's message numbering that starts over. Anything still
        // buffered belonged to the dead connection's numbering and can
        // only be misleading now, so it's dropped - the reliable transport
        // redelivers whatever actually mattered on the new connection.
        void on_reconnected() {
            pending.clear();
            next_msg_id = 0;
        }

        std::size_t applied_offset() const { return next_apply_offset; }

        // Restores the replication checkpoint after a replica restart.
        //
        // next_apply_offset is the primary's absolute WAL position, which
        // deliberately has no relationship to this replica's own file
        // layout (see the file header) - so it cannot be recovered from
        // recover() along with everything else, and a restarted replica
        // that doesn't restore it can only be resnapshotted. Persisting it
        // is left to the caller precisely because *where* to put it is a
        // policy question this file shouldn't answer: a record in the
        // replica's own storage under a tag from the project's central
        // enum, a side file, whatever fits. Pair it with applied_offset()
        // and persist at the same moment the replica syncs, so the two can
        // never disagree after a crash.
        void set_applied_offset(std::size_t offset) { next_apply_offset = offset; }

        // How many messages are buffered waiting for an earlier msg_id to
        // arrive - should stay small/transient in practice; a value that
        // only grows is a sign something upstream is stuck, not this
        // layer's own bug.
        std::size_t reorder_buffer_size() const { return pending.size(); }

    private:
        void apply_one(auto& table, const std::vector<uint8_t>& data, auto&& send_reply) {
            if (data.empty()) return;
            switch ((msg_kind)data[0]) {
                case msg_kind::wal_batch: apply_wal_batch(table, data, send_reply); break;
                case msg_kind::snapshot_batch: apply_snapshot_batch(data); break;
                case msg_kind::snapshot_done: apply_snapshot_done(table, data, send_reply); break;
                default: break; // progress_ack/resync_request are primary-side messages; ignore if misrouted here
            }
        }

        void apply_wal_batch(auto& table, const std::vector<uint8_t>& data, auto&& send_reply) {
            if (data.size() < 1 + sizeof(uint64_t)) return;

            uint64_t from_offset;
            std::memcpy(&from_offset, data.data() + 1, sizeof(from_offset));
            const std::byte* wal_bytes = (const std::byte*)(data.data() + 1 + sizeof(from_offset));
            std::size_t wal_len = data.size() - 1 - sizeof(from_offset);

            // Sanity check, not the sole ordering guarantee (msg_id order
            // is what actually guarantees this in practice) - a mismatch
            // means desync, most likely the primary compacted and rewrote
            // its offsets since this replica last heard from it. Report it
            // rather than silently dropping: the primary can then ship a
            // fresh snapshot, the one recovery path both new-replica
            // bootstrap and this use.
            if (from_offset != next_apply_offset) {
                send_reply(make_resync_request());
                return;
            }

            std::size_t applied = table.apply_wal_range(wal_bytes, wal_len);
            next_apply_offset += applied;
            send_reply(make_progress_ack());
        }

        void apply_snapshot_batch(const std::vector<uint8_t>& data) {
            if (data.size() < 1) return;
            incoming_snapshot.insert(incoming_snapshot.end(), data.begin() + 1, data.end());
        }

        void apply_snapshot_done(auto& table, const std::vector<uint8_t>& data, auto&& send_reply) {
            if (data.size() < 1 + sizeof(uint64_t)) return;
            uint64_t resume_offset;
            std::memcpy(&resume_offset, data.data() + 1, sizeof(resume_offset));

            table.reset(); // discard whatever this replica had - a snapshot is only meaningful applied on top of nothing
            if (!incoming_snapshot.empty()) {
                table.apply_wal_range((const std::byte*)(incoming_snapshot.data()), incoming_snapshot.size());
            }
            incoming_snapshot.clear();

            // Deliberately NOT table.current_offset() - see the file
            // header on why next_apply_offset tracks the primary's
            // absolute WAL position independently of however many bytes
            // this replica's own freshly-compacted file happens to
            // contain.
            next_apply_offset = resume_offset;
            send_reply(make_progress_ack());
        }

        std::vector<uint8_t> make_progress_ack() const {
            std::vector<uint8_t> ack(1 + sizeof(uint64_t));
            ack[0] = (uint8_t)msg_kind::progress_ack;
            uint64_t applied_offset_value = next_apply_offset;
            std::memcpy(ack.data() + 1, &applied_offset_value, sizeof(applied_offset_value));
            return ack;
        }

        std::vector<uint8_t> make_resync_request() const {
            return {(uint8_t)msg_kind::resync_request};
        }

        std::unordered_map<uint32_t, std::vector<uint8_t>> pending;
        std::vector<uint8_t> incoming_snapshot; // accumulates across possibly-multiple snapshot_batch messages until snapshot_done
        uint32_t next_msg_id = 0;
        std::size_t next_apply_offset = 0;
    };

    // =====================================================================
    // Primary side: tracks, per known replica (keyed by its proven static
    // public key - see protocol_endpoint's own peer_static-vs-conn_id
    // reasoning, the same idea applies here so a reconnect, which gets a
    // brand-new conn_id, still finds the right resume point), how far it
    // has confirmed applying, and ships whatever's new since then.
    // =====================================================================
    template <typename spec>
    class primary_shipper {
    public:
        // Registers (or re-recognizes, on reconnect) a replica.
        // resume_from is normally 0 for a brand-new replica (it needs
        // everything - the caller should follow this with one
        // ship_snapshot() call) or whatever offset replicated_offset()
        // last reported for a reconnecting one, read before its old
        // session died.
        void on_replica_connected(const crypto::public_key& replica_key, uint64_t conn_id, std::size_t resume_from) {
            auto it = replicas.find(replica_key);
            if (it != replicas.end()) {
                it->second.conn_id = conn_id;
                it->second.shipped_offset = resume_from;
            } else {
                replicas.emplace(replica_key, replica_state{conn_id, resume_from, 0});
            }
        }

        // Deliberately does NOT erase the replica's resume state - it must
        // survive past the session's own death (see protocol.hpp's own
        // session-lifetime-vs-resume-state-lifetime distinction). Only
        // conn_id is invalidated; a later on_replica_connected() with the
        // same key picks the state back up.
        void on_replica_disconnected(const crypto::public_key& replica_key) {
            auto it = replicas.find(replica_key);
            if (it != replicas.end()) it->second.conn_id = std::nullopt;
        }

        // Feed a progress_ack payload through here (same filtering
        // responsibility as replica_applier::on_message).
        void on_ack(const crypto::public_key& replica_key, const std::vector<uint8_t>& data) {
            if (data.empty() || (msg_kind)data[0] != msg_kind::progress_ack || data.size() < 1 + sizeof(uint64_t)) return;
            auto it = replicas.find(replica_key);
            if (it == replicas.end()) return;
            uint64_t applied_offset_value;
            std::memcpy(&applied_offset_value, data.data() + 1, sizeof(applied_offset_value));
            it->second.confirmed_offset = std::max(it->second.confirmed_offset, (std::size_t)applied_offset_value);
        }

        // Primitive, not policy: how far a specific replica has confirmed
        // applying, or nullopt if replica_key is unknown. A caller
        // building "wait for majority"/"wait for all"/"wait for this one
        // specific replica" logic reads this across whichever replicas it
        // cares about and decides for itself what counts as committed -
        // this file doesn't decide that.
        std::optional<std::size_t> replicated_offset(const crypto::public_key& replica_key) const {
            auto it = replicas.find(replica_key);
            if (it == replicas.end()) return std::nullopt;
            return it->second.confirmed_offset;
        }

        // How many of `keys` have confirmed applying through
        // target_offset. The counting primitive every "is this write
        // committed?" policy is built from - majority, all, a specific
        // named replica, weighted by datacenter, whatever. Unknown keys
        // simply don't count.
        std::size_t replicas_confirmed_through(std::size_t target_offset,
                                               std::span<const crypto::public_key> keys) const {
            std::size_t n = 0;
            for (const auto& key : keys) {
                auto it = replicas.find(key);
                if (it != replicas.end() && it->second.confirmed_offset >= target_offset) ++n;
            }
            return n;
        }

        // Convenience over the above for the common "at least N of these"
        // shape. Still not policy: the caller picks the set and the
        // threshold - this file never decides that a majority is what
        // matters, or which replicas are eligible to vote.
        bool quorum_reached(std::size_t target_offset, std::span<const crypto::public_key> keys,
                            std::size_t required) const {
            return replicas_confirmed_through(target_offset, keys) >= required;
        }

        // Replicas whose tracked position is impossible: strictly ahead of
        // the primary's own current offset. A replica can't legitimately
        // have more than the primary ever wrote, so this is the tell-tale
        // signature of the primary having compacted and rewritten its
        // offsets out from under them (see storage::compact - it restarts
        // numbering from 0 in a fresh, smaller region), or of a stale saved
        // checkpoint from before such a compaction.
        //
        // This matters because it is otherwise SILENT: tick() below ships
        // only what's newer than a replica's position, so a replica stuck
        // "ahead" of the primary would simply never be shipped anything
        // again, forever, with no error anywhere. Detected here, acted on
        // by the caller (ship_snapshot() to each) - the detection is
        // mechanism, the decision to resnapshot, and how often to retry it
        // for a flapping replica, stays policy.
        std::vector<crypto::public_key> replicas_needing_resync(const auto& table) const {
            std::vector<crypto::public_key> out;
            std::size_t current = table.current_offset();
            for (const auto& [replica_key, state] : replicas) {
                if (state.conn_id.has_value() && state.shipped_offset > current) out.push_back(replica_key);
            }
            return out;
        }

        // Ships a full, non-disruptive snapshot to one replica - call this
        // once right after on_replica_connected() for a brand-new replica
        // (resume_from == 0), and again whenever a resync_request arrives
        // from that replica (see replica_applier's desync detection).
        // Chunks the exported snapshot into snapshot_batch messages the
        // same way tick() chunks WAL bytes into wal_batch ones, followed by
        // one snapshot_done message carrying the exact primary-side offset
        // subsequent wal_batch shipping should resume from.
        //
        // Assumes single-threaded access to `table` for the duration of
        // this call (export_snapshot() + current_offset() must observe a
        // consistent snapshot together) - a primary writing from a second
        // thread concurrently needs the same shared-mutex contract
        // background_compact() already documents in storage.hpp.
        // `ep` is `auto&`, not protocol_endpoint<>&: the endpoint's
        // keepalive/liveness/handshake timeouts are template parameters, so
        // hardcoding the default instantiation here would lock replication
        // to endpoints that happen to use the default timeouts - which is
        // exactly wrong for a test or a deployment that tunes them.
        void ship_snapshot(auto& table, auto& ep,
                           const crypto::public_key& replica_key, std::size_t max_batch_bytes = 64 * 1024) {
            auto it = replicas.find(replica_key);
            if (it == replicas.end() || !it->second.conn_id.has_value()) return;
            uint64_t conn_id = *it->second.conn_id;

            auto snapshot = table.export_snapshot();
            uint64_t resume_offset = table.current_offset(); // captured right after export - see the file header's single-threaded-access note

            for (std::size_t off = 0; off < snapshot.size(); off += max_batch_bytes) {
                std::size_t chunk_len = std::min(max_batch_bytes, snapshot.size() - off);
                std::vector<uint8_t> payload(1 + chunk_len);
                payload[0] = (uint8_t)msg_kind::snapshot_batch;
                std::memcpy(payload.data() + 1, snapshot.data() + off, chunk_len);
                ep.send_message(conn_id, std::move(payload), /*reliable=*/true);
            }

            std::vector<uint8_t> done(1 + sizeof(uint64_t));
            done[0] = (uint8_t)msg_kind::snapshot_done;
            std::memcpy(done.data() + 1, &resume_offset, sizeof(resume_offset));
            ep.send_message(conn_id, std::move(done), /*reliable=*/true);

            // Subsequent tick() calls ship incremental wal_batch bytes
            // starting from here - correct regardless of order relative to
            // the snapshot messages above, since everything on one
            // connection shares one monotonic msg_id sequence and the
            // replica's reorder buffer applies strictly in that order.
            it->second.shipped_offset = resume_offset;
        }

        // Call once per tick from the primary's own event loop: ships
        // whatever's newly written since each connected replica's last
        // shipped offset, reading directly from the storage's own mmap'd
        // region - no queue between storage and this, same reasoning as
        // background_compact's snapshot/replay (see storage.hpp), just a
        // different consumer of the same already-written, immutable-once-
        // written bytes. Uses the reliable path: losing a replication
        // chunk is never an acceptable "best effort," it must retransmit.
        void tick(auto& table, auto& ep, std::size_t max_batch_bytes = 64 * 1024) {
            std::size_t current = table.current_offset();
            for (auto& [replica_key, state] : replicas) {
                if (!state.conn_id.has_value()) continue; // disconnected - nothing to ship until it reconnects
                if (state.shipped_offset >= current) continue; // caught up

                std::size_t from = state.shipped_offset;
                std::size_t to = std::min(current, from + max_batch_bytes);

                std::vector<uint8_t> payload(1 + sizeof(uint64_t) + (to - from));
                payload[0] = (uint8_t)msg_kind::wal_batch;
                uint64_t from64 = from;
                std::memcpy(payload.data() + 1, &from64, sizeof(from64));
                std::memcpy(payload.data() + 1 + sizeof(from64), table.raw_base() + from, to - from);

                ep.send_message(*state.conn_id, std::move(payload), /*reliable=*/true);
                state.shipped_offset = to;
            }
        }

    private:
        struct replica_state {
            std::optional<uint64_t> conn_id;
            std::size_t shipped_offset = 0;
            std::size_t confirmed_offset = 0;
        };
        // std::map, not unordered_map: crypto::public_key is
        // std::array<uint8_t,32>, which already has a working operator< to
        // key off (lexicographic, since uint8_t supports <) but no
        // portable std::hash specialization to rely on - this sidesteps
        // needing to write one.
        std::map<crypto::public_key, replica_state> replicas;
    };

} // namespace replication
