#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ]; [ "$(getprop ro.product.device)" = lancelot ]
[ "$(readlink -f /dev/block/by-name/esp)" = /dev/block/mmcblk0p48 ]
[ "$(readlink -f /dev/block/by-name/Ubuntu)" = /dev/block/mmcblk0p49 ]
stage=${1:?}; tag=${2:?}; oldroot=${3:?}; oldloader=${4:?}; oldboot=${5:?}; newroot=${6:?}; newloader=${7:?}; newboot=${8:?}; rootbackup=${9:?}
shift 9; mapsha=${1:?}; databytes=${2:?}; backupsha=${3:?}; oldbytes=${4:?}; newbytes=${5:?}
[ "$newbytes" -ge 4194304 ]; [ "$newbytes" -le 16777216 ]
case "$tag" in *[!a-zA-Z0-9_-]*|'') exit 2;; esac
case "$stage" in /data/local/tmp/hackios-*) ;; *) exit 2;; esac
case "$rootbackup" in /hackios/20H392/touch-hardware-20261024/root.before-*.apfs) ;; *) exit 2;; esac
for item in root.apfs loader.efi boot.xnup; do [ -f "$stage/$item" ]; done
[ "$(stat -c %s "$stage/root.apfs")" = "$newbytes" ]
[ "$(sha256sum "$stage/root.apfs" | cut -d ' ' -f 1)" = "$newroot" ]
[ "$(sha256sum "$stage/loader.efi" | cut -d ' ' -f 1)" = "$newloader" ]
[ "$(sha256sum "$stage/boot.xnup" | cut -d ' ' -f 1)" = "$newboot" ]
mp=/mnt/hackios-wifi-install-$$; [ ! -e "$mp" ]; mkdir "$mp"; mounted=0
cleanup(){ cd /; if [ "$mounted" = 1 ]; then sync; umount "$mp"; fi; rmdir "$mp"; }
trap cleanup EXIT
mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mp"; mounted=1
rootfile="$mp/hackios/20H392/touch-hardware-20261024/root.apfs"
[ "$(stat -c %s "$rootfile")" = "$oldbytes" ]
[ "$(sha256sum "$rootfile" | cut -d ' ' -f 1)" = "$oldroot" ]
[ "$(sha256sum "$mp$rootbackup" | cut -d ' ' -f 1)" = "$backupsha" ]
[ "$(sha256sum "$mp/hackios/20H392/ios-data.map" | cut -d ' ' -f 1)" = "$mapsha" ]
[ "$(stat -c %s "$mp/hackios/20H392/ios-data.hfs")" = "$databytes" ]
free_blocks=$(stat -f -c %f "$mp"); block_bytes=$(stat -f -c %S "$mp")
if [ "$newbytes" -gt "$oldbytes" ]; then
 [ -r /sys/fs/ext4/mmcblk0p49/reserved_clusters ]
 reserved=$(cat /sys/fs/ext4/mmcblk0p49/reserved_clusters)
 [ "$(((free_blocks - reserved) * block_bytes))" -ge "$((newbytes - oldbytes + 2097152))" ]
fi
[ ! -e "$stage/root.before-pair.apfs" ]
cp "$rootfile" "$stage/root.before-pair.apfs"; sync
[ "$(sha256sum "$stage/root.before-pair.apfs" | cut -d ' ' -f 1)" = "$oldroot" ]
umount "$mp"; mounted=0
if [ "$oldroot" != "$newroot" ]; then
 mount -t ext4 -o rw /dev/block/by-name/Ubuntu "$mp"; mounted=1
 rootfile="$mp/hackios/20H392/touch-hardware-20261024/root.apfs"
 [ "$(sha256sum "$rootfile" | cut -d ' ' -f 1)" = "$oldroot" ]
# Extend only the bootstrap file, within the verified free space. Existing
# blocks and the fully allocated ios-data file remain in their original extents.
 dd if="$stage/root.apfs" of="$rootfile" bs=4096 conv=notrunc
 if [ "$newbytes" -lt "$oldbytes" ]; then truncate -s "$newbytes" "$rootfile"; fi
 sync
 [ "$(stat -c %s "$rootfile")" = "$newbytes" ]
 [ "$(sha256sum "$rootfile" | cut -d ' ' -f 1)" = "$newroot" ]
 umount "$mp"; mounted=0
fi
mount -t ext4 -o ro,noload /dev/block/by-name/Ubuntu "$mp"; mounted=1
[ "$(sha256sum "$mp/hackios/20H392/touch-hardware-20261024/root.apfs" | cut -d ' ' -f 1)" = "$newroot" ]
[ "$(sha256sum "$mp/hackios/20H392/ios-data.map" | cut -d ' ' -f 1)" = "$mapsha" ]
[ "$(stat -c %s "$mp/hackios/20H392/ios-data.hfs")" = "$databytes" ]
umount "$mp"; mounted=0
mount -t vfat -o rw /dev/block/by-name/esp "$mp"; mounted=1
[ ! -e "$stage/esp.before" ]
mkdir -p "$stage/esp.before/EFI/HackiOS" "$stage/esp.before/EFI/BOOT"
for rel in EFI/HackiOS/loader.efi EFI/HackiOS/boot.xnup EFI/BOOT/BOOTAA64.EFI; do
 old=$oldloader
 if [ "$rel" = EFI/HackiOS/boot.xnup ]; then old=$oldboot; fi
 [ "$(sha256sum "$mp/$rel" | cut -d ' ' -f 1)" = "$old" ]
 cp "$mp/$rel" "$stage/esp.before/$rel"
done
sync
for rel in EFI/HackiOS/loader.efi EFI/HackiOS/boot.xnup EFI/BOOT/BOOTAA64.EFI; do
 old=$oldloader
 if [ "$rel" = EFI/HackiOS/boot.xnup ]; then old=$oldboot; fi
 [ "$(sha256sum "$stage/esp.before/$rel" | cut -d ' ' -f 1)" = "$old" ]
done
# Existing ESP backups stay in place. The complete current pair is verified
# on Android /data before any ESP write and copied to the host before boot.
for rel in EFI/HackiOS/loader.efi EFI/HackiOS/boot.xnup EFI/BOOT/BOOTAA64.EFI; do
 old=$oldloader; new=$newloader; name=loader.efi
 if [ "$rel" = EFI/HackiOS/boot.xnup ]; then old=$oldboot; new=$newboot; name=boot.xnup; fi
 [ "$(sha256sum "$mp/$rel" | cut -d ' ' -f 1)" = "$old" ]
 if [ "$name" = boot.xnup ]; then
  # Native Android is still the boot partition. The old payload is already
  # verified above, so an interrupted write is recoverable before Mu is flashed.
  payloadbytes=$(stat -c %s "$stage/$name")
  previousbytes=$(stat -c %s "$mp/$rel")
  dd if="$stage/$name" of="$mp/$rel" bs=4096 conv=notrunc; sync
  if [ "$payloadbytes" -lt "$previousbytes" ]; then truncate -s "$payloadbytes" "$mp/$rel"; sync; fi
  cmp "$stage/$name" "$mp/$rel"
 else
  [ ! -e "$mp/$rel.$tag-new" ]; cp "$stage/$name" "$mp/$rel.$tag-new"; cmp "$stage/$name" "$mp/$rel.$tag-new"; sync
  mv "$mp/$rel.$tag-new" "$mp/$rel"
 fi
 [ "$(sha256sum "$mp/$rel" | cut -d ' ' -f 1)" = "$new" ]
done
umount "$mp"; mounted=0
mount -t vfat -o ro /dev/block/by-name/esp "$mp"; mounted=1
sha256sum "$mp/EFI/HackiOS/loader.efi" "$mp/EFI/HackiOS/boot.xnup" "$mp/EFI/BOOT/BOOTAA64.EFI"
echo COMPLETE_PAIR_READONLY_VERIFIED_OLD_BACKUPS_RETAINED
