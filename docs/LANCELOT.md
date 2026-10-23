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

Raw Android evidence belongs in the ignored `local/` directory. Verified partition backups
are in `D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022`:

- `android-boot.img`: 64 MiB; matches `E:\shuaji\Lancelot\boot.img`.
- `esp-before.img`: 1 GiB; SHA-256 `6f1e5c78166360f661c113d638c90e000e114c61ffe3f56b2ecf06c691713510`.
- `Ubuntu-before-ios.img.gz`: the entire 33,285,963,776-byte partition, compressed to
  2,559,218,342 bytes. Decompressed/source SHA-256:
  `5c5327b280c1197065f2a82489a1959405e6782dfc0f5cc8661b96872a6ac031`.
  Gzip integrity, byte count and before/after source hashes all verified.
Use `tools/collect-android-port.py --adb PATH --serial SERIAL --out NEW_DIRECTORY` to repeat collection.

## EFI/XNU boot

```sh
JOBS=4 bash common/build-lancelot-kernel.sh
```

Produces `out/lancelot/EFI/HackiOS/loader.efi` and `boot.xnup`. The EFI app loads the XNUP
from its own ESP and allocates its memory through UEFI. **No UEFI filesystem writes**:
logs go to the existing MUSB LLOG1 RAM ring, not FAT metadata or raw log partitions.
The lancelot build explicitly selects MTK even if GOP's buffer moves. It does not use the
redwood boot shim, hard-coded payload reservation, Qualcomm USB protocol or UFS registers.
The generic `common/build.sh lancelot` entry delegates to this path.

Default `boot` continues through XNU initialization without deliberate checkpoint/timer
stops. Use `checkpoint` or `sweep` only to investigate an actual startup blocker or collect
targeted logs: `checkpoint` defaults to assembly checkpoint 1 in the packing-only script;
`sweep` advances through 1, 2, 3, 4, 5 and 10. **Current strategy: the firmware's boot CPU only**,
with no `bootcpu=6`, PSCI preflight or secondary-core start. `cpus=1` does not cause
PSCI affinity queries for other cores. The loader refuses secondary CPU and firmware
storage mailbox requests on lancelot rather than silently taking the withdrawn path.
`selftest` exercises the same-core context round trip and is not yet validated on MTK.
These tests are not complete iOS boots.
The packer defaults to repaired `out/lancelot/mach.lancelot`, produced by the build wrapper.
Build success is not hardware validation. Continue normal execution and inspect actual
failures; enable diagnostic checkpoints only when needed. Root mount and userland remain
dependent on native storage support.

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
loader's Qualcomm USB API. Loader logs are appended to this ring for USB retrieval.
UEFI USB remains a boot-services facility; running XNU on the same
core suspends its callbacks until the kernel returns.

Capture with Windows platform-tools or Linux fastboot:

```sh
python tools/read-lancelot-log.py --fastboot /path/to/fastboot --out logs/lancelot.log
```

The reader validates the advertised byte ranges and hex framing before advancing a cursor.
Host tests reject truncation, extra data and a bare OKAY with no firmware log.

### UEFI write defect and scope

First hardware test loaded the payload and printed the memory map, then blocked while
writing `last.log`. Android reported corrupt FSINFO/FAT; the complete original ESP was
restored and its hash verified. Root cause in Mu-Silicium's `MsdcFifoWrite`: the 32-bit
FIFO write read `*ByteBuffer` (one byte), advancing by four, producing byte/zero corruption.
`uefi/mt6768-msdc-fifo.patch` repairs the actual word load. `tools/test-msdc-fifo.py`
compiled the real function: old source fails at length 4, fixed source preserves alignments
0..7 and lengths 0..4096 under ASan/UBSan.

The user authorized this existing fix and its use; it is committed and pushed as
Mu-Silicium `251bd657`. The resulting image SHA-256 is
`a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c`.
**Ask the user before any further UEFI source modifications.** Loader/XNU work continues
without more UEFI changes. The source tree's older unrelated local edits were not committed.

### iOS image status

16.7.16 `root.apfs` and `system.apfs` built successfully. apfsck reports both clean;
the kernel APFS reader checked 77 and 62,393 entries against their source trees respectively.
A 4 MiB `flashboot.apfs` (stage0 + mount points) also passed readback checks.
`tools/prepare-lancelot-flash.py` converted the original local DMGs to consecutive APFS
containers: root 5,635,047,424 bytes, OS cryptex 2,743,074,816 bytes, App 10,485,760 bytes.
Output SHA-256: `0c5685be9960495afccbdd7f0fd257b35620931f7b456ae793c2a55c7241b51a`.
This image and the three prepared APFS files were deployed to the existing Ubuntu ext4
directory `/hackios/20H392` on 2026-10-23. All four SHA-256 checks passed after transfer and
after a read-only remount, then Ubuntu was unmounted; Android remains running. See
`docs/LANCELOT-HANDOFF-2026-10-23.md` for logs, hashes and the latest reboot authorization.
These are files inside ext4, not a raw APFS partition; hardware boot/ABI compatibility is unverified.
If 16.7 userspace is incompatible, the user authorizes the upstream `ios165/fetch.sh`
download of matching 16.5. Keep the two versions in separate source directories.

The experimental A75 strategy halted after `lancelot: starting secondary MPIDR 600`.
The user reports the earlier same-core path reached further. A75 boot and its dependent
firmware eMMC mailbox strategy have therefore been withdrawn; `common/build-lancelot.sh ios`
now refuses that unsafe/incomplete recipe before replacing output files. The prepared iOS
images are retained. Continue the known single-core path, then implement validated RAM/native
eMMC loading. The same-core build provides a 30-second USB capture window before handoff,
and prints the physical entry, boot-args and return-stub addresses before the actual branch.
No further UEFI source change is involved.

### Single-core hardware result

The single-core test on the authorized UEFI image returned successfully from checkpoints
**1, 2, 3 and 4**. Actual firmware: EL1, Cortex-A55 MIDR `0x411fd050`, timer 13 MHz,
GOP 1080x2340 at `0x7cdb0000`. XNU's claimed contiguous window is
`0xc0000000..0x13c000000` (1984 MiB), payload at `0x13c000000`.
The original prebuilt stalled at checkpoint 5. On 2026-10-23, the rebuilt kernel with
`xnu/lancelot.patch` **returned successfully from checkpoint 5 on hardware**, after enabling
MMU. Log: `local/lancelot-cp5-hardware-20261023/xnu-cp5.log`, boot ID `0000000003c7f618`,
`STOPPED AT CHECKPOINT 5`, `result=checkpoint 5 0`. Its entry PA is `0xc34464a8`.
Checkpoint 10/C entry has also subsequently passed as recorded below; iOS userspace has not run.

The first checkpoint-10 test did not return (`local/lancelot-cp10-hardware-20261023/`).
Its early C return helper could parse `bootcpu` before `PE_init_platform()` initialized
`PE_state.bootArgs`. A guarded parse and regression test now cover this case; the rebuilt
candidate is in `local/lancelot-cp10-pefix-20261023/`. **Hardware retest passed** with boot ID
`0000000003b76e9b`, `STOPPED AT CHECKPOINT 10` and `result=checkpoint 10 0`. The successful
kernel SHA-256 is `8baa2dcc630db2a0999729fef409f19eca83e12f50d36e90fac4fb3f0004ba2f`.
Its pre-return SCTLR `0x3454593d` confirms MMU/caches were enabled while executing C.
Continuous execution through CPU/platform initialization is next; checkpoint 11 is available
if a real error needs localization. The build wrapper refuses a stale/different
`loader_return.c` in an existing XNU workspace.

The pinned prebuilt actually uses **4 KiB kernel pages**, not 16 KiB: disassembly has
2 MiB L2 blocks, 4 KiB table spacing and `_sysreg_restore` TCR `0x22a51a251a`
(TG0=0/TG1=2, T0SZ=T1SZ=26, IPS=40-bit). The loader checks TGran4 accordingly.
The next checkpoint-4 diagnostic records SCTLR/TCR/TTBR0/TTBR1/VBAR before restoring
firmware state and walks the kernel's tables read-only, with bounded physical addresses.
Hardware checkpoint-4 readback confirms correct translations for the physical entry,
kernel virtual entry and exception vectors. The standalone loader MMU probe also completed
`stage=3` with zero ESR/ELR/FAR using those exact tables and the prebuilt's SCTLR.
That probe proved a minimal MMU round trip; the later repaired XNU checkpoint-5 test passed
separately. Neither is an iOS boot.

**MMU policy:** normal XNU/iOS execution must retain the MMU and caches. Disabling them
is only a diagnostic teardown/handoff, followed by restoring the firmware's MMU; it is
not a boot workaround. The standalone probe is now opt-in (`mkpayload.py --mmu-test`),
allowed only after checkpoint 4 without RAM disks. Normal checkpoint/sweep payloads do
not run it. The planned MMU-only-disable comparison was cancelled before device deployment.
The next work is a fix to the checkpoint's teardown code and normal post-MMU execution.
`xnu/lancelot.patch` now loads the return target before teardown and synchronizes
MMU/cache state on the taken checkpoint/exception path. The actual checkpoint macro
compiles, and a disassembly test confirms the ordinary boot branch bypasses the complete
teardown sequence. The rebuilt kernel SHA-256
`17557e43eb9a4604149ba85ec2508bfcfd9964c9b1f1a39462f846ab1d9f30b1` contains this repair and
passed checkpoint 5 on device; the checked-in prebuilt remains the older kernel.

### Rebuild and deploy the repair

The build uses WSL's Linux filesystem and four jobs initially; the machine had about
8.8 GiB free WSL RAM when preparation started. Keep the phone in Android while preparing.
These commands do not flash anything:

```sh
# Can run with Windows Python when WSL's GitHub downloads are slow:
python tools/fetch-xnu-toolchain.py --out local/build-deps-NEW
# In the WSL workspace (adjust archive path to the Windows checkout):
python3 tools/prepare-xnu-sources.py "$PWD"
bash tools/setup-xnu-offline.sh /mnt/d/AI_rom/iOSforLava/hackios-xnu-kernel/local/build-deps-NEW
JOBS=4 bash common/build-lancelot-kernel.sh
```

Archives are pinned to the upstream Theos toolchain/16.5 SDK and macOS 13.3 SDK for
pre-generated DriverKit headers. The complete Windows `local/build-deps-20261022` archives
passed SHA-256 and XZ verification and were installed successfully in the WSL workspace.
Use those before downloading again. Fresh downloads prefer `https://ghfast.top` (override
with `--mirror HTTPS_PREFIX`, repeatable, or `HACKIOS_GITHUB_MIRROR`), then direct GitHub.
Exact ranges and reference hashes are checked: only macOS SDK has a publisher hash; the
other two reference hashes are from the verified direct-origin download. No Apple IPSW or
phone firmware is downloaded by this step.

`build-lancelot-kernel.sh` requires XNU's `make build` target to succeed and verifies
checkpoints 1–5 in the linked machine code before packaging. A compile/link failure stops
packaging even if an old kernel is present. Up-to-date repaired kernels can be reused by
make. The full `all/config` target still reports DriverKit RPC export/symbolset failures
from upstream's incomplete `iig` stubs (`xnu/README.md`); the checkpoint-only build does not
claim to fix those or produce a complete DriverKit SDK. `mach.lancelot` is a local artifact.
Device reboot/flash tests are now authorized. Deployment uses
`tools/deploy-lancelot.py --adb PATH --serial SERIAL --files DIRECTORY` with readback hashes,
and the already-authorized UEFI image only. No Ubuntu partition write is needed for this test.

Full iOS boot also needs a low-memory storage strategy (the system image exceeds the usable
contiguous RAM), MTK input and watchdog integration, and verification of 16.7 userspace with
the 16.5 kernel. Mali GPU, MediaTek Wi-Fi/audio and native eMMC support are not provided by
the redwood drivers.
