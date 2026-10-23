#!/bin/bash
# ESP boot files for the existing MT6768 UEFI. No Qualcomm boot image packer.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
MODE=${1:-boot}
if [ "$MODE" = ios ]; then
  # The suspended firmware cannot service the same-core kernel's eMMC mailbox.
  # Fail before replacing any existing loader, payload or checksum files.
  echo "iOS boot requires validated RAM/native eMMC loading; A75 mailbox mode withdrawn." >&2
  exit 1
fi
case "$MODE" in
  boot|checkpoint|sweep|selftest) ;;
  *) echo "usage: $0 [boot|checkpoint|sweep|selftest|ios]" >&2; exit 1 ;;
esac
KERNEL=${KERNEL:-out/lancelot/mach.lancelot}
[ -s "$KERNEL" ] || { echo "Build the repaired kernel with common/build-lancelot-kernel.sh first" >&2; exit 1; }
OUT=out/lancelot/EFI/HackiOS
mkdir -p "$OUT"
bash xnuloader/build.sh lancelot
cp xnuloader/loader-lancelot.efi "$OUT/loader.efi"
case "$MODE" in
  boot)
    # Continue through all checkpoints. Panic/exception return remains available.
    python3 xnuloader/mkpayload.py -o "$OUT/boot.xnup" "kernel=$KERNEL" \
      --cmdline '-v cpus=1 touchscreen=0 gpu=0 wifi=0 adsp=0'
    python3 tools/test-lancelot-payload.py "$OUT/boot.xnup" --expect-boot
    ;;
  checkpoint|sweep)
    # Checkpoint 1 is XNU's assembly entry, before MMU setup (C starts at 10).
    checkpoint=${CHECKPOINT:-1}
    [ "$MODE" != sweep ] || checkpoint=998
    python3 xnuloader/mkpayload.py -o "$OUT/boot.xnup" "kernel=$KERNEL" \
      --checkpoint "$checkpoint" --cmdline '-v cpus=1 touchscreen=0 gpu=0 wifi=0 adsp=0'
    ;;
  selftest)
    python3 xnuloader/mkpayload.py -o "$OUT/boot.xnup" "kernel=$KERNEL" --checkpoint 999
    ;;
esac
sha256sum "$OUT/loader.efi" "$OUT/boot.xnup" > out/lancelot/SHA256SUMS
echo "ESP files: $OUT (experimental $MODE; hardware validation required)"
