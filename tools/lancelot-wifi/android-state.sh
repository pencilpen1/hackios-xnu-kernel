#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ]; [ "$(getprop ro.product.device)" = lancelot ]
[ "$(readlink -f /dev/block/by-name/esp)" = /dev/block/mmcblk0p48 ]
[ "$(readlink -f /dev/block/by-name/Ubuntu)" = /dev/block/mmcblk0p49 ]
mp=/mnt/hackios-wifi-state-$$; [ ! -e "$mp" ]; mkdir "$mp"; mounted=0
cleanup(){ cd /; if [ "$mounted" = 1 ]; then umount "$mp"; fi; rmdir "$mp"; }
trap cleanup EXIT
mount -t vfat -o ro /dev/block/by-name/esp "$mp"; mounted=1
stat -f -c "ESP_FREE %f %S" "$mp"
sha256sum "$mp/EFI/HackiOS/loader.efi" "$mp/EFI/HackiOS/boot.xnup" "$mp/EFI/BOOT/BOOTAA64.EFI"
umount "$mp"; mounted=0
mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mp"; mounted=1
stat -f -c "UBUNTU_FREE %f %S" "$mp"
if [ -r /sys/fs/ext4/mmcblk0p49/reserved_clusters ]; then printf "EXT4_RESERVED_CLUSTERS "; cat /sys/fs/ext4/mmcblk0p49/reserved_clusters; fi
stat -c "ROOT_BYTES %s" "$mp/hackios/20H392/touch-hardware-20261024/root.apfs"
sha256sum "$mp/hackios/20H392/touch-hardware-20261024/root.apfs" "$mp/hackios/20H392/touch-hardware-20261024/profile.json" "$mp/hackios/20H392/flashboot.apfs" "$mp/hackios/20H392/ios-data.map"
stat -c "%s %b %n" "$mp/hackios/20H392/ios-data.hfs"
if [ -n "${1:-}" ]; then
 case "$1" in /hackios/20H392/touch-hardware-20261024/root.before-*.apfs) ;; *) exit 2;; esac
 sha256sum "$mp$1"
fi
umount "$mp"; mounted=0
echo READ_ONLY_STATE_COMPLETE
