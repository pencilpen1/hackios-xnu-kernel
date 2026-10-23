#!/system/bin/sh
# Receive one new bootstrap profile directly into Ubuntu, never into userdata.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = lancelot ]
[ "$(readlink -f /dev/block/by-name/Ubuntu)" = /dev/block/mmcblk0p49 ]
[ "$(blockdev --getsize64 /dev/block/by-name/Ubuntu)" = 33285963776 ]
! grep -q mmcblk0p49 /proc/mounts
mountpoint=/data/local/tmp/hackios-profile-mount
[ ! -e "$mountpoint" ]
mkdir "$mountpoint"
mounted=0
cleanup() {
  cd /
  if [ "$mounted" = 1 ]; then sync; umount "$mountpoint"; fi
  rmdir "$mountpoint"
}
trap cleanup EXIT
mount -t ext4 -o rw /dev/block/by-name/Ubuntu "$mountpoint"
mounted=1
dest=$mountpoint/hackios/20H392/runtime-log-20261023
[ ! -L "$mountpoint/hackios" ] && [ ! -L "$mountpoint/hackios/20H392" ]
[ -d "$mountpoint/hackios/20H392" ] && [ ! -e "$dest" ]
mkdir "$dest"
cd "$dest"
echo 'READY Ubuntu profile receiver'
toybox nc -s 127.0.0.1 -p 5561 -l -W 60 | tar -xf -
test -s SHA256SUMS
sha256sum -c SHA256SUMS
sync
cd /
umount "$mountpoint"
mounted=0
mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mountpoint"
mounted=1
cd "$dest"
sha256sum -c SHA256SUMS
cd /
umount "$mountpoint"
mounted=0
echo 'PROFILE VERIFIED AND UNMOUNTED'
