# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A fork of HackiOS that boots Apple's open-source **XNU `xnu-8796.121.2`** on a **Redmi 9 / lancelot (MT6768)** and runs the **iOS 16.7.16 / 20H392** userland. The phone is flashed only in `boot` (a Mu-Silicium UEFI with our EFI loader); the system image lives on an ext4 "Ubuntu" partition and iOS volumes in APFS containers on the same eMMC. The upstream project also supports POCO X5 Pro (redwood) and QEMU, but work in this checkout is lancelot-only.

Work currently in progress is the **Mali-G52 GPU** bring-up: power → feature registers → private MMU/DMA → fault recovery → pixels → Mesa/IOSurface/CoreAnimation/Metal. Touch, SMP, eMMC/MSDC, storage profiles and SpringBoard boot are already accepted baselines; do not re-run those regressions unless new evidence says they broke.

Sibling material outside the repo root:

- `D:\AI_rom\iOSforLava\LANCELOT-GPU-AI-HANDOFF-*.md`, `LANCELOT-GPU-PROGRESS-*.md`, `LANCELOT-GPU-REVIEW-*.md` — Chinese handoff/status documents; the newest one is the first thing to read.
- `D:\AI_rom\iOSforLava\iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` — the iOS input image.

## Non-negotiable operating rules

1. **Commit locally at milestones; never push to GitHub without the user's approval.** The user reviews each milestone and decides when it goes to the cloud. Earlier Mu pushes are historical and do not authorise new ones. Keep commits scoped to the GPU work and never sweep unrelated user changes into them. `gpu_complete=false` until real rendering is verified.
2. **Errors are for diagnosis, not just archiving.** Keep the failed log/result intact, then find the causal mechanism and fix it. A preserved failure with no root-cause investigation is an unfinished step.
3. **Evidence is append-only.** Every run lives in its own `attemptN/` directory. `build.py` and `package.py` refuse to overwrite an existing attempt; `analyze-runtime.py` refuses to replace a `runtime-result.json`. Never delete, rewrite or "correct" a failed run's logs, hashes, screen transcript or deployment ledger. A retry gets a new directory or a new attempt number.
3. **Never format, change GPT, or write large userdata files.** ESP is `/dev/block/mmcblk0p48`, Ubuntu is `/dev/block/mmcblk0p49` (GPT names are case-sensitive). Mount them only read-only unless staging the two ESP files through the guarded staging script.
4. **The native FASTBOOT endpoint is `116745f30503`, product `lancelot`, boot partition exactly 64 MiB.** The XNU diagnostic endpoint `lancelot-uefi` is read-only: never reboot/flash/write through it. Enter native FASTBOOT physically; scripts must verify `product: lancelot` and the Boot hash before flashing.
5. **Pinned artifacts must be hash-checked, never replaced:** Android Boot `E:\shuaji\Lancelot\boot.img` SHA-256 `b5de0152...f7e84`; recovery Mu `a23bdf6d...f4c7f7c`; touch profile `81a86d26...36763`; activation profile `86fbf42a...e867a0`. A per-test Mu hash is recorded in `current/candidate.json`.
6. **Ask the user before changing UEFI source.** The one approved UEFI fix (MSDC FIFO write) is already flashed.
7. **The Windows tree and the WSL tree are different states.** Hash-verified, selective sync (`build.py`) is the only supported way to move sources. Host-test PASS, build PASS and a matching source-audit do not prove the current Windows candidate was built or flashed. Never use an old attempt's audit for new source.
8. **Do not `git reset`, `checkout`, `clean` or otherwise discard the dirty tree.** It contains the user's in-progress GPU work plus their README/STORAGE-MODEL/Mu/MSDC/submodule work.

## Environment

- Builds and most tests run in **WSL `Ubuntu`**, workspace `/home/pencilqwq/hackios-lancelot`, kernel tree `xnu-src/xnu-8796.121.2`, theos at `local-toolchain/theos`.
- Windows authoring root: `D:\AI_rom\iOSforLava\hackios-xnu-kernel`.
- ADB/fastboot: `E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\`.
- A repository path containing spaces breaks XNU's makefiles; this one is fine.

## Commands

### Host tests (run from Windows; each script re-execs itself in WSL)

```powershell
python tools/test-lancelot-mali.py            # core/platform/client/export + quarantine/MMIO fixtures
python tools/test-lancelot-uefi-dma.py        # real firmware DMA adapter + shared core, GCC and clang
python tools/test-lancelot-uefi-gpu.py        # power/ID adapter + loader handoff
python tools/test-lancelot-gpu-receipt.py     # 432-byte receipt v3/v4 encode/decode/acceptance
python tools/test-lancelot-loader-pmic.py
```

These compile the **actual production sources** with `-fsanitize=address,undefined`, assert the source hashes did not change, and never touch a device. Other device-stage tests in `tools/` (`test-lancelot-touch-*.py`, `-smp`, `-cache`, `-pwrap`, `-sspm-*`, `-payload`, `-log`) follow the same pattern.

Single C fixture (inside WSL):

```bash
clang -std=c11 -DLM_HOST_TEST -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I xnuloader -I xnu/files/bsd/dev \
  tools/mali-tests/core_test.c xnu/files/bsd/dev/lancelot_mali_core.c -o /tmp/core
/tmp/core              # normal suites
/tmp/core quarantine   # quarantine / negative-path suites
```

### Full lancelot kernel + ESP payload

```bash
# inside WSL; runs the lancelot host-test suite first, then make build (exit status must be 0)
JOBS=4 bash common/build-lancelot-kernel.sh
# first-time setup only
xnu/apply.sh && xnu-deps/setup.sh
# pack the ESP pair
KERNEL=out/lancelot/mach.lancelot bash common/build-lancelot.sh boot
bash xnuloader/build.sh lancelot
```

`common/build-lancelot-kernel.sh` refuses to build if any `xnu/files/...` file differs from the actual kernel tree, and `loader_return.c` is checked separately. `xnu-build.sh ... build` is used deliberately instead of the upstream `-k` recipe: for lancelot, a real exit status is required.

### GPU attempt pipeline (Windows ↔ WSL)

```powershell
# 1. selective audited sync + GPU-only tests + formal loader/XNU build into attempt N
wsl -d Ubuntu -- python3 /mnt/d/AI_rom/iOSforLava/hackios-xnu-kernel/local/lancelot-gpu-20261026/private-dma/build.py --attempt 11

# 2. pack boot.xnup, verify hashes, update the sole deployment candidate (does not deploy)
python local/lancelot-gpu-20261026/private-dma/package.py --attempt 11 `
  --previous-deployment local/lancelot-gpu-20261026/private-dma/attempt10/deployment.json `
  --mmu-fault

# 3. decode the same-boot receipt from the finished capture (never overwrites)
python local/lancelot-gpu-20261026/private-dma/analyze-runtime.py --attempt 11
```

`build.py` asserts the WSL sources match `local/lancelot-gpu-20261025/resume-deploy/source-audit.json`, copies only its explicit path list, mirrors `xnu/files/*` into both the overlay and the real kernel tree, then updates the ledger. `package.py` reads WSL artifacts over `\\wsl.localhost\Ubuntu`, builds the XNUP with `xnuloader/mkpayload.py`, validates it with `tools/test-lancelot-payload.py`, snapshots `previous-current/` and `previous-sources/`, and writes `local/lancelot-touch-hardware-20261024/current/` plus `candidate.json`. If a build fails, keep its directory and use the next attempt number.

### Deployment and capture

Deployment is driven by a per-attempt ledger, not a single current script. The sequence, as recorded in `attemptN/deployment.json` and `attemptN/stage-esp.sh`:

1. Verify Android native mode over ADB: root, `ro.product.device=lancelot`, `sys.boot_completed=1`, Android boot-id, ESP/Ubuntu block symlinks, pinned Boot hash, both profile hashes, and that ESP/Ubuntu are not otherwise mounted.
2. Push `loader.efi` + `boot.xnup` to `/data/local/tmp/...`, then run the generated `stage-esp.sh`: assert the old ESP hashes, write `.new` + compare, atomically `mv`, sync, remount read-only and re-read all three (including `EFI/BOOT/BOOTAA64.EFI` fallback).
3. `adb reboot bootloader`, confirm `product: lancelot` and `partition-size:boot=0x4000000`, `fastboot flash boot <pinned test Mu>`, `fastboot reboot`.
4. Capture logs while the phone boots (see below).
5. Return to Android: `python local/lancelot-gpu-20261025/restore-android.py --log <new-file>` after the user physically enters native FASTBOOT. It verifies the fixed Boot hash and both flash stages, then waits for Android.

```powershell
# follow firmware LLOG1 into XNU KLOG2 over fastboot INFO for a bounded duration
python tools/read-lancelot-log.py --runtime --quiet `
  --fastboot E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\fastboot.exe `
  --serial lancelot-uefi --out <attempt>/runtime.log --duration 180
```

`runtime.log` is the decoded stream; `runtime.log.fastboot` is the raw transport transcript; `reader.console.log` records capture progress. The reader validates LLOG1/KLOG framing, detects boot-ID changes, reports ring overruns, and exits non-zero on transport failure or an empty capture.

## Architecture

### Boot chain

```
Android FASTBOOT ──flash boot──► Mu-Silicium UEFI (MT6768, patched MSDC FIFO)
                                    │
                                    ▼
                     ESP /EFI/HackiOS/loader.efi  (xnuloader, resident EFI app)
                       ├─ stays in memory: USB fastboot log transport (LLOG1),
                       │  watchdog, receives the kernel log, returns to UEFI
                       ├─ loads ESP /EFI/HackiOS/boot.xnup  (XNUP: Mach-O kernel,
                       │  cmdline, ubuntu-boot marker)
                       ├─ optional GPU power/feature/private-DMA probes before handoff
                       ▼
                     XNU (BCM2837 config + lancelot platform)
                       ├─ GICv3, PSCI SMP, timer, eMMC/MSDC, MUSB USB, native touch (NT36672)
                       ├─ APFS: iOS root/OS/App containers on the Ubuntu partition;
                       │  /private/var and /tmp in RAM
                       ├─ AMFI/Sandbox stand-ins, minicrypto, IOSurface/framebuffer/key-store
                       ├─ lancelot_mali: read-only GPU SPM/register observation (no GPU service)
                       ▼
                     iOS 16.7.16 launchd → … → backboardd → SpringBoard → apps
                       └─ kernel returns to the loader with its log (KLOG2)
```

The loader writes **no filesystem metadata**; firmware logs go to the MUSB LLOG1 RAM ring and XNU logs to KLOG2. Both are read back over the diagnostic fastboot endpoint. The phone keeps running after a capture ends.

### Repository layout

| Path | Contents |
|---|---|
| `common/` | `build-lancelot-kernel.sh` (tests + kernel build), `build-lancelot.sh` (ESP payload), device profiles |
| `xnuloader/` | `loader.c` and lancelot headers: `lancelot.h` (platform/boot/receipt), `lancelot_ubuntu.h`, `lancelot_pmic.h`, `lancelot_gpu_prepare.h`, `lancelot_gpu_features.h`, `lancelot_gpu_dma.h`, `lancelot_gpu_dma_native.h` |
| `xnu/` | `redwood.patch` (Apple files), `lancelot.patch` (MT6768 checkpoint overlay), `files/` (new files at their XNU-tree paths), `apply.sh`, `export.sh` |
| `xnu/files/bsd/dev/` | lancelot drivers: USB, eMMC/MSDC, touch, SPI, SMP, cache, PMIC, pwrap, SSPM, Mali |
| `xnu/files/iokit/Kernel/` | `LancelotMali.cpp`, user/client ABI headers |
| `tools/` | host tests, Android collectors, receipt codec, log reader, payload validator, flash utilities |
| `tools/mali-tests/` | C/C++ fixtures compiled against real production sources (core, platform, client, UEFI adapter/lifecycle, receipt) |
| `local/` | gitignored evidence and scripts; authoritative runtime history. `local/lancelot-touch-hardware-20261024/current/` is the only deployment candidate |
| `uefi/` | Project Silicium patches (read-only from our side) |
| `qemu/`, `wifi/`, `prebuilt/` | upstream port pieces, unchanged for lancelot |

### Dual-tree build model

The Windows checkout is where sources are edited. The WSL checkout is the build environment, and its `xnu-src/xnu-8796.121.2` is the tree `make` actually compiles. `build.py` is the bridge:

- asserts the WSL copy of every file in the source ledger still matches its recorded SHA-256 (and that the actual kernel tree copy matches `xnu/files/`),
- refuses unexpected new WSL paths,
- copies an explicit file list from Windows → overlay (and `xnu/files/*` → kernel tree), saving `previous-sources/` first,
- writes `source-audit.json` and updates the shared ledger,
- runs the GPU test suite, builds the loader and XNU, verifies the Mach-O checkpoint, and records bytes/hashes.

Consequently **a host test green is about the Windows source; a build green is about the WSL copy; neither is a hardware result.**

### XNU overlay model

`xnu/apply.sh` fetches tag `xnu-8796.121.2` into `xnu-src/`, applies `redwood.patch`, applies `lancelot.patch`, then copies `xnu/files/` over the tree. `xnu/export.sh` regenerates both forms from the whole tree. Known traps:

- `export.sh` exports **the whole tree**, so it can silently roll back someone else's files if the tree is behind the patch; reconcile before exporting.
- Do not run `export.sh` with `lancelot.patch` applied — it would be folded into `redwood.patch` and applied twice.
- `common/build-lancelot-kernel.sh` treats any drift between `xnu/files/` and the actual tree as an error; fix the tree, don't bypass the check.
- `xnu-build.sh` normally ends with `check_all_exports` noise on upstream builds; lancelot builds use the `build` target and require a clean exit.

### GPU bring-up subsystem

Staged, independent boot args (all must be present together for the current candidate):

```
cpus=8 lancelot_touch=3 gpu=0 lancelot_mali=1
lancelot_gpu_power=1 lancelot_gpu_features=1 lancelot_gpu_dma=1 [lancelot_gpu_fault=1]
```

`gpu=0` is the Adreno/redwood path and stays off. `lancelot_mali=1` is **read-only** XNU observation: print the same-boot receipt, read-only SPM/AO, prove MFG domains off. It does not create a GPU service, WACS2 exclusive owner or SSPM backend; fake providers and `lancelot_mali=3/4` are forbidden.

Firmware (loader) side:

- `lancelot_gpu_prepare.h` — real MFG power-domain enable/disable, cache → shader/tiler/L2 PWROFF, READY polling, soft reset, final idle proof.
- `lancelot_gpu_features.h` — 42 read-only G52 registers, two stable passes, protected access guards.
- `lancelot_gpu_dma.h` — adapter around the shared core: leases 2 data pages + 4 page-table pages (CPU/PA/write-back checks), maps only job/output, PoC clean/invalidate, restricted MMIO transport, sentinels, cleanup and receipt emission.
- `lancelot_gpu_dma_native.h`, `lancelot_pmic.h` — native platform helpers.

Shared core (compiled both into the loader tests and into XNU):

- `lancelot_mali_core.c/.h` — AS root/MEMATTR/TRANSCFG and job NEXT programming with strict readback, `JOB_IRQ_JS_STATE` CURRENT/NEXT sampling, stale-DONE/busy rejection, reset/idle/quarantine logic.
- `lancelot_mali_private_dma.h` — one private WRITE_VALUE transaction. `0x4c4d4732` at output offset 128; an unmapped guard VA (`0x02003080` in the fault candidate) is used for controlled translation-fault acceptance. Six pages are leased; unknown DMA retains all backing and refuses ordinary kernel handoff.

XNU side: `lancelot_mali.c` (read-only resume + receipt), `lancelot_mali_platform.c` (power), `lancelot_mali_observe.c`, `lancelot_mali_pmic.c`, `lancelot_sspm_pmic.c`/`lancelot_sspm_boot.c`/`lancelot_sspm_selftest.h` (SSPM protocol, no claimed exclusive backend), `iokit/Kernel/LancelotMali.cpp` + client ABI/logic headers.

**Receipt:** fixed **432 bytes / 96 little-endian u32 words**, emitted by the loader (UEFI frame) and again by XNU (same boot, same CRC). Version 3 carries first-error/phase/transport facts; version 4 adds bit 92 fault-test mode, 93 expected-fault, 94/95 expected fault VA. `tools/lancelot_gpu_receipt.py` decodes and enforces per-field acceptance. Both frames must match in boot, words and CRC. A 256-entry (candidate: 512) trace ring lives in the RAM log; the screen prints a summary only — the last trace entries may be lost and that does not invalidate the receipt.

**Interpretation rules that have already caused mistakes:**

- `error=-5` is expected in the controlled-fault run; the original EIO must be preserved. A fault PASS is never an ordinary DMA or rendering PASS.
- `job reset=0` means "no error-recovery reset", not "no reset"; the normal-path final core shutdown reset still runs.
- A missing log tail or lost trace entries is not evidence of zero register values or a PASS.
- Normal DMA success (`submitted=1 sequence=1 value=0x4c4d4732 guards=1`) proves MMU/DMA only, not pixels, shaders or desktop acceleration.

### Hardware facts

- MT6768, Mali-G52 MP2, GPU ID `0x74021000`, MMU features `0x2823` (35-bit GPU VA / 40-bit PA), AS present `0xff`, JS present `7`, two shader cores.
- eMMC/MSDC, not UFS. GPT: `esp` / `Ubuntu`. iOS volumes: `/hackios/20H392/touch-hardware-20261024/root.apfs` and `/hackios/20H392/sb-activation-20261024/root.apfs`.
- ESP layout: `EFI/HackiOS/loader.efi`, `EFI/HackiOS/boot.xnup`, plus `EFI/BOOT/BOOTAA64.EFI` (the same loader).
- Any `fastboot` result with `FAILED` in stdout/stderr is a failure even when the exit code is 0; both `Sending` and `Writing` must say `OKAY`.

### Evidence and acceptance model

Each run directory contains `deployment.json` (every ADB/fastboot command, output hash, exit code), `deployment.log`, `stage-esp.sh`, `reader.console.log`, `runtime.log`/`.fastboot`, build/audit/test JSONs, and `runtime-result.json` with SHA-256s of all evidence. `analyze-runtime.py` sets independent flags rather than one verdict:

`receipt_verified`, `dma_hardware_verified`, `expected_mmu_fault_hardware_verified`, `xnu_readonly_resume_verified`, `desktop_verified`, `rendering_verified`, `gpu_complete`.

Do not let one flag imply another, and do not treat process liveness (SpringBoard running) as a visible desktop or GPU acceleration. `docs/checks.md` ("How our own checks lie") is required reading before writing any new acceptance check: every skipped item needs its own counted column, an instrument must be shown able to fire, and a diagnostic must first be validated on a known-good input.

## Conventions

- Use the next free `attemptN` number; never reuse or overwrite. Retries and re-captures get their own subdirectory (`live-log-retry1/`, `capture2/`, …), each with its own result JSON.
- Every artifact, log, source snapshot and ESP file is identified by SHA-256 and byte count in the result JSON.
- Status/handoff documents are written in Chinese at `D:\AI_rom\iOSforLava\`; detailed design notes are in `docs/LANCELOT-*.md`.
- Preserve the current bootargs baseline unless the experiment explicitly changes one flag. Do not remove `cpus=8`, `lancelot_touch=3`, `gpu=0`, `lancelot_mali=1`, `lancelot_gpu_power=1`, `lancelot_gpu_features=1`; `package.py` asserts each appears exactly once.
- A GPU capture is finite (180 s used in recent runs, 60–900 s allowed); only one log reader may run at a time. Unknown DMA state means retain resources and do not hand off to XNU.
- `local/` is ignored by git; the handoff trail there is the project's history. Treat it as read-only unless a new attempt directory is being created.
