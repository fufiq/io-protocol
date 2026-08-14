#!/usr/bin/env python3
"""Fold the supported storage headers into one drop-in file.

    tools/amalgamate.py            write dist/field_storage.hpp
    tools/amalgamate.py --check    exit non-zero if the checked-in copy is stale

The engine is already header-only and already needs nothing but the standard
library and POSIX. What this removes is the directory layout: field_storage.hpp
includes ../embedded/platform.hpp by relative path, so copying it alone breaks.
After this, a consumer copies one file.

WHAT IS NOT DONE HERE, deliberately. System includes are left exactly where they
are rather than hoisted and deduplicated at the top. Some of them sit inside
conditionals - <nmmintrin.h> under __x86_64__, <arm_acle.h> under
__ARM_FEATURE_CRC32 - and hoisting those would either break the guard or pull an
x86 header into an ARM build. Repeating a system include costs nothing; every
one of them has its own guard.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "dist" / "field_storage.hpp"

# In dependency order. field_storage.hpp pulls in the first two itself, but
# naming them makes the contents of the amalgamation explicit rather than
# implied by whatever the includes happen to be today.
SOURCES = [
    "embedded/platform.hpp",
    "storage/storage.hpp",
    "storage/field_storage.hpp",
    "storage/prefetch_gather.hpp",
    "storage/blob.hpp",
    "storage/indexes.hpp",
]

BANNER = """// =============================================================================
// field_storage.hpp - GENERATED, single-file build of the storage engine.
//
//     tools/amalgamate.py
//
// Do not edit. Edit the sources and regenerate; `ctest` fails if this file and
// the sources disagree, so a stale copy cannot reach a release.
//
// Contents, in dependency order:
{contents}//
// Needs the standard library and POSIX, and nothing else. To use it, copy this
// one file into your project:
//
//     #include "field_storage.hpp"
//
//     using spec = storage::field_spec<"data.log", 8ull << 30,
//                                      "data.idx", 1'000'000, 200'000>;
//     storage::field_storage<spec> table;
//
// Paths can also be given at run time, which is what a sandboxed process needs:
//
//     storage::field_storage<spec> table(storage::storage_paths::under<spec>(dir));
//
// The specification is FIELD_STORAGE.md in the source repository.
// =============================================================================

#pragma once
"""


def inline(path: Path, seen: set, out: list) -> None:
    """Emit `path`, replacing local includes with the files they name."""
    if path in seen:
        return
    seen.add(path)

    out.append(f'\n// ---- begin {path.relative_to(ROOT)} '
               + '-' * max(0, 60 - len(str(path.relative_to(ROOT)))) + '\n')

    for line in path.read_text().splitlines(keepends=True):
        stripped = line.strip()
        # One #pragma once at the top of the amalgamation covers all of them.
        if stripped == "#pragma once":
            continue
        if stripped.startswith('#include "'):
            target = stripped.split('"')[1]
            resolved = (path.parent / target).resolve()
            if not resolved.exists():
                raise SystemExit(f"{path}: cannot resolve #include \"{target}\"")
            inline(resolved, seen, out)
            continue
        out.append(line)

    out.append(f'// ---- end {path.relative_to(ROOT)}\n')


def build() -> str:
    contents = "".join(f"//   {s}\n" for s in SOURCES)
    out = [BANNER.format(contents=contents)]
    seen: set = set()
    for rel in SOURCES:
        inline((ROOT / rel).resolve(), seen, out)
    return "".join(out)


def main() -> int:
    text = build()
    check = "--check" in sys.argv

    if check:
        if not OUT.exists():
            print(f"{OUT} is missing - run tools/amalgamate.py", file=sys.stderr)
            return 1
        if OUT.read_text() != text:
            print(f"{OUT} is stale - run tools/amalgamate.py and commit the result",
                  file=sys.stderr)
            return 1
        print(f"{OUT.relative_to(ROOT)} is up to date ({len(text.splitlines())} lines)")
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)} ({len(text.splitlines())} lines, "
          f"{len(text) / 1024:.0f} KB) from {len(SOURCES)} sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
