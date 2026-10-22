#!/bin/bash
# Build and verify the repaired XNU before packaging a phone checkpoint test.
# Never substitutes the old prebuilt on failure and never flashes a partition.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
export THEOS=${THEOS:-$PWD/local-toolchain/theos}
JOBS=${JOBS:-4}
export JOBS
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
KERNEL=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
START=$(mktemp)
trap 'rm -f "$START"' EXIT
status=0
bash xnu-build.sh -j"$JOBS" -k > out/lancelot/kernel-build.log 2>&1 || status=$?
# Upstream make can fail its post-link export checks; require a fresh, repaired link.
if [ ! -f "$KERNEL" ] || [ ! "$KERNEL" -nt "$START" ]; then
  echo "No fresh kernel produced (make exit $status); see out/lancelot/kernel-build.log" >&2
  exit 1
fi
python3 tools/verify-xnu-checkpoint.py "$KERNEL" | tee out/lancelot/kernel-verification.txt
cp "$KERNEL" out/lancelot/mach.lancelot
sha256sum out/lancelot/mach.lancelot >> out/lancelot/kernel-verification.txt
KERNEL=out/lancelot/mach.lancelot CHECKPOINT=${CHECKPOINT:-5} bash common/build-lancelot.sh checkpoint
python3 tools/test-lancelot-payload.py out/lancelot/EFI/HackiOS/boot.xnup
echo "Repaired kernel and checkpoint files ready (make exit $status). No device writes performed."
