# FIELD_STORAGE.md - `field_storage.hpp` specification

> One append-only log over mmap, indexed per **field** rather than per record.
> Nothing is ever patched in place. Single-writer; **concurrent readers are
> safe while no writer runs** (§12); no MVCC. Owns its log and its index; owns
> no threads; blocks only where documented below.

## 0. What it is, and when to choose it

A sibling of `storage.hpp`, not a mode of it. They share the record format, the
checksums, the concepts and the tag discipline - a log written by one is
readable by the other's scanner - but they differ in the one thing that cannot
be a runtime flag: **what an index entry means, and therefore what a read
returns.**

| | `storage.hpp` | `field_storage.hpp` |
|---|---|---|
| Index entry | `{ offset }` → the record's bytes | `{ base, chain, len, tag }` → the base CREATE plus a chain of overridden fields |
| An UPDATE | memcpy into the materialization | push an override node |
| Regions | two: log + materialization | one: the log |
| `get<T>()` | a pointer into the mapping | a copy into the caller's object |
| Disk footprint | 2× logical data | 1× logical data |

**Choose the field engine when writes matter.** An update appends the changed
field and moves a pointer; it touches no page of the record, so there is
nothing to fault on. Measured against `storage.hpp` on identical workloads:

| | in RAM | out of RAM |
|---|---|---|
| random update | **254×** faster | **56×** faster |
| major faults per update | 49 vs 49 | **41 vs 7,986** |

**Choose `storage.hpp` when reads dominate and the data fits in memory.** Its
record is contiguous, so a read is a pointer: 6,550,781 ops/sec against this
engine's 1,541,045. That advantage disappears entirely once the data stops
fitting - out of RAM the two converge (4,747 vs 5,731), because every read is a
fault either way.

**Choose `cached_storage.hpp` if you want both**, sized by one number. It is
this engine plus a bounded materialization that is a *cache* rather than a
home; `cache_bytes == 0` degenerates to this engine byte for byte.

## 1. Declaring one

```cpp
using spec = storage::field_spec<
    "data.log",          // path            the log
    64ull << 30,         // capacity        posix_fallocate'd at open
    "data.idx",          // index_path      slots + node arena, mapped
    32'000'000,          // max_ids         one 16-byte slot per id EVER issued
    8'000'000,           // max_overrides   24-byte nodes, shared pool
    8,                   // max_depth       collapse threshold          (default 8)
    4096,                // max_record_bytes                            (default 4096)
    "data.ckp",          // checkpoint_path enables restart()           (default "": off)
    0                    // max_span        collapse by log spread      (default 0: off)
>;

storage::field_storage<spec> table;
if (!table.ok()) { /* mapping failed */ }
```

`index_path` is mandatory - a field storage keeps its index in a file by
construction. Index size is fixed at open:

```
index_bytes = header + max_ids × 16 + max_overrides × 24
```

Compile-time bounds, all asserted:

| | limit | why |
|---|---|---|
| `max_depth` | < 255 | depth is 8 bits of `field_slot::chain` |
| `max_overrides` | < 16,777,216 | a node index is 24 bits of the same word |
| `max_record_bytes` | ≤ 65,535 | `field_slot::len` is 16 bits |

`posix_fallocate`, not `ftruncate`: a sparse file gives **SIGBUS** on ENOSPC in
the middle of a write, which is not a failure mode you can handle.

## 2. The log

One file, `mmap` `MAP_SHARED`. File-backed rather than anonymous on purpose:
clean pages are evictable, anonymous pages can only be swapped.

### `record_header` - 24 bytes

```
id        8   offset  0    id == 0 marks a sparse tail; depends on offset 0
offset    4          8     where inside the record the diff belongs
len       4         12
op        1         16     create | update | destroy
flags     1         17     eight bits belonging to the layer above
tag       2         18
checksum  4         20     CRC32C over header + payload
```

Payload follows; the whole thing is padded to 8 bytes.

`flags` is the engine writing zeroes and never reading them. A layer that needs
to mark a boundary - the end of a transaction, say - sets a bit in a header that
was going to be written anyway, rather than spending a record on it. See
`set_last_record_flags()`.

## 3. The index

### `field_slot` - 16 bytes per id

```cpp
uint64_t base;    // log offset of the CREATE's payload; 0 = no record
uint32_t chain;   // 24 bits: first override node, or no_override
                  //  8 bits: depth
uint16_t len;     // the record's length
tag_t    tag;
```

Three decisions here, each paid for by a measurement.

**`len` and `tag` live in the slot, not in the log.** `record_len()` used to be
`header_of(base + slots[id].base)->len` - a dereference into the log *mapping*,
so merely learning how many bytes to read cost a page touch per record. Measured
on 512 MB under a 320 MB cap, that made all six read paths identical to within
30% and left `O_DIRECT` taking 7,272 major faults per 10,000 reads while reading
its data into an aligned pool it had specifically to avoid the page cache.

**`head` and `depth` share one word.** The index is the one structure that has
to stay resident: at 27M records, 16 bytes is 432 MB against 24 bytes' 648 MB,
and that difference decides whether it survives memory pressure. When it did not
survive, every read path collapsed to ~1,000 ops/sec and became
indistinguishable, because all of them need the slot before they can do
anything at all.

**Those 16 bytes came from a feature that was measured and removed** - keeping
the newest override in the slot gave 9.2M upd/s against the baseline's 12.5M for
the same 4.08M reads.

### `override_node` - 24 bytes

```cpp
uint64_t log_offset;    // where the new bytes are
uint32_t field_offset;  // where they belong inside the record
uint32_t len;
uint32_t next;
```

An update to a field that is **already overridden replaces its node** rather
than adding one. A chain is therefore bounded by the number of *distinct fields
touched*, never by the update count.

## 4. Core operations

```cpp
template <tagged_aggregate T> record_id create(const T&);
template <tagged_aggregate T> record_id recreate(record_id, const T&);
record_id create_raw(record_id, tag_t, std::span<const std::byte>);
[[nodiscard]] record_id reserve_id() noexcept;

template <auto member> void update(record_id, member_value_t<member>);
void patch(record_id, uint32_t offset, std::span<const std::byte>);
void destroy(record_id);
```

`update<&T::field>` takes the offset from a pointer-to-member **at compile
time**. Changing eight bytes of a kilobyte record writes eight bytes - this is
the entire point of the design.

`patch` is its run-time counterpart, for a caller holding bytes without naming a
type. The offset is data there, so it takes the checked path: a diff that would
reach past the end of the record is refused, not applied.

`reserve_id()` claims an id without writing, for a caller that must name a
record before it is ready to append it - a staged transaction assigning ids to
records that later staged records refer to.

## 5. Reading - and why there is no pointer

```cpp
template <tagged_aggregate T> std::optional<T> get(record_id) const;
template <tagged_aggregate T> bool get(record_id, T& out) const;
bool read_raw(record_id, std::span<std::byte> out) const;

tag_t tag_of(record_id) const;
bool alive(record_id) const;
uint32_t depth(record_id) const;
```

`assemble()` memcpys the base record, then walks the chain applying each
overridden field on top. Because a field has at most one node, order within the
chain does not matter.

**There is no address at which the current record exists.** That is not an
omission, it is the design: the base is wherever it was created and each
override is wherever the log happened to be when it was written. A caller
written against `storage.hpp`'s pointer-returning `get()` compiles here only if
it says `auto`, never `const T*`:

```cpp
auto v = table.get<customer>(id);   // std::optional<customer> here, const customer* there
if (v) use(v->balance);             // same syntax either way
```

Iteration:

```cpp
template <tagged_aggregate T> void for_each(auto&& fn) const;  // fn(record_id, const T&)
void for_each_raw(auto&& fn) const;                            // fn(raw_view{id, tag, bytes})
```

## 6. Depth - four ways to bound a gather

Without a bound, a chain grows with the number of distinct fields a record has
ever had touched, and a gather grows with it.

| | trigger | use |
|---|---|---|
| `max_depth` | automatic, on write | the default safety valve; guarantees a gather is at most `1 + max_depth` accesses |
| `collapse_if_deeper_than(id, n)` | on read | a write-only record never pays to become contiguous; a hot one pays once |
| `collapse_deep(n)` | batch, sorted by address | maintenance tick or background thread; neither reads nor writes pay on the critical path |
| `max_span` | spread in the log exceeds N bytes | depth is a proxy; spread is the thing itself - two overrides in one page cost one fault, two 20 GB apart cost two |

`collapse(id)` rewrites the record as one contiguous CREATE and frees its chain.
`compact_chain(id)` re-appends the overrides back to back without collapsing:
depth is unchanged, but the gather now reads one place instead of *k* scattered
ones, and it costs the changed bytes rather than the whole record.

Measured trade at depth 1 versus unbounded: reads 11.2M vs 3.8M, updates 2.2M vs
12.5M, log growth +1074 MB vs +64 MB. **The default of 8 is a starting point,
not an answer** - pick it from your read/write ratio.

## 7. The single application point

```cpp
template <provenance from = provenance::external>
void apply(record_header, const std::byte*);          // runtime dispatch

template <record_op op>
void apply_local(record_header, const std::byte*);    // compile-time
```

Every path into the storage - a live write, a replayed record, a replicated one
- ends in exactly one of three op bodies. An observer hangs there, which is what
keeps a user-space secondary index from drifting from the data: it is maintained
by the same call that recovery replays.

**Two entry points, one implementation per op**, for a measured reason. Bytes
off a disk or a wire carry their op in the header and must dispatch at run time;
a record this process just wrote does not, and saying so in the type is what
lets the switch disappear. Left at run time it cost clang 1.27× on the update
path against gcc, purely from a function it declined to inline because it
carried all three cases.

`provenance` is a compile-time trust level. An external update is checked
against the record's bounds; a local one is not, because its offset came from a
pointer-to-member and is checked by the type system.

### The observer contract

An observer sees `on_record(table, header, payload)` for every applied record.
Two rules follow from the fact that recovery replays it:

- **Index maintenance belongs in the observer.** It lives only in memory and
  must be rebuilt identically every time.
- **Constraints and triggers do not.** A trigger that decremented stock when an
  order shipped must not decrement it again while the log is replayed at
  startup - the decrement is already a record in that same log. Those belong in
  a transaction layer above.

## 8. Durability

```cpp
void sync() noexcept;             // msync the log's dirty tail
std::size_t current_offset() const noexcept;
bool set_last_record_flags(uint8_t bits) noexcept;
void release_log(std::size_t upto) noexcept;
```

`sync()` msyncs from the last synced point to the write cursor, never the whole
file. When it returns, every record appended so far is durable.

### The barrier must be spaced by more than it costs

A filesystem commit is **per-inode, not per-range**: `msync` honours its range
for writeback, but the commit it triggers underneath drags every dirty page of
that inode into the transaction. This is why the log is the only thing this
engine syncs.

Choosing how often to sync is the caller's job, and getting it wrong is easy.
Measured on a QEMU virtio disk:

| dirty | mean msync | barriers/sec |
|---|---|---|
| 4 KB | 87.8 µs | 11,389 |
| 64 KB | 150.7 µs | 6,635 |
| 1 MB | 955.3 µs | 1,047 |
| 8 MB | 5,880 µs | 170 |

Above 64 KB the cost is **bandwidth**, not commit rate. And a barrier placed at
the device's own single-page latency - 80 µs, which looks like the physically
correct answer - is the *degeneration point*: `sync()` itself costs ~87 µs, so
the check right after a barrier always finds the interval already expired and
the loop collapses to one barrier per two operations. That measured 107,708
barriers over 204,800 inserts and 9.4 s of iowait.

**Use both bounds, whichever fires first**: N operations *or* N microseconds.
The count bounds what a crash can lose; the time bounds how long a writer waits.
With 1,000 ops or 200 µs, the same workload fired 1,399 barriers and durable
updates went from 20,569 to 642,689 ops/sec.

```cpp
if (++since >= batch || now - last >= interval) { table.sync(); since = 0; last = now; }
```

### What a checksum does not prove

CRC32C over header and payload detects a torn write; it does not prove the
record was *meant* to be there. A record whose checksum verifies but whose
append never completed logically is indistinguishable from a good one - that is
what `flags` and a commit marker in the layer above are for.

## 9. Recovery, checkpoint, restart

```cpp
std::size_t recover(std::size_t file_size);
bool checkpoint() noexcept        requires (spec::fast_restart);
std::size_t restart(std::size_t) requires (spec::fast_restart);
bool started_from_checkpoint() const noexcept;
void discard_checkpoint() noexcept;
std::size_t for_each_record(std::size_t from, auto&& fn) const;
```

`recover()` resets the index and walks the log from zero, feeding every record
to the same `apply()` a live write goes through. It stops at the first bad
checksum, at `id == 0` (the sparse tail), or at a record that would run past the
end of the file.

`checkpoint()` on a clean shutdown: msync the log, msync the index, then write a
marker - temp file, fsync, rename, fsync the parent directory. Everything the
marker describes is durable *before* the marker exists.

`restart()` validates magic, that the index and the marker agree on
`write_offset`, that the byte after that offset is still sparse, and the CRC of
the log's tail. **Any disagreement falls back to a full `recover()`** - the
checkpoint is an optimisation, never a source of truth.

## 10. Compaction

```cpp
void compact(std::byte* scratch_base, int scratch_fd = -1);
bool compaction_due(double min_garbage = 0.5, std::size_t min_bytes = 64 << 20) const;
std::size_t compacted_size() const;
double garbage_ratio() const;
```

Compaction rewrites every live record as one contiguous CREATE into a fresh
region. It reclaims more than dead records here: **the whole update history goes
with it**, every chain drops, and the node arena resets rather than being left
as a free list under a high-water mark that never comes back down.

**Pass `scratch_fd`.** It is the file behind `scratch_base`, and passing it is
what keeps the storage owning a file afterwards. Omit it and the fd becomes -1:
`log_fd()` hands a closed descriptor to the async readers, `release_log()`'s
`posix_fadvise` silently does nothing, and `checkpoint()` names an offset in a
file it can no longer fsync. `-1` is correct only when the scratch is genuinely
not a file - a vector, or an anonymous mapping.

`compaction_due()` is a **predicate, not a task**. Compaction needs a fresh
region and its file, and only the caller can supply those; the engine can say
whether it is worth doing, not when you can afford it. The byte floor matters: a
log holding one record and one update is 50% garbage by ratio and worth nothing
to compact, and a caller polling without a floor would compact continuously from
startup.

## 11. Asynchronous read paths

```cpp
int  log_fd() const noexcept;
bool for_each_piece(record_id, auto&& fn) const;         // fn(file_offset, offset_in_record, len)
void for_each_piece_ordered(record_id, auto&& fn) const;
void prefetch(record_id) const noexcept;
bool pin_index() noexcept;
```

Three backends implement the same `read_many(engine, ids, out, stride)` - batch
in, batch out, so the backend is swappable and nothing leaks outward.

| | portability | measured, 512 MB under a 320 MB cap |
|---|---|---|
| `prefetch_gather.hpp` | **POSIX, no threads, no pread** | **156,597** |
| `direct_gather.hpp` | Linux (io_uring + O_DIRECT) | 148,121 |
| `uring_gather.hpp` | Linux (io_uring) | 99,365 |
| plain `get()` in a loop | anywhere | 1,082 |

**`prefetch_gather` is the one to reach for.** It is a single
`posix_madvise(POSIX_MADV_WILLNEED)` over the coalesced page ranges a window of
records is about to touch, issued while the previous window is being assembled.
No threads, no `pread`, no rings - and it beat both Linux-specific paths.

It works by removing two of the three multipliers at once. Queue depth stops
being 1 because the kernel is handed a whole window's pages, and - the part that
surprised the author - **major faults drop to zero**, because the page is
already in the cache by the time the assemble touches it. A 275 µs major fault
becomes a ~1 µs minor one.

The catch is that it only helps when you read in **batches**: a lone `get(id)`
still runs at ~1,100/sec out of RAM, because there is nothing to tell the kernel
in advance when the next id is not yet known.

`for_each_piece` emits the record as **disjoint** pieces in file offsets. Disjoint
is a correctness requirement, not tidiness: `assemble()` gets away with reading
the whole base and overwriting fields on top because it runs in order, and a
ring completes in whatever order the device likes. It returns `false` when two
overrides overlap - only run-time `patch()` can produce that - and emits nothing
in that case, so the caller can fall back whole.

`for_each_piece_ordered` emits the whole base then the overrides, for a reader
that copies out of its own pool and controls the order itself. It exists because
splitting the base under `O_DIRECT` cost 18.5× read amplification against 8.2×
for the contiguous form.

**The field layout is the one that parallelises.** A slot holds the base's offset
*and* every override's offset up front, in a resident structure, so a record with
*d* overridden fields is `1 + d` independent reads whose addresses are all known
before any of them is issued. Under mmap that was a cost; issued to a ring those
same places are read at once. `storage.hpp` has nothing to gain here - its
record is contiguous, so a read is one request.

## 11b. Replication

```cpp
std::size_t apply_wal_range(const std::byte* bytes, std::size_t len);
std::vector<std::byte> export_snapshot() const;
void reset() noexcept;
```

`replication/replication.hpp` ships this engine's log to a replica as-is: the
on-disk record format *is* the wire format, so there is no second schema to keep
in sync with the first.

`apply_wal_range` appends the received bytes **verbatim** and then replays from
the local copy. Verbatim matters twice: the bytes are already complete records
with their own checksums, so copying them keeps the two logs byte-identical and
therefore keeps the two ends agreeing on offsets - which is what lets a replica
resume by naming one. And the replay must run off the local copy, never the
caller's buffer, because a slot's `base` is an offset into *this* mapping;
applying straight from a network payload would record an offset into memory
about to be reused. It returns how many bytes were applied, which is less than
`len` if a checksum fails - the same rule and the same conclusion as recovery.

`export_snapshot` re-encodes every live record as one contiguous CREATE. That
makes it a **state, not a history**: a record with a chain arrives collapsed, and
the result is independent of how the log got that way, which is exactly what a
replica that has fallen out of step needs. Unlike `compact()` it disturbs
nothing - no append, no chain freed, no offset moved.

`reset()` returns to empty, keeping the files. A snapshot is only meaningful
applied on top of nothing, so the desync path is `reset()` then
`apply_wal_range(snapshot)`. The node arena and `next_id` go too, because a
snapshot names its own ids and a stale high-water mark would hand out nodes the
incoming state is about to claim.

Measured on 400 records with chains and deletions: the log was 79,280 bytes and
its snapshot 51,984 - chains collapse away. Corrupting one byte in the middle of
a snapshot applied 25,992 of 51,984 and discarded the rest.

## 12. Limits

**One writer, and readers only while it is idle.** There is no locking anywhere
in this file. The boundary is narrower than "single-threaded", and it was
checked rather than assumed:

| | |
|---|---|
| several readers, no writer running | **safe** - verified with ThreadSanitizer, 8 threads × 96,000 reads, clean |
| a reader concurrent with a writer | **data race**, undefined |
| more than one writer | never |

Concurrent reads work because the whole read path is genuinely `const`:
`alive()`, `record_len()` and `assemble()` only read the slot, walk the node
arena and memcpy out of the mapping. No counters, no lazy fields, nothing
`mutable`.

They stop working the moment a writer moves. ThreadSanitizer puts it at
`field_slot::head()` inside `assemble()`: `set_override` rewrites `chain` - the
list head and the depth share one word - while a reader is reading it, so the
reader can follow a head to a node the writer has not finished filling.

Practically: **split the phases.** A writer runs, stops, and then any number of
threads read in parallel. For analytics over a settled snapshot that is a
working arrangement today with no new code. For readers *during* writes you need
MVCC, and the log already holds every version - the missing piece is an index
per snapshot offset, not a change to this file.

**The index must be resident.** 16 bytes per id *ever issued* plus 24 per live
override. A destroyed record's slot is never reclaimed, so index size tracks
**lifetime id churn, not live-record count**.

At 1 TB of data in 1 KB records the primary index is 16 GB, and each secondary
index in the layer above adds ~32 GB. On a 128 GB machine that binds the
**number of indexes**, not the volume of data. If it ever does not fit, the
structure has no locality to fall back on - a hash spreads keys uniformly by
design, so the resident fraction *is* the hit rate.

**Reads are copies.** Every `get()` materialises. If your workload is
read-dominated and fits in memory, `storage.hpp` is 4× faster at it.

## 13. Best practices

**Size `max_ids` from ids ever issued, not from live records.** Slots are never
reclaimed. If you churn, size for the churn or plan to compact and restart.

**Size `max_overrides` from concurrently-overridden fields**, not from records.
With `max_depth` collapsing, a chain is bounded by distinct fields touched since
the last collapse. Running out is not an error - the record keeps its previous
value rather than a wrong one - but it is silent, so watch
`overrides_used()` against `max_overrides`.

**Set the barrier from measurement, and use both bounds.** Run
`bench/msync_probe.c` on the actual device. Then place the barrier at *N
operations or N microseconds, whichever first*, with the interval comfortably
above what `sync()` costs there. A barrier spaced tighter than its own cost
spends all its time being the previous one.

**Pick `max_depth` from your read/write ratio, not from the default.** Low depth
buys reads and sells writes and log size, at roughly the rates in §6.

**Call `compaction_due()` on a tick and act on it yourself.** Give `compact()`
a real file and pass its fd. Size the region with `compacted_size()`.

**Checkpoint on a policy, not only at shutdown.** Otherwise the crash replay
window is the whole log and grows with uptime.

**Put index maintenance in the observer and constraints in the layer above.**
See §7. Getting this backwards produces a database that is correct until the
first crash.

**Say `auto`, never `const T*`.** See §5.

**Do not `msync` anything but the log.** A per-inode commit means a second
mapped, randomly-written structure in the same file turns every barrier into a
scattered flush - measured at 3.0 s and 1010 MiB against 0.2 s and 40 MiB once
the two were separated. If you need a durable in-memory structure alongside,
keep it in anonymous memory and snapshot it sequentially.

**Do not use `MADV_RANDOM` on the log.** It looks obviously right and is not.
Measured on 26 GB: reads 3,262 → 2,516, updates 10,148 → 6,180, major faults on
reads 72k → 144k. The region is written sequentially during fill and recovery,
where readahead is exactly right, and one advice cannot serve both phases.

**Do not benchmark it with 5,000 operations.** The kernel hands out the first
few thousand dirty pages free; that measures the run-up, not the steady state.
An in-place update read 94,969 ops/sec over 5,000 operations and 11,012 over
200,000.

## 14. Measured

VPS: 1 core, 960 MB RAM, swap off, QEMU virtio disk. 1 KB records. Barrier: 1,000
operations or 200 µs. One process per cell, page cache dropped between them.

| | field | `storage.hpp` | LMDB |
|---|---|---|---|
| random update, in RAM | **3,208,666** | 12,611 | 9,905 |
| random update, out of RAM | **592,926** | 10,641 | 3,560 |
| major faults / update | **41** | 7,986 | 11,501 |
| CPU / iowait on update | **93.8% / 0.00 s** | 3.7% / 13.52 s | 7.3% / 16.58 s |
| random read, in RAM | 1,541,045 | **6,550,781** | 827,178 |
| random read, out of RAM | **5,731** | 4,747 | 3,812 |
| fill | 303,751 | 146,760 | **331,151** |
| durable fill | 151,741 | 92,037 | **315,116** |
| on disk per 1 GB of data | **1.05 GB** | 2.1 GB | 1.34 GB |

The update row is the design. Read it in the two columns beside it rather than
in ops/sec: **41 major faults against ~8,000 and ~11,500**, and 93.8% CPU with
zero iowait against 3-7% CPU with 13-17 seconds of it. Both in-place designs -
ours and a mature B+tree - *stall on the device*. Appending touches no page of
the record, so there is nothing to wait for.

The read row is the cost, and it is real only while the data fits.

## 15. Out of scope

No query language, no planner, no joins. No secondary indexes - those live in
user code over the observer (`bench/app_layer.hpp` is a worked example with
unique constraints, foreign keys, triggers and staged transactions). No
concurrency of any kind. No replication, though the single application point is
the seam one would attach to.
