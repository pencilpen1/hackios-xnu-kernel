# Lancelot handoff — 2026-10-23

## Latest user constraints

- Keep Android running: **no reboot or boot test in the current session**. Continue the
  host-side toolchain/kernel repair; checkpoint 5 hardware validation remains for a later
  explicitly authorized test.
- ESP corruption is resolved by the existing UEFI MSDC patch, as confirmed by the user.
  Every future authorized UEFI source change must receive its own Git commit; ask before
  making new UEFI changes. No additional UEFI source changes were made for this deployment.
- Prefer a GitHub mirror for new downloads; keep size/hash/archive checks and use direct
  GitHub only as a fallback. Existing complete verified local archives need no redownload.
- The user now permits removal of old Ubuntu OS files. They have not been deleted: deployment
  is already complete and about 15.8 GiB remains free. Never delete `/hackios/20H392`, the
  verified host backup, userdata, ESP, or GPT as part of any later Ubuntu-only cleanup.

## Device and recovery

- Xiaomi Redmi 9 `lancelot`, MT6768/MT6769T, 6x Cortex-A55 + 2x Cortex-A75, 4 GiB RAM.
- Final confirmed operating state: Android (`sys.boot_completed=1`), USB debugging authorized.
  Final check is saved in kernel checkout `local/device-final-20261023.txt` (Ubuntu unmounted,
  mountpoint absent, Android boot SHA-256 unchanged, battery 100%, no deployment TCP forward).
  Fastboot boot tests can
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
  staging area. This completed deployment preserved the pre-existing root filesystem; the
  user now permits removal of those old Ubuntu files if needed.
- Mount under `/data/local/tmp/hackios-ubuntu` only for the operation; this is a temporary
  mountpoint on Android userdata, while all copied image bytes go to Ubuntu's ext4.
  Refuse a mounted partition, require partition 49 and exact size, create a new directory
  `hackios/20H392`, and verify every file with SHA-256. Unmount, then remount read-only and
  verify again. No formatting, GPT edits, raw filesystem replacement, or boot-sector write.
- **Deployment completed successfully on 2026-10-23. Do not run the deployment command again.**
  The four payloads and metadata are at `/hackios/20H392` on Ubuntu. Device mountpoint was
  confirmed absent after completion; `/dev/block/mmcblk0p49` was confirmed unmounted. Android
  remained online (`lancelot`); battery was 100% at final check. Ubuntu had 16,588,904 KiB free
  after deployment.
- Durable host-side evidence:
  `D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022\deploy-ios-20261023.log` and
  `deploy-ios-20261023.log.json` in the same directory. JSON reports `verified: true`; the
  log records initial writable transfer verification, sync/unmount, read-only remount hash
  verification, and final unmount.
- Final SHA-256 values (also stored in on-device `SHA256SUMS` and host JSON):

  | File | Bytes | SHA-256 |
  |---|---:|---|
  | `ios-containers.img` | 8,388,608,000 | `0c5685be9960495afccbdd7f0fd257b35620931f7b456ae793c2a55c7241b51a` |
  | `system.apfs` | 2,497,843,200 | `43259e8c096dbf8326fb772e6fc3e061f80992a0f4ce3a1617a1c396a2e410ed` |
  | `root.apfs` | 4,194,304 | `af779aca030322e02d33c6d9c503a20146f7ebdc0adb5fa29fd44d44b0347399` |
  | `flashboot.apfs` | 4,194,304 | `14485e7c5ce125ef516168445c2500ac04ca5e40a908c2aef516b2d92c24ba0c` |

- For audit only, this is the command that was executed; it writes files and is **not** a dry-run:

```powershell
python tools/deploy-ubuntu-files.py `
  --adb 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\adb.exe' `
  --serial 116745f30503 `
  --workspace '\\wsl.localhost\Ubuntu\home\pencilqwq\hackios-lancelot' `
  --backup 'D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022\Ubuntu-before-ios.img.gz' `
  --log 'D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022\deploy-ios-20261023.log'
```

- `tools/deploy-ubuntu-files.py` has no dry-run or `--write` option: invoking it performs the
  deployment, and it refuses to overwrite the existing log or destination directory. Do not
  rerun it. `deployment-source.json` and `SHA256SUMS` are alongside the four payload files.
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
  The old prebuilt does **not** contain this fix. The new host-built `mach.lancelot` passed
  `tools/verify-xnu-checkpoint.py`; its checkpoint-5 payload passed validation. These are
  static checks, not an on-device checkpoint-5 pass.
- Local host RAM is ample (~8.9 GiB available in WSL). A complete earlier download exists in
  `local/build-deps-20261022` in the Windows kernel checkout: all three archives matched their
  manifest SHA-256/length and passed XZ decompression integrity checks on 2026-10-23.
  The later `local/build-deps-20261023` retry timed out and contains unusable `.partial` files;
  it is not running and is not the source for installation. Offline setup from the verified
  20261022 directory completed successfully, including the firehose/pthread kernel libraries.

## Completed host rebuild and remaining build limitation

- WSL workspace: `/home/pencilqwq/hackios-lancelot` (Linux filesystem).
  `JOBS=4 CHECKPOINT=5 bash common/build-lancelot-kernel.sh` passed with **make build exit 0**.
  Tests passed: overlay applies/reverses, compiled checkpoint teardown, linked checkpoints
  1–5 (normal path bypasses teardown), and boot-CPU-only checkpoint-5 XNUP validation.
- The setup now supplies a workspace-local `python` link to Python 3 for XNU's LLDB syntax
  checker. The actual checker ran successfully; no system Python installation was changed.
- The checkpoint builder now uses XNU's explicit `build` target and stops on compile/link
  failure. It no longer accepts arbitrary `make -k` failures just because a kernel exists.
  The full `all/config` target still fails the existing DriverKit RPC export/symbolset checks
  because the repository uses partial `iig` stubs (`xnu/README.md`). This is a recorded
  limitation, not a passed full SDK/export build. No export list was weakened.
- Artifacts under WSL `out/lancelot/` are also copied and SHA-256 verified in Windows
  `D:\AI_rom\iOSforLava\hackios-xnu-kernel\local\lancelot-repaired-20261023\`.
  `build-report.json`, `kernel-build.log`, `kernel-verification.txt` and the initial failed
  `all` build log (`build-before-host-fix-20261023/kernel-build.log`) are retained there.

  | Artifact | Bytes | SHA-256 |
  |---|---:|---|
  | `mach.lancelot` | 15,144,488 | `17557e43eb9a4604149ba85ec2508bfcfd9964c9b1f1a39462f846ab1d9f30b1` |
  | `EFI/HackiOS/loader.efi` | 68,096 | `1bebd8de8835682b3460ed6b0d4482dab65f3359eac3924e214d46340b9f4bd3` |
  | `EFI/HackiOS/boot.xnup` | 15,144,664 | `61c3c2bfc4ca0e5219fc53c06cae6fd144347f7d2ad60e7c88599d0532318d36` |

- These checkpoint artifacts are **host-only, not deployed to ESP or boot**. No reboot was
  performed. The earlier four iOS/APFS payloads are already deployed on Ubuntu.
- Mirror-first dependency fetching now supports repeatable `--mirror HTTPS_PREFIX` and
  `HACKIOS_GITHUB_MIRROR`, defaulting to `https://ghfast.top`, then direct GitHub. All three
  archive hashes are pinned to published/original-download values. Offline tests verify
  mirror ordering, invalid/truncated/oversized range fallback and corrupted-archive rejection.

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

1. Read this file and `docs/LANCELOT.md`. Deployment is done; evidence and four payload hashes
   are recorded above. If desired, perform a read-only check via the script's remote `verify`
   action, then confirm `/dev/block/mmcblk0p49` is unmounted. Do not rerun the host deployment
   command. Expected files are under the **Ubuntu filesystem directory** `/hackios/20H392`;
   `/data/local/tmp/hackios-ubuntu` is only a temporary mountpoint and should not exist.
2. Do not flash or alter UEFI. No more UEFI source edits without asking the user first.
   The user authorized the already-pushed MSDC patch only. The boot partition must remain
   `E:\shuaji\Lancelot\boot.img` unless the user approves another test.
3. The XNU toolchain is installed and the build passed. If setup needs reconstruction, use
   the already-verified archives in Windows `local/build-deps-20261022` and install into
   WSL `/home/pencilqwq/hackios-lancelot/local-toolchain/theos` with
   `JOBS=4 bash tools/setup-xnu-offline.sh /mnt/d/AI_rom/iOSforLava/hackios-xnu-kernel/local/build-deps-20261022`.
   The pinned source repositories are already present and must not be discarded. The failed
   20261023 `.partial` files are not usable archives. If a fresh download is actually needed,
   prefer a mirror and verify against the recorded hashes before extraction.
4. The repaired kernel and checkpoint-5 payload are ready at the paths/hashes above. To
   rebuild after source changes use `JOBS=4 CHECKPOINT=5 bash common/build-lancelot-kernel.sh`;
   require make build exit 0 and the linked verifier. Do not package/flash the old `prebuilt/`
   as if it contained `xnu/lancelot.patch`. Keep the phone running Android during the build.
   Only with later boot-test authorization, retest checkpoint 5 on the known single CPU and
   keep normal runtime MMU enabled. The opt-in MMU probe is teardown-only, never an OS boot.
5. Implement a real read-only MT6768 MSDC/eMMC kernel driver or a validated same-core data
   loading method. The UEFI EFI app can read its ESP payload, but XNU stops the same-core
   UEFI services; the withdrawn A75/PSCI firmware-mailbox strategy is not the next test.
   Ubuntu is still an ext4 filesystem: the APFS containers are **files inside** it, not raw
   APFS at partition offset zero. A raw eMMC read alone is insufficient; add read-only ext4
   file lookup/extent mapping (or another explicitly designed file-loading path) before APFS
   root mount. The current images are already deployed and the full pre-iOS backup remains
   available; no repeat deployment is needed.
6. Test the on-disk iOS 16.7.16 (20H392) images with the 16.5 XNU. If userspace ABI fails,
   fetch the matching 16.5 firmware with the upstream downloader into a **separate** source
   directory; do not overwrite the preserved 20H392 extraction.

Only claim the image deployed after checking both the deploy log and JSON SHA-256 report.
Image files on Ubuntu do **not** mean XNU, native storage, or iOS userspace has booted.
