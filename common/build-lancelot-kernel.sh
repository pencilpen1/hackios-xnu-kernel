#!/bin/bash
# Build and verify repaired XNU; continuous boot is the default.
# Never substitutes the old prebuilt on failure and never flashes a partition.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
export THEOS=${THEOS:-$PWD/local-toolchain/theos}
JOBS=${JOBS:-4}
export JOBS
MODE=${1:-boot}
case "$MODE" in
  boot|checkpoint|sweep) ;;
  *) echo "usage: $0 [boot|checkpoint|sweep]" >&2; exit 1 ;;
esac
mkdir -p out/lancelot
if [ ! -d xnu-src/xnu-8796.121.2 ]; then
  bash xnu/apply.sh
fi
python3 tools/test-lancelot-overlay.py xnu-src/xnu-8796.121.2
if git -C xnu-src/xnu-8796.121.2 apply --check "$PWD/xnu/lancelot.patch" 2>/dev/null; then
  git -C xnu-src/xnu-8796.121.2 apply "$PWD/xnu/lancelot.patch"
else
  git -C xnu-src/xnu-8796.121.2 apply --reverse --check "$PWD/xnu/lancelot.patch"
fi
python3 tools/test-checkpoint-teardown.py xnu-src/xnu-8796.121.2/osfmk/arm64/start.s
RETURN_SOURCE=xnu-src/xnu-8796.121.2/osfmk/arm64/loader_return.c
if ! cmp -s xnu/files/osfmk/arm64/loader_return.c "$RETURN_SOURCE"; then
  echo "loader_return.c differs from the tracked port source; reconcile local XNU edits before building" >&2
  exit 1
fi
python3 tools/test-loader-early-return.py "$RETURN_SOURCE"
KERNEL=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
# XNU's build target performs setup/header export/compile/link. The all/config
# target additionally checks unavailable DriverKit RPC exports and builds kext
# symbolsets. A checkpoint package needs only build; require its real exit status
# instead of accepting any failing `make -k` that happened to leave a kernel.
if ! bash xnu-build.sh -j"$JOBS" build > out/lancelot/kernel-build.log 2>&1; then
  echo "Kernel build failed; see out/lancelot/kernel-build.log" >&2
  exit 1
fi
test -s "$KERNEL"
python3 tools/verify-xnu-checkpoint.py "$KERNEL" | tee out/lancelot/kernel-verification.txt
cp "$KERNEL" out/lancelot/mach.lancelot
sha256sum out/lancelot/mach.lancelot >> out/lancelot/kernel-verification.txt
KERNEL=out/lancelot/mach.lancelot CHECKPOINT=${CHECKPOINT:-5} bash common/build-lancelot.sh "$MODE"
if [ "$MODE" != boot ]; then
  python3 tools/test-lancelot-payload.py out/lancelot/EFI/HackiOS/boot.xnup
fi
echo "Repaired kernel and $MODE files ready (make build exit 0). No device writes performed."
