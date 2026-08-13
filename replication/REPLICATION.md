# REPLICATION.md - `replication.hpp` specification

> Ships one storage's WAL to N replicas over an established
> `protocol_endpoint` session. Two classes, no threads, no timers, no policy:
> it decides *how* bytes move and *how* progress is counted, and never decides
> what "committed" means. Works against either storage engine.

## 0. What it is, and what it deliberately is not

The primary's log is append-only and immutable once written. Replication is
therefore not a protocol so much as a cursor: each replica has an offset, and
the primary sends the bytes after it. Everything else in this file exists
because that simple statement breaks in four places - the replica might be
brand new, it might have fallen out of sync, it might reconnect, and the
primary might have compacted the ground out from under it.

| | |
|---|---|
| **Is** | WAL shipping, snapshot bootstrap, desync detection in both directions, per-replica progress accounting |
| **Is not** | synchronous replication, failover, leader election, conflict resolution, a retry policy, a timer |

**There is no synchronous commit here, and that is a design decision rather
than an omission.** `create()` and `update()` stay instant and infallible on
the primary - they gain no failure mode they did not have. "Wait until my write
reached a quorum" is a loop the caller writes over the counting primitives in
§7, with its own timeout, at the call site that cares. `bench/replication_demo.cpp`
and `tests/test_replication.cpp` both show the shape, in a plain loop and in a
fiber.

**Not templated on the storage spec.** Both classes take the table as `auto&`
and never name its type, so the engine underneath is genuinely irrelevant -
the same code drives `storage.hpp` and `field_storage.hpp`, and the test suite
runs every applier scenario against both.

## 1. The two halves

```cpp
class replica_applier {                       // on each replica
    void on_message(auto& table, uint32_t msg_id, const std::vector<uint8_t>& data, auto&& send_reply);
    void on_reconnected();
    std::size_t applied_offset() const;
    void set_applied_offset(std::size_t);
    std::size_t reorder_buffer_size() const;
};

class primary_shipper {                       // on the primary
    void on_replica_connected(const crypto::public_key&, uint64_t conn_id, std::size_t resume_from);
    void on_replica_disconnected(const crypto::public_key&);
    void on_ack(const crypto::public_key&, const std::vector<uint8_t>& data);
    void tick(auto& table, auto& ep, std::size_t max_batch_bytes = 64 * 1024);
    void ship_snapshot(auto& table, auto& ep, const crypto::public_key&, std::size_t max_batch_bytes = 64 * 1024);

    std::optional<std::size_t> replicated_offset(const crypto::public_key&) const;
    std::size_t replicas_confirmed_through(std::size_t target, std::span<const crypto::public_key>) const;
    bool quorum_reached(std::size_t target, std::span<const crypto::public_key>, std::size_t required) const;
    std::vector<crypto::public_key> replicas_needing_resync(const auto& table) const;
};
```

`tick()` is called once per pass of the primary's own event loop. It reads
straight out of the storage's mapping - there is no queue between the two,
because the bytes it ships were immutable the moment they were written.

**Replicas are keyed by proven static public key, not by connection id.** A
reconnect produces a brand-new `conn_id`, and the resume point has to survive
that. `on_replica_disconnected` therefore clears only the connection and keeps
everything else.

## 2. The wire

Five kinds, one tag byte each - the same idea as `storage::tag_t`, applied to
the wire instead of to disk.

| kind | direction | payload after the tag |
|---|---|---|
| `wal_batch` | primary → replica | `uint64 from_offset`, then raw WAL bytes |
| `progress_ack` | replica → primary | `uint64 applied_offset` |
| `snapshot_batch` | primary → replica | a chunk of an exported snapshot |
| `snapshot_done` | primary → replica | `uint64 resume_offset` |
| `resync_request` | replica → primary | nothing |

Everything goes over the endpoint's **reliable** path. Losing a replication
chunk is never an acceptable best effort.

Framing is deliberately thin because the layer underneath already provides what
would otherwise have to be rebuilt here: authentication, ordering within a
connection, retransmission. `msg_id` comes from the transport, and it is what
actually orders application - the offsets are a cross-check (§4).

## 3. Bootstrap and resync are one path

A brand-new replica and a desynced replica get the *same* treatment: a full
snapshot, then incremental shipping from the offset it names. That is one code
path rather than two, which is worth stating because the alternative - a
special "catch me up from the middle" repair - is where this kind of code
usually grows its bugs.

```cpp
shipper.on_replica_connected(key, conn_id, /*resume_from=*/0);
shipper.ship_snapshot(table, ep, key);        // new replica: exactly this
```

`export_snapshot()` re-encodes every live record as a CREATE. It does **not**
disturb the primary's own log - unlike `compact()`, which rewrites it. A
snapshot is chunked into `snapshot_batch` messages and terminated by one
`snapshot_done` carrying the offset to resume from.

On the replica, `snapshot_done` calls `table.reset()` **first**. A snapshot is
only meaningful applied on top of nothing: merging it into existing state would
leave behind any record the primary no longer has, and ids collide by
construction because every storage numbers from 1.

`ship_snapshot` needs single-threaded access to the table for its duration -
`export_snapshot()` and `current_offset()` must observe the same instant.

## 4. `next_apply_offset` is the primary's number, not the replica's

The single most misreadable thing in this file.

A replica tracks "which primary offset do I expect next", and that is tracked
**independently of its own `current_offset()`**. The two are equal only in the
pure-incremental case. After a snapshot they diverge on purpose: the replica's
file now holds just the compacted live set, starting from its own offset 0,
while `next_apply_offset` jumps to wherever the primary's absolute WAL position
was when the snapshot was taken.

Nothing in either engine's `apply()` cares what absolute offset a record sits
at - lookups are by record id, never by position - so the divergence is free.

The consequence that bites: **`next_apply_offset` cannot be recovered by
`recover()`**, because it is not a property of the replica's own file. A
restarted replica that has not saved it can only be re-snapshotted.

```cpp
// persist at the same moment the replica syncs, so the two cannot disagree
table.sync();
save_somewhere(applier.applied_offset());
...
applier.set_applied_offset(load_from_somewhere());   // after restart
```

*Where* to persist it is left to the caller because it is a policy question: a
record in the replica's own storage under a tag from your central enum, a side
file, whatever fits. The engine offers `set_last_record_flags()` if you want it
in a header that was going to be written anyway.

## 5. Desync, detected from both ends

Desync is normally invisible, which is why both directions are checked.

**Replica side.** Every `wal_batch` carries the offset it starts at. If it does
not match what the replica expects, nothing is applied and a `resync_request`
goes back. The usual cause is that the primary compacted and renumbered.

**Primary side**, and this is the one that would otherwise be silent. A replica
whose tracked position is *strictly ahead* of the primary's own
`current_offset()` is impossible - it cannot hold more than the primary ever
wrote. It is the signature of a compaction having rewritten offsets underneath
it, or of a stale saved checkpoint from before one.

```cpp
for (const auto& key : shipper.replicas_needing_resync(table))
    shipper.ship_snapshot(table, ep, key);
```

Without that check, `tick()` ships only what is newer than a replica's
position - so a replica stuck "ahead" is simply never sent anything again,
forever, with no error anywhere.

Detection is mechanism; deciding to resnapshot, and how often to retry it for a
replica that keeps flapping, is policy and stays with the caller.

## 6. Reconnect

Call `on_reconnected()` on every `handshake_established` for that connection,
including the first, where it is a harmless no-op.

A reconnect builds a fresh transport connection whose `send_msg_id` restarts at
0. An applier still expecting msg_id 4 from the dead session would park every
message of the new one in its reorder buffer forever, waiting for ids that will
never come again. So `on_reconnected()` drops the buffer and resets the
numbering - but deliberately **not** `next_apply_offset`, because the WAL
position stays valid across a reconnect. Only the transport's numbering starts
over.

There is a matching trap on the primary. It ships a new replica's snapshot the
instant its own handshake completes, so those packets can land in the very same
`poll_incoming()` batch as the replica's own `handshake_established` event. A
handshake loop that watches only for that event and ignores everything else
silently drops them, and the replica then waits forever for ids that already
came and went. **Route replication traffic from the first poll after
`connect()`**, not from "once the handshake loop is done."

## 7. Counting progress, and why there is no `wait_for_quorum()`

```cpp
std::optional<std::size_t> replicated_offset(key);
std::size_t replicas_confirmed_through(target, keys);
bool quorum_reached(target, keys, required);
```

These are counting primitives. Majority, all, one named replica, weighted by
datacenter - every one of those is a policy built from the same count, and this
file refuses to pick. Unknown keys simply do not count.

A caller's synchronous commit is then an explicit, timeout-able loop:

```cpp
table.sync();                                    // durable locally first
const std::size_t target = table.current_offset();
while (!shipper.quorum_reached(target, keys, 2)) {
    if (timed_out()) return commit_result::not_replicated;
    pump_events();                               // or fiber::yield()
}
```

Keeping this at the call site is what keeps the storage's write API free of a
failure mode it otherwise does not have, and keeps the decision of *when* to
pay for durability where the decision belongs.

**`durability::replicated` is advisory.** It records the intent that a
storage's durability is expected to come from replicas; nothing enforces that a
caller actually waits. A tag cannot.

### The ack means "applied", not "durable"

`progress_ack` is sent when the replica has applied the bytes - which by
default means they are in its mapping, not on its disk. If a quorum is supposed
to mean the data survives a replica losing power, the replica must `sync()`
*before* the ack goes out. That is wired in the caller's `send_reply` callback,
not here:

```cpp
applier.on_message(table, msg_id, data, [&](const std::vector<uint8_t>& reply) {
    table.sync();                                // ack only after it is durable
    ep.send_message(conn_id, reply, /*reliable=*/true);
});
```

`tests/test_replication.cpp::test_sync_before_ack` pins this down.

## 8. Limits

| | |
|---|---|
| writers | one primary, as everywhere else in this project |
| failover | not here - promoting a replica is the caller's, and nothing in this file elects anything |
| conflict resolution | none, and none needed: one writer means no conflicts |
| `msg_id` wraparound | out of scope - four billion messages on one connection, and a reconnect resets the numbering long before |
| reorder buffer | bounded in practice, not by construction: a message at or below `next_msg_id` is dropped rather than parked, so retransmission cannot grow it, but a genuine permanent gap will hold everything after it |
| snapshot cost | `export_snapshot()` builds the whole thing in memory before sending |

A `reorder_buffer_size()` that only grows means something upstream is stuck. It
is worth exporting to your metrics for exactly that reason.

## 9. Best practices

**Ship a snapshot immediately after registering a new replica.** `resume_from
== 0` and no snapshot means the first `wal_batch` mismatches and you spend a
round trip discovering what you already knew.

**Call `replicas_needing_resync()` on the same tick as `tick()`.** It is the
only thing standing between you and a replica that silently stops receiving.

**Persist `applied_offset()` in the same operation that syncs the replica.** If
they can disagree after a crash, they eventually will.

**Route replication traffic from the first poll after connect.** See §6 - this
one has already cost real debugging time.

**Decide what an ack means before you rely on a quorum.** Applied and durable
are different claims; §7 shows how to make it the second.

**Do not put the sync-before-ack inside a hot loop without measuring it.** It
turns every batch into a durability barrier, and `FIELD_STORAGE.md` §8 applies
in full: a barrier spaced tighter than it costs spends all its time being the
previous one.

## 10. What is verified

`tests/test_replication.cpp` - 71 checks, run in Release and under ASan/UBSan
by `ctest`. Every applier scenario runs against **both** engines:

| | |
|---|---|
| out-of-order delivery | messages 2 and 1 arrive before 0; nothing applies until 0 does, then all three drain in order |
| duplicates | a repeated `msg_id` neither re-applies nor accumulates in the buffer |
| permanent gap | five messages behind a missing head stay buffered and nothing applies |
| desync | a `from_offset` mismatch emits `resync_request` and applies nothing |
| snapshot | replaces stale state rather than merging - checked with an id the snapshot does not contain |
| split snapshot | survives being cut across two messages |
| checkpoint restore | a restarted applier with a restored offset resumes incrementally, no resnapshot |

Networked scenarios, over real endpoint pairs on loopback: disconnect with
writes accumulating then reconnect-and-resume, sync-before-ack, replica restart
from its own recovered file, quorum reached, quorum timing out, and the same
wait driven from a fiber.

`bench/replication_demo.cpp` runs the whole thing end to end in six phases -
snapshot bootstrap, incremental catch-up, multi-replica fan-out, and both
directions of desync self-healing.

**No throughput numbers exist yet, and this section will not invent any.** What
would need measuring: catch-up rate against `max_batch_bytes`, the cost of
sync-before-ack at a realistic barrier spacing, and how snapshot bootstrap time
scales with live-set size. Until those are run, treat replication as verified
for correctness and unmeasured for performance.
