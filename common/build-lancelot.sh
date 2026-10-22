#!/bin/bash
# ESP boot files for the existing MT6768 UEFI. No Qualcomm boot image packer.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
MODE=${1:-checkpoint}
KERNEL=${KERNEL:-prebuilt/mach.development.bcm2837}
OUT=out/lancelot/EFI/HackiOS
mkdir -p "$OUT"
bash xnuloader/build.sh lancelot
cp xnuloader/loader-lancelot.efi "$OUT/loader.efi"
case "$MODE" in
  checkpoint|sweep)
    # Checkpoint 1 is XNU's assembly entry, before MMU setup (C starts at 10).
    checkpoint=${CHECKPOINT:-1}
    [ "$MODE" != sweep ] || checkpoint=998
    python3 xnuloader/mkpayload.py -o "$OUT/boot.xnup" "kernel=$KERNEL" \
      --checkpoint "$checkpoint" --cmdline '-v bootcpu=6 cpus=1 touchscreen=0 gpu=0 wifi=0 adsp=0'
    ;;
  selftest)
    python3 xnuloader/mkpayload.py -o "$OUT/boot.xnup" "kernel=$KERNEL" --checkpoint 999
    ;;
  ios)
    # The large root/cryptex containers live on the prepared Ubuntu partition.
    # Core 0 remains in UEFI to service read-only eMMC requests; XNU starts on A75 #6.
    test -s userland/root.apfs && test -s userland/flashboot.apfs
    python3 xnuloader/mkpayload.py -o "$OUT/boot.xnup" "kernel=$KERNEL" \
      ramdisk=userland/root.apfs bootdisk=userland/flashboot.apfs \
      --cmdline '-v bootcpu=6 cpus=1 ufs=2 root_apfs=ufs_Ubuntu touchscreen=0 gpu=0 wifi=0 adsp=0 loader_return_after=120'
    ;;
  *) echo "usage: $0 [checkpoint|sweep|selftest|ios]" >&2; exit 1 ;;
esac
sha256sum "$OUT/loader.efi" "$OUT/boot.xnup" > out/lancelot/SHA256SUMS
echo "ESP files: $OUT (experimental $MODE; hardware validation required)"
