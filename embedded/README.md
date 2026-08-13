# embedded/ - running the engine off a desktop Linux box

Two things stand between `field_storage.hpp` and a phone, and both live here.

**Where the files go** is not here at all - it turned out to belong in the
engine, as `storage::storage_paths` (see `FIELD_STORAGE.md` §1). A sandboxed
process learns its directory at startup, so:

```cpp
storage::field_storage<spec> table(storage::storage_paths::under<spec>(app_dir));
```

**What the system calls mean** is `platform.hpp`. Four of them are not the same
everywhere, and each difference is a correctness problem rather than a
portability annoyance:

| | difference | what breaks if ignored |
|---|---|---|
| `posix_fallocate` | absent on Darwin | a sparse mapping raises **SIGBUS** when the disk fills mid-write, and a full phone is ordinary |
| `fsync` / `msync` | on Darwin do **not** flush the drive's write cache | §8's durability is silently not durable after power loss |
| `posix_fadvise` | absent on Darwin | nothing - it is a hint, and the Darwin branch does nothing on purpose |
| page size | 16 KB on Apple silicon and recent Android, not 4096 | `msync` rejects a misaligned address, and nothing checks its return value |

That last one is the quiet one. The engine rounds a flush's start down to a page
boundary; round to 4096 on a 16 KB-page device and the address you hand `msync`
is not page-aligned, so it fails - and durability fails with it, without a
single error surfacing. `platform::page_size()` asks the OS once and caches it.

## The durability choice on Darwin, which is real

`msync` and `fsync` on Darwin hand the data to the device and return. They do
not wait for it to reach the medium.

```cpp
storage::platform::durability_mode() = storage::platform::sync_mode::barrier;
```

- **`full`** (the default) - `fcntl(F_FULLFSYNC)`, which flushes the drive's own
  cache. This is what `fsync` means on Linux and the only setting under which
  the guarantees in `FIELD_STORAGE.md` §8 survive a power cut. It is slow, and
  on some devices dramatically so.
- **`barrier`** - `fcntl(F_BARRIERFSYNC)`, which orders this write ahead of
  later ones without waiting for the medium. Survives a process or kernel crash,
  not a power cut. Apple's own SQLite build uses this even when an application
  explicitly asks for full synchronisation, so it is the platform's de facto
  normal rather than a corner to apologise for.

The default is the safe one because the safe one should be what you get without
thinking about it. Changing it is one line and should be a decision you can
point at.

## Status: the Darwin paths are unverified

They were written against Apple's documented behaviour and reviewed, but never
compiled and never run - the machine they were written on has no Darwin
toolchain. **Every `#if defined(__APPLE__)` branch needs a build and a run on a
real Mac before it is trusted.** The Linux paths are under continuous test.

One bug was already caught by review rather than by a compiler, which is a fair
warning about the rest: `F_PREALLOCATE` with `F_PEOFPOSMODE` allocates its
length *past the current end of the file*, not up to an absolute size. The
obvious translation of `posix_fallocate` would therefore have doubled the file
on every restart, because the engine preallocates on every open and
`posix_fallocate` is idempotent. The shim now allocates only the shortfall.

## Verifying a port

```sh
cmake --build build --target platform_check && ./build/platform_check
```

It exercises each shim against a real file and checks the property the engine
depends on, not merely that the call returned zero: that the space is genuinely
reserved rather than sparse (`st_blocks`), that preallocating twice is
idempotent, that a page-aligned subrange flushes, and that the barrier mode is
harmless. It runs on Linux too, so a change that breaks the contract shows up on
the machine doing the development.

## Android

Mostly already works. `posix_fallocate` is present from API 21 and the page size
comes from `sysconf`, so what Android actually needed was the runtime path - the
app's private directory - which is in the engine. The one thing to keep in mind
is memory: the index has to stay resident (`FIELD_STORAGE.md` §12), and 16 bytes
per id ever issued is small at phone scale but is the number to size from.

## iOS, beyond the shims

- **Data protection.** A file encrypted with the default class cannot be mapped
  while the device is locked. `NSFileProtectionCompleteUntilFirstUserAuthentication`
  is the usual setting for a database that must survive a screen lock.
- **`mlock`** is restricted, so `pin_index()` returns false. That is already the
  correct behaviour - it is best-effort by design - and needs nothing here.
- **Jetsam** kills by resident size, which suits this engine: the log is
  file-backed, so its clean pages are evictable rather than swappable.
