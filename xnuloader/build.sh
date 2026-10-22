#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
target=${1:-auto}
flags=()
output=loader
base=0xA1000000
case "$target" in
  auto) ;;
  lancelot) flags+=(-DXNULOADER_LANCELOT=1); output=loader-lancelot; base=0x41000000 ;;
  *) echo "usage: $0 [auto|lancelot]" >&2; exit 1 ;;
esac
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter "${flags[@]}" -c loader.c -o "$output.obj"
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:$base /out:$output.efi "$output.obj"
ls -la "$output.efi"
