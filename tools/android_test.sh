#!/usr/bin/env bash
# Build the storage engine's tests for Android and run them on a device.
#
#   tools/android_test.sh
#
# Needs an NDK and a device or emulator visible to adb.
#
# Builds and runs three suites: the storage engine, the protocol over the epoll
# transport, and replication. The last two need libsodium - the NDK ships no
# crypto and the protocol is mostly crypto - so they are skipped with a message
# rather than silently when SODIUM_ANDROID is not set:
#
#   git clone --depth 1 -b stable https://github.com/jedisct1/libsodium
#   cd libsodium && ANDROID_NDK_HOME=$ANDROID_NDK NDK_PLATFORM=android-24 \
#       ./dist-build/android-x86_64.sh     # or android-armv8-a.sh
#   export SODIUM_ANDROID=$PWD/libsodium-android-westmere
#
# io_uring is absent from all of it on purpose: Android blocks it for apps with
# seccomp, so epoll is the transport there permanently rather than a fallback.
#
#   SANITIZE=1 tools/android_test.sh
#
# builds everything with ASan and UBSan and pushes the NDK's runtime alongside.
# vptr is excluded: it needs RTTI across the statically-linked libc++ boundary
# and reports doctest's own reporter and libc++'s filebuf as violations, which
# is noise about the harness rather than anything under test.
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

FLAGS=(-std=c++23 -Wall -Wextra -static-libstdc++ -Wl,-z,max-page-size=16384)
ASAN_LIB=""
if [ -n "${SANITIZE:-}" ]; then
    FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    ASAN_LIB="$(find "$TOOLCHAIN/.." -name "libclang_rt.asan-${TRIPLE%%-*}-android.so" -print -quit)"
    [ -n "$ASAN_LIB" ] || { echo "no ASan runtime for $TRIPLE in the NDK" >&2; exit 1; }
    echo "sanitizers on; runtime $ASAN_LIB"
else
    FLAGS+=(-O2)
fi

"$CXX" "${FLAGS[@]}" -I"$REPO/build/_deps/doctest-src" \
    "$STAGE/main.cpp" "$REPO/tests/test_storage.cpp" "$REPO/tests/test_field_storage.cpp" \
    -o "$STAGE/storage_tests"
"$CXX" "${FLAGS[@]}" -I"$REPO/dist" "$REPO/tools/amalgam_smoke.cpp" -o "$STAGE/amalgam_smoke"
"$CXX" "${FLAGS[@]}" "$REPO/embedded/platform_check.cpp" -o "$STAGE/platform_check"

BINARIES=(platform_check amalgam_smoke storage_tests)

# The protocol and replication suites, if there is a libsodium to link them
# against. Skipped loudly rather than quietly: "no crypto here" and "the crypto
# tests passed" must not look the same from the outside.
if [ -n "${SODIUM_ANDROID:-}" ] && [ -f "$SODIUM_ANDROID/lib/libsodium.a" ]; then
    "$CXX" "${FLAGS[@]}" -I"$SODIUM_ANDROID/include" -I"$REPO/build/_deps/doctest-src" \
        "$STAGE/main.cpp" "$REPO/tests/test_protocol.cpp" "$SODIUM_ANDROID/lib/libsodium.a" \
        -o "$STAGE/protocol_tests"
    "$CXX" "${FLAGS[@]}" -I"$SODIUM_ANDROID/include" \
        "$REPO/tests/test_replication.cpp" "$SODIUM_ANDROID/lib/libsodium.a" \
        -o "$STAGE/replication_tests"
    BINARIES+=(protocol_tests replication_tests)
else
    echo
    echo "SKIPPING the protocol and replication suites: set SODIUM_ANDROID to a"
    echo "libsodium built for this ABI (see the header of this script)."
fi

[ -n "$ASAN_LIB" ] && "$ADB" push "$ASAN_LIB" /data/local/tmp/ >/dev/null
"$ADB" push "${BINARIES[@]/#/$STAGE/}" /data/local/tmp/ >/dev/null
"$ADB" shell "cd /data/local/tmp && chmod +x ${BINARIES[*]}"

status=0
for t in "${BINARIES[@]}"; do
    echo
    echo "=== $t ==="
    "$ADB" shell "cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp \
        ASAN_OPTIONS=detect_stack_use_after_return=0 ./$t; echo EXIT=\$?" | tee "$STAGE/$t.out"
    grep -q "EXIT=0" "$STAGE/$t.out" || status=1
done

"$ADB" shell "cd /data/local/tmp && rm -f ${BINARIES[*]} *.log *.idx *.ckp *.tmp" >/dev/null 2>&1 || true

echo
[ $status -eq 0 ] && echo "all Android checks passed" || echo "SOME ANDROID CHECKS FAILED"
exit $status
