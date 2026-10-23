# Lancelot handoff — 2026-10-23

## Device and recovery

- Xiaomi Redmi 9 `lancelot`, MT6768/MT6769T, 6x Cortex-A55 + 2x Cortex-A75, 4 GiB RAM.
- Current expected operating state: Android, USB debugging authorized. Fastboot boot tests can
  hang the device; enter native FASTBOOT with Power + Volume Down. The known recovery files are
  `E:\shuaji\Lancelot\boot.img` and `MuUEFI-lancelot.img`.
- The current boot partition was restored to the supplied Android `boot.img` (SHA-256
  `b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84`).
- Partition backup directory: `D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022`.
  The raw Android boot and ESP backups and the whole Ubuntu backup are in this directory.
  The Ubuntu image is gzip, 33,285,963,776 bytes uncompressed, 2,559,218,342 bytes compressed;
  backup/source SHA-256 is `5c5327b280c1197065f2a82489a1959405e6782dfc0f5cc8661b96872a6ac031`.
  It was verified before/after acquisition. The original ESP is 1 GiB, SHA-256
  `6f1e5c78166360f661c113d638c90e000e114c61ffe3f56b2ecf06c691713510`; it was restored after
  the first FAT-write experiment and verified by whole-partition hash.

## Current image deployment (do not confuse with boot support)

- Target is the existing `Ubuntu` partition, `/dev/block/mmcblk0p49`; userdata is not an image
  staging area. Ubuntu contains the pre-existing root filesystem and must be preserved.
- Mount under `/data/local/tmp/hackios-ubuntu` only for the operation; this is a temporary
  mountpoint on Android userdata, while all copied image bytes go to Ubuntu's ext4.
  Refuse a mounted partition, require partition 49 and exact size, create a new directory
  `hackios/20H392`, and verify every file with SHA-256. Unmount, then remount read-only and
  verify again. No formatting, GPT edits, raw filesystem replacement, or boot-sector write.
- Run from the kernel checkout after Android is online:

```powershell
python tools/deploy-ubuntu-files.py `
  --adb 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\adb.exe' `
  --serial 116745f30503 `
  --workspace '\\wsl.localhost\Ubuntu\home\pencilqwq\hackios-lancelot' `
  --backup 'D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022\Ubuntu-before-ios.img.gz' `
  --log 'D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022\deploy-ios-20261023.log' `
  --write
```

- `--write` is the explicit deployment switch. Omitting it performs full backup/source/device
  hash checks and produces a plan, without modifying Ubuntu.
- Files to place: `ios-containers.img` (root + OS cryptex + App cryptex, 8,388,608,000 bytes),
  `system.apfs`, `root.apfs`, `flashboot.apfs`, and `deployment-source.json`/`SHA256SUMS`.
  The combined container image SHA-256 is
  `0c5685be9960495afccbdd7f0fd257b35620931f7b456ae793c2a55c7241b51a`.
- **Presence on Ubuntu is not a bootable iOS installation.** The current EFI loader reads only
  `EFI/HackiOS/boot.xnup` from ESP; the XNU same-core eMMC read path is not implemented. The
  previous A75 + UEFI mailbox proposal halted and was withdrawn. Do not enable that strategy
  without fresh, explicit design and hardware validation.

## Verified progress

- The 16.7.16 `20H392` IPSW was imported offline by BuildManifest. `root.apfs` and
  `system.apfs` passed `apfsck` and the kernel APFS reader (77 and 62,393 entries respectively).
  `flashboot.apfs` also passed APFS checks. This verifies image integrity, **not** userland ABI.
- Original MU UEFI USB diagnosis works: fastboot `lancelot-uefi`, `getvar:product`, and `oem log`.
  Loader logs append to the firmware's existing LLOG1 ring at `0x512c0000`; use
  `tools/read-lancelot-log.py` with Windows or native Linux fastboot.
- On device, the loader loaded XNUP and XNU returned correctly from checkpoints 1–4.
  Minimal MMU enable/disable/firmware-return probe also passed with ESR/ELR/FAR zero.
  Normal XNU checkpoint 5 (enabling MMU and continuing execution) remains unresolved.
- The pinned kernel is XNU `xnu-8796.121.2`, configured for 4 KiB granules. Actual TCR from the
  prebuilt is `0x22a51a251a`; do not infer 16 KiB merely from CPU support.
- `xnu/lancelot.patch` fixes checkpoint-only and early-exception return ordering; ordinary XNU
  boot bypasses that teardown. Source/application and linked-machine-code verifiers exist.
  The old prebuilt does **not** contain this fix. Kernel build must succeed and pass
  `tools/verify-xnu-checkpoint.py` before packaging/deployment as a repaired kernel.
- Local host RAM is ample (~8.8 GiB free in WSL); the blocker has been extremely slow GitHub
  download throughput for the 86 MiB cross toolchain, not foreground applications. A parallel
  range downloader runs from Windows and writes verified archives to `local/build-deps-20261023`.
  Check that job/files before restarting it; never overwrite its partial output.

## UEFI change boundary

- User instruction: **do not modify UEFI; ask first**. No UEFI change is planned or needed for
  the next steps.
- One existing, explicitly authorized fix is already committed/pushed to Mu-Silicium as
  `251bd657`: MSDC 32-bit FIFO writes now read a full word. The actual function passed ASan/UBSan
  host tests. Its test image hash is `a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c`.
  Do not make additional UEFI changes without asking.
- Kernel repository commits pushed to `pencilpen1/hackios-xnu-kernel`: `0bf2983`, `cae871b`,
  `9ec84c0`, `0221bd8`, `057c0d5`, `013652b`, `23dde59` (check Git for the latest HEAD).

## Next AI: required sequence

1. Read this file and `docs/LANCELOT.md`; check `deploy-ios-20261023.log` and its JSON
   report for `verified: true`. Confirm Ubuntu was unmounted and Android remains online.
   Expected files are under the **Ubuntu filesystem directory** `/hackios/20H392`;
   the device mountpoint `/data/local/tmp/hackios-ubuntu` is temporary and should not exist.
2. Do not flash or alter UEFI. No more UEFI source edits without asking the user first.
   The user authorized the already-pushed MSDC patch only. The boot partition must remain
   `E:\shuaji\Lancelot\boot.img` unless the user approves another test.
3. Finish obtaining the XNU host toolchain. WSL direct GitHub download previously stalled;
   `tools/fetch-xnu-toolchain.py` from Windows also hit connection timeouts. Its failed
   attempt left `.partial` files in `local/build-deps-20261023`; do not trust or resume those
   ranges blindly. Prefer a fresh output directory, verify archive hashes, and install into
   the workspace-local `local-toolchain/theos` with `tools/setup-xnu-offline.sh`.
4. Build the kernel using `JOBS=4 bash common/build-lancelot-kernel.sh`; require the linked
   kernel verifier to confirm checkpoints 1–5. Do not package/flash the old `prebuilt/` as
   if it contained `xnu/lancelot.patch`. Retest checkpoint 5 on the known single CPU and
   keep normal runtime MMU enabled. The opt-in MMU probe is teardown-only, never an OS boot.
5. Implement a real read-only MT6768 MSDC/eMMC kernel driver or a validated same-core data
   loading method. The UEFI EFI app can read its ESP payload, but XNU stops the same-core
   UEFI services; the withdrawn A75/PSCI firmware-mailbox strategy is not the next test.
   Start with reading the deployed APFS container from `/dev/block/mmcblk0p49` before
   attempting root mount. Do not write Ubuntu again: the current iOS files are already
   deployed and the full pre-iOS backup remains available.
6. Test the on-disk iOS 16.7.16 (20H392) images with the 16.5 XNU. If userspace ABI fails,
   fetch the matching 16.5 firmware with the upstream downloader into a **separate** source
   directory; do not overwrite the preserved 20H392 extraction.

Only claim the image deployed after checking both the deploy log and JSON SHA-256 report.
Image files on Ubuntu do **not** mean XNU, native storage, or iOS userspace has booted.
