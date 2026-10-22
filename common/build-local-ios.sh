#!/bin/bash
# Build images from tools/import-local-ios.py output; never invokes fetch.sh.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
SOURCE=$(realpath "${1:?usage: common/build-local-ios.sh PATH_TO_IMPORTED_IOS}")
test -f "$SOURCE/source.json"
for PATCH in common/patches/userland-local-ios.patch common/patches/userland-prebuilt-tests.patch; do
  if git apply --unidiff-zero --check "$PATCH" 2>/dev/null; then
    git apply --unidiff-zero "$PATCH"
  else
    git apply --unidiff-zero --reverse --check "$PATCH"
  fi
done
env PREBUILT=1 REST=1 IOS_SOURCE="$SOURCE" IOS_CACHE="$SOURCE/dyld-cache" bash userland/build.sh
cp "$SOURCE/source.json" userland/system-source.json
