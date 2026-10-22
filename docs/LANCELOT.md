# Lancelot / MT6768 bring-up

## Source and hardware baseline (2026-10-22)

- Android reports `lancelot`, `mt6768`, hardware `MT6769T`; 4 GiB physical DDR.
- 6 Cortex-A55 + 2 Cortex-A75, MPIDR `0x000..0x700`, stride `0x100`, little mask `0x3f`.
  Vendor DT unit names (`cpu@001`, etc.) **do not equal** `reg`; the extractor now uses `reg`.
- GICv3 distributor `0x0c000000`, redistributors `0x0c040000`; PSCI uses SMC.
- GOP framebuffer reservation: `0x7cdb0000..0x7f000000`; NT36672A Dijing panel.
- USB: `mediatek,mt6768-usb20`, MUSB MAC `0x11200000`, USB2 PHY `0x11cc0800`.
  Android's `musb-hdrc` is configured at high speed in device/sink mode.
- Storage is eMMC/MSDC, not Qualcomm UFS. GPT names are case-sensitive: `esp`, `Ubuntu`.
- Android reference: [Shockwave 17.0](https://github.com/MrShockWAVEog/shockwave_kernel_xiaomi_mt6768/tree/412a5a948c67309ca5f6ecbf1450dd6caf5cfc1b).
- Existing UEFI: [pencilpen1/Mu-Silicium](https://github.com/pencilpen1/Mu-Silicium),
  baseline `293c87e0`, lancelot USB dual v0.3. It has successfully booted Linux on this device.

Raw Android evidence and partition backups belong in the ignored `local/` directory.
Use `tools/collect-android-port.py --adb PATH --serial SERIAL --out NEW_DIRECTORY` to repeat collection.

## EFI/XNU boot

```sh
bash common/build-lancelot.sh checkpoint
```

Produces `out/lancelot/EFI/HackiOS/loader.efi` and `boot.xnup`. The EFI app loads the XNUP
from its own ESP, allocates its memory through UEFI, and writes `EFI/HackiOS/last.log`.
The lancelot build explicitly selects MTK even if GOP's buffer moves. It does not use the
redwood boot shim, hard-coded payload reservation, Qualcomm USB protocol or UFS registers.
The generic `common/build.sh lancelot` entry delegates to this path.

`checkpoint` currently stops at XNU assembly checkpoint 1 (before page tables), and `selftest`
exercises just the loader's context round trip. Neither is a complete iOS boot.
The shipped XNU is the repository's unmodified prebuilt `xnu-8796.121.2`.
Build success is not hardware validation: advance through checkpoints 1–5, 10+, then
root mount and userland after examining the real device log.

## Offline iOS image preparation

The supplied IPSW is **16.7.16 / 20H392**, whereas upstream's XNU and injected libraries
target **16.5 / 20F66**. The importer preserves the actual build identity. ABI compatibility
and SpringBoard on MTK remain to be tested.

Use a Linux filesystem, including WSL's ext4, for extraction:

```sh
bash common/setup.sh
python3 tools/import-local-ios.py /path/to/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw \
  --workspace "$PWD" --out "$PWD/ios-local/20H392"
bash common/build-local-ios.sh ios-local/20H392
```

No iOS download is performed. The root/OS cryptex/App cryptex paths come from BuildManifest,
ZIP CRCs are checked while extracting, and SHA-256s are recorded in `source.json`.
`common/patches/userland-local-ios.patch` is the small reproducible change to the separate
userland checkout that makes its input/cache paths configurable. Apple's bytes and output
images are local artifacts, not Git content.

## Logging and remaining bring-up

The existing UEFI exposes diagnostic fastboot `18d1:d00d`, serial `lancelot-uefi`,
interface 0, OUT `0x01`, IN `0x81`. Its `oem log <16-hex-digit-cursor>` returns LLOG1/LLOGD
records from a RAM ring at `0x512c0000`. It is not a CDC ACM COM port, and is not the
loader's Qualcomm USB API. ESP logs provide the initial kernel evidence while USB transport
integration is developed. UEFI USB remains a boot-services facility; running XNU on the same
core suspends its callbacks until the kernel returns.

Full iOS boot also needs a low-memory storage strategy (the system image exceeds the usable
contiguous RAM), MTK input and watchdog integration, and verification of 16.7 userspace with
the 16.5 kernel. Mali GPU, MediaTek Wi-Fi/audio and native eMMC support are not provided by
the redwood drivers.
