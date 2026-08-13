#!/usr/bin/env bash
# Turn this directory into a runnable Expo app.
#
# Only the parts worth reading are checked in: the native module, the schema and
# its generator, and the screen. Everything else - node_modules, the generated
# android/ project - is produced here, because a repository is not a good place
# for half a gigabyte of dependencies or for a second copy of the engine that
# can drift from dist/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

# The engine arrives as ONE file, which is what dist/ is for.
cp "$REPO/dist/field_storage.hpp" "$HERE/modules/fieldstore/android/src/main/cpp/"

# One schema, both sides: C++ structs whose every offset is static_asserted, and
# a TypeScript decoder generated from the same offsets.
python3 "$HERE/schema/gen.py"

npm install
npx expo prebuild --platform android --clean
echo
echo "now: npx expo run:android   (with an emulator or device on adb)"
