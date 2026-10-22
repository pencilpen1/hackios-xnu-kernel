#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ]
mountpoint=/mnt/hackios-log-$$
mkdir "$mountpoint"
mounted=0
cleanup() {
  if [ "$mounted" = 1 ]; then umount "$mountpoint"; fi
  rmdir "$mountpoint"
}
trap cleanup EXIT
mount -t vfat -o ro /dev/block/by-name/esp "$mountpoint"
mounted=1
cat "$mountpoint/EFI/HackiOS/last.log"
