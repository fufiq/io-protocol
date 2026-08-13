#!/usr/bin/env bash
# Build the storage engine's tests for Android and run them on a device.
#
#   tools/android_test.sh
#
# Needs an NDK and a device or emulator visible to adb. Only the ENGINE is
# built: net/ and replication/ want io_uring and libsodium, which Android does
# not have, so they are Linux-only and this does not pretend otherwise.
#
# Two flags here are not decoration, and both were found the hard way on a
# 16 KB-page image:
#
#   -Wl,-z,max-page-size=16384   Without it every LOAD segment is aligned to
#                                4096, and the loader on a 16 KB-page device
#                                reads the dynamic section at the wrong offset.
#                                The symptom is not a clean error - it is pages
#                                of "unused DT entry: unknown (type 0x7077894c…)"
#                                where the "types" are the binary's own machine
#                                code, then either CANNOT LINK EXECUTABLE or a
#                                bare SIGSEGV.
#
#   -static-libstdc++            Otherwise the binary needs libc++_shared.so
#                                pushed alongside it.
#
# The API level must be recent enough for the image. Building against 24 and
# running on API 37 produced the same misparsed dynamic section as the alignment
# problem, from a different cause.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
NDK_ROOT="${ANDROID_NDK:-$HOME/Android/Sdk/ndk}"
NDK="${NDK_PATH:-$(ls -d "$NDK_ROOT"/*/ 2>/dev/null | sort -V | tail -1)}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
STAGE="${TMPDIR:-/tmp}/android_engine_tests"

[ -d "$TOOLCHAIN" ] || { echo "no NDK toolchain at $TOOLCHAIN" >&2; exit 1; }
command -v "$ADB" >/dev/null 2>&1 || [ -x "$ADB" ] || { echo "no adb at $ADB" >&2; exit 1; }

ABI="$("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r')"
API="$("$ADB" shell getprop ro.build.version.sdk | tr -d '\r')"
PAGE="$("$ADB" shell getconf PAGESIZE | tr -d '\r')"
case "$ABI" in
    arm64-v8a) TRIPLE=aarch64-linux-android ;;
    x86_64)    TRIPLE=x86_64-linux-android ;;
    *) echo "unhandled ABI $ABI" >&2; exit 1 ;;
esac

# Clamp to the newest toolchain the NDK actually ships.
NEWEST="$(ls "$TOOLCHAIN" | sed -n "s/^${TRIPLE}\([0-9]\+\)-clang++$/\1/p" | sort -n | tail -1)"
LEVEL=$(( API < NEWEST ? API : NEWEST ))
CXX="$TOOLCHAIN/${TRIPLE}${LEVEL}-clang++"

echo "device: $ABI, API $API, page size $PAGE - building with ${TRIPLE}${LEVEL}"

mkdir -p "$STAGE"
cat > "$STAGE/main.cpp" <<'EOF'
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
EOF

DOCTEST="$(find "$REPO/build/_deps/doctest-src" -maxdepth 1 -type d -name doctest -print -quit 2>/dev/null || true)"
[ -n "$DOCTEST" ] || { echo "configure the host build first so doctest is fetched" >&2; exit 1; }

FLAGS=(-std=c++23 -O2 -Wall -Wextra -static-libstdc++ -Wl,-z,max-page-size=16384)

"$CXX" "${FLAGS[@]}" -I"$REPO/build/_deps/doctest-src" \
    "$STAGE/main.cpp" "$REPO/tests/test_storage.cpp" "$REPO/tests/test_field_storage.cpp" \
    -o "$STAGE/storage_tests"
"$CXX" "${FLAGS[@]}" -I"$REPO/dist" "$REPO/tools/amalgam_smoke.cpp" -o "$STAGE/amalgam_smoke"
"$CXX" "${FLAGS[@]}" "$REPO/embedded/platform_check.cpp" -o "$STAGE/platform_check"

"$ADB" push "$STAGE/storage_tests" "$STAGE/amalgam_smoke" "$STAGE/platform_check" /data/local/tmp/ >/dev/null
"$ADB" shell "cd /data/local/tmp && chmod +x storage_tests amalgam_smoke platform_check"

status=0
for t in platform_check amalgam_smoke storage_tests; do
    echo
    echo "=== $t ==="
    "$ADB" shell "cd /data/local/tmp && ./$t; echo EXIT=\$?" | tee "$STAGE/$t.out"
    grep -q "EXIT=0" "$STAGE/$t.out" || status=1
done

"$ADB" shell "cd /data/local/tmp && rm -f storage_tests amalgam_smoke platform_check *.log *.idx *.ckp *.tmp" >/dev/null 2>&1 || true

echo
[ $status -eq 0 ] && echo "all Android checks passed" || echo "SOME ANDROID CHECKS FAILED"
exit $status
