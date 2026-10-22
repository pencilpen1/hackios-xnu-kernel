#!/system/bin/sh
# Run as root from Android. Input directory: loader.efi, boot.xnup, SHA256SUMS.
# Installs the test as the removable-media EFI default, preserving the old app.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = lancelot ]
cd "${1:?staged payload directory required}"
sha256sum -c SHA256SUMS
[ -s loader.efi ] && [ -s boot.xnup ]
mountpoint=/mnt/hackios-esp-$$
mkdir "$mountpoint"
mounted=0
cleanup() {
  if [ "$mounted" = 1 ]; then sync; umount "$mountpoint"; fi
  rmdir "$mountpoint"
}
trap cleanup EXIT
mount -t vfat -o rw /dev/block/by-name/esp "$mountpoint"
mounted=1
dest=$mountpoint/EFI/HackiOS
mkdir -p "$dest" "$mountpoint/EFI/BOOT"
previous=$dest/BOOTAA64.before-hackios.efi
if [ ! -e "$previous" ] && [ -f "$mountpoint/EFI/BOOT/BOOTAA64.EFI" ]; then
  cp "$mountpoint/EFI/BOOT/BOOTAA64.EFI" "$previous"
fi
if [ -f "$dest/last.log" ]; then
  cp "$dest/last.log" "$dest/previous-$(date +%s).log"
fi
cp loader.efi "$dest/loader.efi.new"
cp boot.xnup "$dest/boot.xnup.new"
cmp loader.efi "$dest/loader.efi.new"
cmp boot.xnup "$dest/boot.xnup.new"
mv "$dest/loader.efi.new" "$dest/loader.efi"
mv "$dest/boot.xnup.new" "$dest/boot.xnup"
cp loader.efi "$mountpoint/EFI/BOOT/BOOTAA64.EFI.new"
cmp loader.efi "$mountpoint/EFI/BOOT/BOOTAA64.EFI.new"
sync
mv "$mountpoint/EFI/BOOT/BOOTAA64.EFI.new" "$mountpoint/EFI/BOOT/BOOTAA64.EFI"
sha256sum "$dest/loader.efi" "$dest/boot.xnup" "$mountpoint/EFI/BOOT/BOOTAA64.EFI"
echo 'ESP deployment verified. Boot partition has not been changed by this script.'
