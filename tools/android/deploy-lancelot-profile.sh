#!/system/bin/sh
# Receive a bounded base64 archive directly into Ubuntu through rooted ADB.
# Base64 survives the phone's su terminal wrapper; a line bound supplies EOF.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = lancelot ]
[ "$(readlink -f /dev/block/by-name/Ubuntu)" = /dev/block/mmcblk0p49 ]
[ "$(blockdev --getsize64 /dev/block/by-name/Ubuntu)" = 33285963776 ]
action=${1:?receive or cleanup required}
token=${2:?deployment token required}
case "$token" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#token}" = 32 ]
mountpoint=/data/local/tmp/hackios-profile-$token
profile=${4:-service-log-20261023}
case "$profile" in service-log-20261023|media-prewarm-20261024) ;; *) exit 1;; esac
dest=$mountpoint/hackios/20H392/$profile
incoming=$dest.incoming-$token
mounted_source() {
  awk -v p="$mountpoint" '$2 == p {print $1}' /proc/mounts
}
require_mount() {
  [ "$(readlink -f "$(mounted_source)")" = /dev/block/mmcblk0p49 ]
}
cleanup() {
  cd /
  if [ -n "$(mounted_source)" ]; then
    require_mount
    sync
    umount "$mountpoint"
  fi
  if [ -d "$mountpoint" ]; then rmdir "$mountpoint"; fi
}
case "$action" in
  receive)
    lines=${3:?base64 line count required}
    case "$lines" in ''|*[!0-9]*) exit 1;; esac
    [ "$lines" -gt 0 ] && [ "$lines" -le 100000 ]
    while read -r source target rest; do
      case "$source" in
        /dev/block/*)
          [ "$(readlink -f "$source")" != /dev/block/mmcblk0p49 ] || {
            echo "Ubuntu already mounted at $target" >&2
            exit 1
          }
          ;;
      esac
    done < /proc/mounts
    [ ! -e "$mountpoint" ]
    mkdir "$mountpoint"
    trap cleanup EXIT
    trap 'exit 1' HUP INT TERM
    mount -t ext4 -o rw /dev/block/by-name/Ubuntu "$mountpoint"
    require_mount
    [ ! -L "$mountpoint/hackios" ] && [ ! -L "$mountpoint/hackios/20H392" ]
    [ -d "$mountpoint/hackios/20H392" ] && [ ! -e "$dest" ]
    mkdir "$incoming"
    cd "$incoming"
    echo "RECEIVING $lines base64 lines into Ubuntu"
    set -o pipefail
    head -n "$lines" | base64 -d | tar -xf -
    for file in root.apfs profile.json SHA256SUMS; do
      [ ! -L "$file" ] && [ -f "$file" ]
    done
    [ "$(stat -c %s root.apfs)" = 4194304 ]
    sha256sum -c SHA256SUMS
    sha256sum root.apfs profile.json
    chown 0:0 root.apfs profile.json SHA256SUMS
    chmod 644 root.apfs profile.json SHA256SUMS
    sync
    mv "$incoming" "$dest"
    cd /
    sync
    umount "$mountpoint"
    mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mountpoint"
    require_mount
    cd "$dest"
    sha256sum -c SHA256SUMS
    sha256sum root.apfs profile.json
    cleanup
    trap - EXIT
    echo 'PROFILE VERIFIED AND UNMOUNTED'
    ;;
  cleanup) cleanup;;
  *) exit 1;;
esac
