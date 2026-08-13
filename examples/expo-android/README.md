# The engine inside an Expo app

A React Native app that stores its data in `field_storage`, built and run on a
real Android emulator. It exists to answer one question - does the engine work
behind a JavaScript boundary without giving up what makes it fast - and the
answer is on screen rather than in prose.

```sh
./setup.sh              # copies dist/field_storage.hpp, generates the schema, prebuilds
npx expo run:android    # with an emulator or device on adb
```

## What it measured

On the emulator, a 16 KB-page x86-64 image, 2,000 records of 72 bytes:

| | |
|---|---|
| create 2,000 records | 6 ms |
| update **one field** on all 2,000 | 1 ms |
| `sync()` | 2 ms |
| read 2,000 back and decode in JS | 36 ms |
| log after the run | 256,000 bytes |
| live bytes | 144,000 |

That log figure is the whole design in one number. 2,000 creates are
2,000 × (24-byte header + 72-byte payload) = 192,000 bytes, and 2,000 field
updates are 2,000 × (24 + 8) = 64,000. **A field update cost 32 bytes on disk,
not 96** - and it is the same 8-byte write whether the record is 72 bytes or a
kilobyte.

Relaunching the app reports `records already on disk: 2000`, which is the
durability claim rather than a demonstration of it.

## The boundary is a batch, and everything follows from that

An engine update costs about 20 ns; a crossing into native costs orders of
magnitude more. So there is no call here that touches a single record: a query
descriptor goes down, one packed buffer comes up, and TypeScript decodes it with
offsets generated from the same schema the C++ structs came from.

```ts
const ids = FieldStore.createMany(devices);   // N records, one crossing
FieldStore.touch(ids, BigInt(Date.now()));    // one field on all of them
const rows = FieldStore.readMany(ids);        // one buffer back, decoded in JS
```

**It is plain JNI, not JSI, and that is deliberate.** JSI is what you reach for
when the crossing is on the hot path. Here it happens once per batch of
hundreds, so its cost amortises into nothing while JSI's fragility does not. The
one thing JSI would genuinely buy - handing JS a buffer with no copy - is
unavailable to this engine anyway: a field-indexed record is not contiguous
anywhere, so it has to be assembled into a buffer regardless
(`FIELD_STORAGE.md` §5).

## One schema, two languages, and a build error if they disagree

`schema/gen.py` reads `schema/schema.json` and writes both sides. The C++ it
emits asserts every offset:

```cpp
static_assert(offsetof(device, last_seen_ms) == 8);
static_assert(sizeof(device) == 72);
static_assert(storage::tagged_aggregate<device>);
```

so a layout the compiler lays out differently is a build failure, not a wrong
number at run time. Three things it handles that are easy to get wrong by hand:

- **No padding is allowed.** The engine's `aggregate` concept requires
  `has_unique_object_representations`, because checksums are taken over the
  bytes and a gap full of stack garbage would make them non-deterministic. Where
  alignment demands a gap, the generator emits an explicit `_padN` member and
  tells you how many bytes it cost.
- **64-bit fields become `bigint`.** A `serial` above 2^53 is exactly what a
  float64 decoder loses, and the screen checks for it: two neighbouring serials
  that collide as `Number` stay distinct as `bigint`.
- **No `alignof` assert.** The wire contract is the offsets and the size. A
  struct's own alignment is ABI-dependent - 32-bit x86 aligns `uint64_t` to 4 -
  and asserting it failed a build where every offset still matched exactly.

## Two Android traps, both found the hard way

**16 KB pages.** The emulator image here uses them, as do recent devices.
Without `-Wl,-z,max-page-size=16384` every LOAD segment is aligned to 4096 and
the loader reads the dynamic section at the wrong offset - the symptom is not a
clean error but pages of `unused DT entry: unknown (type 0x7077894c…)`, where
the "types" are the binary's own machine code, then a bare SIGSEGV.

**32-bit x86.** Gradle builds it by default and it broke two things that were
true but not worth carrying, so `abiFilters` is set to `arm64-v8a, x86_64`. One
of them was a real engine bug: the SSE4.2 CRC path used `_mm_crc32_u64`, which
only exists on x86-64, under a guard that also admitted `__i386__`.

## What this is not

Not a library. It is one screen and one module, kept small enough to read in a
sitting. iOS is absent because the Darwin paths in `embedded/platform.hpp` have
never been compiled - see the root `README.md`.
