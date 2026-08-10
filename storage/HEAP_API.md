# HEAP_API.md - `storage.hpp` specification

> A single append-only storage over mmap, storing any number of types without
> upfront registration. Strictly single-threaded except for one explicit,
> narrow exception (`background_compact`, see §7). Opens a file itself only
> for `local_sync` storages; owns no threads; blocks nowhere except where
> explicitly documented below.

## 0. Relationship to `record.hpp`

`storage.hpp` generalizes `record.hpp`'s `storage<T>` + `engine<types...>` into
a single class. What changed:

| | `record.hpp` | `storage.hpp` |
|---|---|---|
| Type list | fixed at compile time - `engine<spec1, spec2, ...>` | none - any `T` can be used at any call site |
| Index | one `vector<index_entry>` per type | one shared `vector<index_entry>`, one id space |
| Type tag | `type_id` = index into the compile-time type tuple | `tag` = a value the type declares itself, opaque to the engine |
| Durability | per table - `spec<T, durability, path, capacity>` | per storage - `storage_spec<durability, path, capacity>` |
| Update | WAL diff + in-place cache | identical |
| Compaction | `storage<T>::compact()` / `engine::compact()`, scope depends on which | one `compact()` method, always - a storage owns exactly one region |

The wire format - WAL layout, CRC32C, checksum semantics, the recovery
algorithm - is unchanged in spirit. Only the C++-level type bookkeeping was
generalized: a storage carries no list of types, no dispatch table, and no
per-type storage. `T` exists only at the call site (`create<T>`, `get<T>`,
`for_each<T>`).

## 1. Durability - one level per storage

```cpp
enum class durability : uint8_t { shared, local_sync, replicated };
```

The same three tiers as `record.hpp`, but the level now applies to the
**entire storage**, not to an individual type. A storage can store any mix of
types under one durability domain; if part of your data needs a different
guarantee, create a second storage - that is a split by durability, not by
data type.

| Level | Physically lives | Who syncs, and when | Use when |
|---|---|---|---|
| `shared` (default) | A region the caller opens and passes in | Nobody, explicitly - only the kernel's background writeback | Losing the last few operations on crash is acceptable |
| `local_sync` | Its own file, opened by the storage itself | Explicit `sync()`; the caller decides when | Durability without waiting on replicas |
| `replicated` | Same physical region as `shared` | Not at this layer - durability comes from replica quorum | Replication is wired up outside this file |

## 2. Declaring a storage and its types

```cpp
// One central enum per project - the only place tags are assigned.
// Values are picked by hand and never reused or renumbered once shipped
// (same discipline as protobuf/thrift field numbers) - the engine keeps
// no type registry, so a stale tag on old data is silently misread.
enum class kind : storage::tag_t { user = 1, order = 2 };

// Each type declares its own tag. The storage never sees this list.
struct user {
    static constexpr storage::tag_t tag = (storage::tag_t)kind::user;
    uint64_t balance;
    uint32_t age;
};

struct order {
    static constexpr storage::tag_t tag = (storage::tag_t)kind::order;
    storage::record_id user_id; // a relationship is just a field - the
                                 // engine has no concept of foreign keys
    uint64_t amount;
};

// Bare storage_spec<> = shared durability, no file. Path is a compile-time
// NTTP string, required for local_sync; capacity defaults to 16MB.
using accounts_spec = storage::storage_spec<storage::durability::local_sync, "accounts.db", 64ull * 1024 * 1024>;

storage::storage<accounts_spec> accounts;      // local_sync - opens its own TWO files
storage::storage<> misc(shared_base);          // shared - caller-owned region

// A fourth parameter overrides the derived data-file name if you need the
// two files on different devices:
using split_spec = storage::storage_spec<storage::durability::local_sync, "/nvme/accounts.db",
                                         64ull * 1024 * 1024, "/ssd/accounts.data">;
```

### local_sync owns two files

A `local_sync` storage opens **two** files, not one:

| file | what it holds | synced? | survives a crash? |
|---|---|---|---|
| `spec::path` | the log - the append-only WAL | yes, this is all `sync()` flushes | yes, it is the source of truth |
| `spec::data_path` | the materialized records | never | no - `recover()` rewrites it from the log |

`spec::data_path` defaults to `spec::path + ".data"`, so specs written
before the split keep working unchanged; `capacity` is reserved once per
file, so a `local_sync` storage reserves `2 * capacity` of disk.

Why two files: a filesystem commit is per-**inode**, so `msync` of the log
tail drags every dirty page of that inode into the transaction. With the
in-place patches in the same file, `update()`'s scattered random writes were
all being paid for at every durability point - 1010 MiB written and 3.0 s
for 200k random durable updates, against 40 MiB and 0.2 s once split
(`bench/probe_split_log_and_data.cpp`, confirmed on the real engine by
`bench/probe_inplace_update_cost.cpp`).

Why a **file** rather than anonymous memory for the data: file pages get
written back, go clean, and are then reclaimable, so the dataset may exceed
RAM. Anonymous pages can only go to swap.

`durability::shared` and `durability::replicated` are unchanged - one
caller-provided region, the index aliasing the log payload directly, no
second copy. `storage<spec>::split_regions` tells you which model you have;
`data_region_offset()` reports how much of the data region is in use
(always 0 when unsplit).

### The index, and putting it in a file too

The index is one machine word per id **ever created** - dead slots are never
reclaimed. It is therefore the one structure whose footprint follows
lifetime churn rather than live data, and on the heap it is anonymous
memory, which the kernel can only swap, never evict. A billion ids ever
issued is 8 GB that never comes back.

A fifth spec parameter moves it into a file, so its pages get written back,
go clean, and become reclaimable like every other region here:

```cpp
using accounts_spec = storage::storage_spec<storage::durability::local_sync, "accounts.db",
                                            64ull * 1024 * 1024,
                                            "",                    // data path: derived
                                            "accounts.idx">;       // index in a file
```

It is **not** a checkpoint. `recover()` still rebuilds the index from the
log every time and never reads what a previous run left in the file - the
file is opened truncated for exactly that reason. The only thing it buys is
that the index stops being unswappable anonymous memory.

Sizing is automatic: `spec::index_entries` is `capacity / 32`, because a
record occupies at least `pad8(sizeof(record_header) + 1) == 32` bytes of
log, so a `capacity`-byte log cannot mint more ids than that. The file costs
`capacity / 4` bytes.

One behavioural difference, and it is an improvement: a mapping cannot grow,
so `recreate()` with a hand-picked id past `index_entries` - or a record
whose `id` field is corrupt - is **refused** (returns 0, appends nothing)
rather than turning into an arbitrarily large allocation. The heap-backed
index has no such limit and will try.

`T` must satisfy `tagged_aggregate`: trivially copyable, standard layout,
no padding (`has_unique_object_representations_v`), not a pointer, not
polymorphic, and it must expose `T::tag` convertible to `tag_t` (`uint16_t`).
Do **not** duplicate the id inside the payload - the storage's index already
maps id → bytes; a redundant `id` field is wasted space, not safety.

## 3. Core operations

```cpp
auto id = storage.create(user{1000, 30});     // record_id, monotonic, never reused
storage.update<&user::balance>(id, 900);      // field type deduced from the pointer-to-member
storage.destroy(id);
const user* u = storage.get<user>(id);        // nullptr if dead or never existed
storage.for_each<user>([](storage::record_id id, const user& u) { ... }); // live records only

storage.for_each_raw([](storage::storage<>::raw_view rv) {
    // rv.id, rv.tag, rv.bytes - no T needed; dispatch on rv.tag yourself
});

storage.recreate(id, user{700, 31});          // full replacement, see §4
storage.current_offset();                     // bytes written so far
storage.live_bytes();                         // sum of live bytes across all records
storage.garbage_ratio();                      // 1 - live/offset (see §6 on its floor)
storage.sync();                               // explicit msync of the LOG - delta only, since the last sync()
storage.data_region_offset();                 // bytes used in the data region (0 unless local_sync)
```

`get<T>`, `for_each<T>`, `create<T>`, `recreate<T>` all require
`tagged_aggregate T`. `update<member>` requires only that the *member's*
value type be a `field` (trivially copyable, standard layout) - it patches
one field, not a whole record.

## 4. `update` vs `recreate`

Two different operations, not interchangeable:

- **`update<member>(id, value)`** - patches one field of a record whose
  shape never changes. Writes a WAL diff record (`offset + len + payload`,
  checksummed over header and payload together) **and** writes the same
  bytes in place via a direct cast-assign, so `get()` never needs to replay
  anything - into the data region for `local_sync`, into the log itself for
  an unsplit storage (which then refreshes that record's checksum). Recovery
  never trusts the in-place bytes - it always replays from the WAL - so the
  WAL stays strictly append-only and "stop at the first checksum mismatch"
  needs no caveats.
- **`recreate<T>(id, value)`** - a fresh `CREATE` written over an existing
  id, for when the value's shape or size changes (a variable-length field,
  or a different `T` entirely at the same id). The old bytes become garbage,
  reclaimed on the next compaction. `id` itself never changes, so any other
  record referencing it by id stays valid.

## 4b. Observing the write stream

Secondary indexes are not this engine's job - but making them *recoverable*
is, and that needs one thing from the engine: a single place where a record
becomes state, reached identically by a live write, by `recover()`, and by
replication's `apply_wal_range()`. That place is `apply()`, and an observer
is called from it:

```cpp
struct balance_index {
    std::multimap<uint64_t, storage::record_id> by_balance;

    void on_record(auto& table, const storage::record_header& hdr, const std::byte* payload) {
        // The storage still holds the PREVIOUS state here, so whatever this
        // record currently indexes is the entry to retract.
        if (const user* previous = table.template get<user>(hdr.id))
            retract(previous->balance, hdr.id);
        ...
    }
};

storage::storage<accounts_spec, balance_index> accounts;
accounts.observer_state().by_balance;   // your index, fed by the engine
```

- It runs **before** the record is applied, so `get<T>(hdr.id)` yields the
  value about to be replaced or removed. The new value is in `payload`,
  already checksum-verified.
- Every reason to refuse a record is checked first, so a record the observer
  sees is guaranteed to be applied.
- The default is `null_observer`, which is empty and marked
  `[[no_unique_address]]` - it costs no space and inlines to nothing.
- It fires on a replica too, because replication ingest goes through the same
  `apply()`. An index maintained this way is maintained on the replica for
  free.

### Catching an index up instead of rebuilding it

```cpp
std::size_t stopped = table.for_each_record(my_saved_offset, [&](const storage::record_header& hdr,
                                                                 std::span<const std::byte> payload) {
    ... // fold into my index; return false to stop early
});
```

Walks the verified records of a log range and hands each to you **without
applying anything** - `for_each_raw`'s sibling for a delta rather than a whole
state. Pair it with `current_offset()`: persist "my index reflects the log
through offset X" and replay only what came after, instead of rebuilding from
scratch on every start.

## 5. Durability and checksums

- Every record carries a CRC32C (hardware, SSE4.2) in its header.
- **Every op - CREATE, UPDATE, DELETE - is checksummed over its header and
  its payload together.** No exceptions.
- `recover(file_size)` scans the file strictly sequentially and stops at
  the first checksum mismatch. Nothing past that point is read or applied.
- A `local_sync` storage never patches its log, so its CREATE checksums are
  written once and stay valid. An unsplit (`shared` / `replicated`) storage
  aliases its index into the log, so `update()` rewrites bytes the CREATE's
  checksum covers - it refreshes that checksum in the same step. Either way
  the log is self-verifying end to end.

> **Format break.** CREATE records used to be checksummed over the header
> alone, because the payload was patched in place and a payload-covering
> checksum would have gone stale. The consequence was that a CREATE's payload
> was unprotected: a torn write landing the header's sector but not the
> payload's, or later bit rot, passed recovery and was applied as a real
> value. Logs written before this change do not recover, and a replica must
> run the same build as its primary.

### What a torn write can and cannot do here

Worth being precise, because the intuition is usually wrong. A torn write is
**all-or-nothing per sector**, not per byte or per field - damage is a whole
512 B or 4 KiB chunk missing or stale, never a scattered field.

- A later `update()` **cannot** damage an earlier CREATE's payload. In a
  split storage the log is only ever appended to. And even the page-level
  rewrite of a partially-filled tail page is harmless: the old part of the
  page is written with bytes identical to what is already on the device, so
  a torn sector leaves the same content either way.
- What is exposed is the moment a record is **first** flushed, if it straddles
  a sector boundary - the header's sector can land while the payload's does
  not. That is what the payload checksum now catches.
- The data file is exempt entirely: it is never synced and never read after a
  crash, so a torn page there means nothing. `recover()` rebuilds it.

### `record_header` layout (24 bytes)

| Field | Type | Offset | Notes |
|---|---|---|---|
| `id` | `record_id` (`uint64_t`) | 0 | `id == 0` marks a sparse file tail, not a real record |
| `offset` | `uint32_t` | 8 | Byte offset into the payload an `update` patches |
| `len` | `uint32_t` | 12 | Payload length in bytes |
| `op` | `record_op` (`uint8_t`) | 16 | `create` / `update` / `destroy` |
| - | `uint8_t` | 17 | padding |
| `tag` | `tag_t` (`uint16_t`) | 18 | opaque to the engine, see §2 |
| `checksum` | `uint32_t` | 20 | CRC32C over the header (bytes 0..19) and the payload |

## 6. Compaction - two variants

### Variant 1 - blocking (`storage<spec>::compact()`)

```cpp
storage.compact(scratch_base);
```

Stops all use of the storage for the duration (single-threaded logic, no new
synchronization). Fast for small storages. `scratch_base` is a region the
caller has already opened and mapped. Each live record is copied by
reading its own `len`/`tag` straight from its header - `T` is never
needed, so this works across any mix of types in the storage at once.

**Compaction adopts the regions you hand it.** The storage unmaps them in
its destructor, so pass real mappings of `spec::capacity` - not a
`std::vector`'s bytes - and do not unmap them yourself. Closing the file
descriptor afterwards is fine and expected; the mapping keeps the inode
alive. In exchange the storage now releases the region it was using, which
it previously just dropped on the floor: before this, every compaction
stranded a `spec::capacity` mapping and its fd, unbounded, for the life of
the process (`bench/probe_compaction_leak.cpp` measures 2560 MB of address
space lost over 20 compactions of a 64 MB storage; it is 0 now).

For a `local_sync` storage the signature takes **two** scratch regions -
`compact(scratch_log, scratch_data)` - because both the log and the data
region have to be rebuilt, and the data one cannot be rebuilt in place
(records are read from the live region while the new layout is written into
the other). A `shared` storage keeps the one-argument form; the two are
separate overloads constrained by `requires`, so using the wrong one is a
compile error rather than a storage that silently un-splits itself.

The on-disk files are swapped safely via temp files + `msync` + atomic
`rename()` - see `maybe_compact()` in `stress_heap.cpp`. **`compact()`
itself never calls `msync`** - durability after compaction is entirely the
caller's decision (call `sync()` afterward if you need it; for a `shared`
storage you typically would not).

**On `garbage_ratio()` after compaction**: it will not reach zero - the
24-byte header is counted as non-live by definition. For small payloads
this gives an unavoidable floor (e.g. 75% for an 8-byte `uint64_t` payload:
`1 - 8/32`, since the 24-byte header pads the record to a 32-byte stride).
That is the price of metadata, not leftover garbage.

### Variant 2 - non-blocking (`storage::background_compact()`)

```cpp
std::mutex mu; // the SAME mutex the writer uses (via batch_commit or by hand)
storage::background_compact(storage, mu, "tmp_path.db", "live_path.db", scratch_capacity);

// local_sync needs a temp/live pair for the data file too:
storage::background_compact(storage, mu, "tmp_path.db", "live_path.db", scratch_capacity,
                            "tmp_path.db.data", "live_path.db.data");

// optional trailing params, defaults shown - tune these if phase 3's lock
// is ever measured holding longer than expected under your write rate:
storage::background_compact(storage, mu, "tmp_path.db", "live_path.db", scratch_capacity,
                            "tmp_path.db.data", "live_path.db.data",
                            /*max_catchup_rounds=*/5, /*catchup_threshold_bytes=*/4096);
```

The phase-1 snapshot is itself two mapped files, derived from `tmp_path`
and unlinked as soon as they are mapped - nothing to clean up, nothing a
crash can leave behind. Their size is fixed at open: the arena gets
`scratch_capacity`, the id→ref array gets `spec::index_entries` slots. If
the live set does not fit, the snapshot **latches an overflow flag and the
compaction is abandoned**, leaving the storage exactly as it was - a
snapshot that quietly dropped records would make compaction delete them.

The blocking `compact()` builds no snapshot at all; it streams straight from
the index into the scratch regions. That is the variant to reach for when
the live set approaches RAM.

Requires the writer to also take `mu` on every operation - otherwise the
lock accomplishes nothing. Two **short** locked sections (an initial
snapshot, then a final catch-up + switchover, **including the final
`sync()`** - it has to happen under the same lock the writer uses, since it
touches the same internal offset bookkeeping the writer mutates on every
batch); the bulk of the work runs lock-free:

1. **Locked**: snapshot of currently-live values, copied - not referenced
 - into a private structure.
2. **Lock-free**: write the snapshot into scratch, catch up on the WAL in
   a few approximating passes.
3. **Locked**: one final catch-up (whatever accumulated while catching up),
   the pointer switchover, and `sync()`.

The snapshot is an **arena**, not a `malloc` per live record: one shared
`std::vector<std::byte>` plus a `map<id, arena_ref{tag, offset, len}>`.
`arena_ref` owns nothing - it is a plain `(offset, len)` into the shared
buffer, using offsets rather than pointers so the buffer can grow and
relocate without invalidating existing entries. This is the same principle
the storage itself uses for its own storage (bump the write offset, never
free individually) applied to a temporary, single-call-lifetime buffer.
`arena_ref::offset` is `size_t`, not `uint32_t` - it is the *cumulative*
position across every live record in the snapshot, not a single record's own
length (that's what `record_header::len`, genuinely `uint32_t`-bounded,
covers) - a 32-bit cap here would silently truncate for any storage
snapshotting more than ~4GB of combined live data.
Measured in isolation (no disk I/O), the arena is roughly 3x faster than a
`vector<byte>`-per-entry snapshot once the live-record count reaches the
low thousands; below a few hundred records the difference disappears into
noise.

Call this from its own thread - the function itself is synchronous and
spawns nothing.

## 7. Thread safety - what is and isn't allowed

- **By default**: `storage<spec>` is strictly single-threaded. No atomics
  inside.
- **With `batch_commit` + `background_compact`**: exactly **two** threads
  per storage - writer and compactor - are permitted, **only** if both use
  the same `std::mutex`, and the writer takes it through `batch_commit`
  (or by hand, the same pattern) on every operation.
- Three or more threads on one storage, or touching the storage outside
  `batch_commit`/an explicit lock, is unsupported - undefined behavior.

## 8. `batch_commit` - group commit

Two overloads - plain, and mutex-aware for pairing with
`background_compact`. All types (`table`, `queue`, `apply`, `post_batch`)
are deduced from the arguments - no explicit `template<typename...>` is
needed at the call site.

```cpp
// plain - no mutex, no counter, no hook
storage::batch_commit(storage, queue, running_flag,
    [](auto& h, const Item& item) { /* h.update<...>(...) */ },
    max_batch = 1000, max_window = 5ms);

// mutex-aware - holds mu for the duration of each batch, calls post_batch after
storage::batch_commit(storage, queue, running_flag, mu,
    [](auto& h, const Item& item) { /* h.update<...>(...) */ },
    [](auto& h) { /* e.g. maybe_compact(h) */ },
    max_batch = 1000, max_window = 5ms);
```

Drains items from `queue` (needs `pop_blocking()` / `pop()` returning
`std::optional<Item>`, see `mt_queue.hpp`) in batches - by count or by
time, whichever comes first - applies each through `apply`, and issues
**one** `sync()` per batch.

Neither overload counts "how many were committed" itself - if you need a
progress counter, increment it inside your own `apply` lambda.

**Good** when you only need the fact "applied and synced." **Not** good if
you need to know which **specific** request was confirmed (e.g. to wake
one particular fiber waiting on a `req_id`) - write your own loop for that;
see `business_demo_heap.cpp`.

## 9. Out of scope

- **Actual replication/quorum** - `durability::replicated` is a tag only;
  the mechanism is wired up outside this file.
- **Multi-record transactions/atomicity** - `storage.hpp` gives no "all or
  nothing" guarantee across several `update()`/`create()` calls. Ordering
  and atomicity of multi-record operations are the caller's responsibility.
- **Full protection against a torn CREATE payload** - see §5.
- **A type registry** - the storage never validates that a `tag` matches the
  bytes actually stored at an id, or that two types don't share a tag.
  That discipline lives entirely in your central `enum`.
