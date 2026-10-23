#!/system/bin/sh
# Mounted-file deployment only. Never writes a block device or formats Ubuntu.
# Keep mount, receiver and verification in one root/mount namespace.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = lancelot ]
[ "$(readlink -f /dev/block/by-name/Ubuntu)" = /dev/block/mmcblk0p49 ]
[ "$(blockdev --getsize64 /dev/block/by-name/Ubuntu)" = 33285963776 ]
action=${1:?action required}
mountpoint=/data/local/tmp/hackios-ubuntu
dest=$mountpoint/hackios/20H392
if grep -qE '^/dev/block/(by-name/Ubuntu|mmcblk0p49) ' /proc/mounts; then
  echo 'Ubuntu already mounted; refusing a second mount' >&2
  exit 1
fi
[ ! -L "$mountpoint" ]
mkdir -p "$mountpoint"
[ -z "$(ls -A "$mountpoint")" ]
mounted=0
cleanup() {
  if [ "$mounted" = 1 ]; then
    sync
    if umount "$mountpoint"; then echo 'Ubuntu partition unmounted';
    else echo 'ERROR: could not unmount Ubuntu' >&2; exit 1; fi
  fi
  rmdir "$mountpoint"
}
trap cleanup EXIT
case "$action" in
  inspect)
    mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mountpoint"
    mounted=1
    df -k "$mountpoint"
    ls -ld "$mountpoint/bin" "$mountpoint/usr" "$mountpoint/etc"
    if [ -d "$dest" ]; then echo 'Existing HackiOS files:'; ls -l "$dest"; fi
    ;;
  deploy)
    [ "${2:-}" = yes ]
    mount -t ext4 -o rw /dev/block/by-name/Ubuntu "$mountpoint"
    mounted=1
    [ ! -e "$dest" ]
    free_kib=$(df -k "$mountpoint" | tail -1 | awk '{print $4}')
    [ "$free_kib" -ge 11534336 ] || { echo 'Need 11 GiB free on Ubuntu' >&2; exit 1; }
    mkdir -p "$dest"
    cd "$dest"
    # nc is bound to loopback; the image stream comes through adb forward.
    echo 'READY mounted Ubuntu ext4 at /data/local/tmp/hackios-ubuntu'
    toybox nc -s 127.0.0.1 -p 5558 -l -W 60 | tar -xf -
    test -s SHA256SUMS
    sha256sum -c SHA256SUMS
    sync
    echo 'FILES VERIFIED on Ubuntu partition (not userdata)'
    ls -l .
    cd /
    ;;
  verify)
    mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mountpoint"
    mounted=1
    cd "$dest"
    sha256sum -c SHA256SUMS
    cat deployment-source.json
    df -k "$mountpoint"
    echo 'READ-ONLY REMOUNT VERIFIED'
    cd /
    ;;
  *) echo 'usage: deploy-ubuntu-files.sh inspect|deploy yes|verify' >&2; exit 1 ;;
esac
