# Lancelot boot-loop fix / transfer upgrade handoff — 2026-10-23

Latest evidence and next candidate: `LANCELOT-SERVICE-LOG-2026-10-23.md`.
Runtime USB now exports user-space logs; real mediaserverd SIGSEGV and backboardd
spawn EACCES loops have been observed. Stable desktop remains unverified.

## Goal and current policy

- User supplied `D:\AI_rom\iOSforLava\IOSnewproblem.md`: Apple-logo boot stalls while
  wifid locale and memory lines repeat. Combine a targeted boot-profile fix with the
  already-built transfer upgrade, test hardware, and push only after the combined result passes.
- Continuous boot is required. Checkpoints/timed returns are only for specific fault/log
  investigations. UEFI source modifications still require confirmation and individual commits.
- Persistent hardware read/write remains a subsequent requirement; current storage is read-only.
- The previous state/history and partition backups are documented in
  `LANCELOT-HANDOFF-2026-10-23.md`. This file is the focused handoff for the new test.

## Diagnosis and targeted changes

The supplied wifid output is a periodic locale query with no networks. The memory report
shows about 1536 MiB available, well above the printed pressure thresholds. Neither line
identifies a crash or proves wifid/OOM is the blocker.

Two concrete build defects were found:

1. `userland/mkjobs.py` reused `name` for the `RWA_PACKED16` environment-variable loop.
   In the on-demand job loop, this overwrote the plist filename: 389 jobs were repeatedly
   written into one file named `RWA_PACKED16`, which launchd does not load. The old output
   claimed success but contained only 25 plists. A regression test reproduces the defect;
   `common/patches/userland-job-env-name.patch` fixes it, with tests for the environment
   variable both absent and present.
2. The fresh RAM data-volume migration seed still named iOS 16.5 build `20F66`, whereas
   the imported/booted firmware is iOS 16.7.16 `20H392`. The new profile matches both
   migration build fields to the verified SystemVersion/BuildManifest. It edits only the
   packaged fresh-data template, not persisted user data or the source IPSW.

`tools/prepare-lancelot-boot-profile.py` creates a separate profile directory. It regenerates
full-root dependencies with `--rest --root-volume`, preserves the explicit eager core set,
leaves the other jobs on demand, and retains wifid. It replaces the interactive demo/CPU
stress job with a low-rate boot observer. The original bootstrap files remain available.

The new profile has **414 plists**. Its APFS image is 4 MiB; apfsck and the kernel APFS
reader verified all **466 entries** against the profile directory.

## Diagnostics

- `tools/lancelot-bootdiag.c`, built statically with libmini, reports native width/clock/batch,
  errors/counters and selected process PID/state/start-time every 30 seconds. It never halts
  the OS. It also reports bounded error/migration/hook excerpts from the window-server logs.
- SpringBoard/backboardd os_log and stdout/stderr go to volatile RAM files:
  `/private/var/log/hackios/SpringBoard.log` and `backboardd.log`. The observer truncates these
  dedicated files above 4 MiB to bound diagnostic RAM consumption.
- The old iOS 16.5-addressed graphics/activation hook guards have **not** been weakened for
  16.7. If output says hooks are skipped, use the actual new addresses/signatures and validate
  them before porting a hook. Do not blanket-disable the checks.

## Combined candidate

Windows artifact directory:
`D:\AI_rom\iOSforLava\hackios-xnu-kernel\local\lancelot-combined-bootfix-20261023\`.

| Artifact | SHA-256 |
|---|---|
| Kernel | `8b7c530a432097dfb5c4a5f39a22f6058a77e120d5a31621c5b419e4447e45d0` |
| Loader | `850432846c53983ab68ed92ae90e4a815e615f696ab3494729489a312ddc3bcc` |
| XNUP | `25b120b39cb8c71b3c29aa42f292c114c81cd10045a2e8ca57ee7116333f88f8` |
| New root.apfs | `959e4af6f08b1163c6159484600fb89b85915787608a8540fb65e17756c944b9` |

- Transfer implementation is local commit `a40b32b`: counted CMD23/CMD18 PIO, verified
  8/4/1-bit and HS52 negotiation/fallback, handoff v2/96 bytes. Full host tests and kernel
  build passed. See `LANCELOT-TRANSFER-UPGRADE.md`; hardware validation is pending.
- New root path on Ubuntu: `/hackios/20H392/bootfix-20261023/root.apfs`. It has already been
  streamed directly into Ubuntu and verified before and after a read-only remount;
  `profile-deployment.log(.json)` records success. The partition was unmounted.
- Flashboot stays `/hackios/20H392/flashboot.apfs`, hash `14485e7c...`; large iOS containers
  are unchanged. The loader is pinned to the new root path/hash. Do not mix it with an
  undeployed/different profile.
- Use the same previously authorized MSDC-fixed UEFI image, SHA-256
  `a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c`.
  No UEFI firmware source changes were made for this fix.

## Hardware validation and next work

The bootfix combination was deployed and booted (firmware boot ID `00000000045630c4`).
Its UEFI capture has 3514 bytes and confirms the root hash, checkpoint 0 and a 384 MHz
MSDC source clock. The user reported continued user-space scrolling, lsd CPU-monitor
messages and unsupported RedwoodAudio properties; a stable desktop is not confirmed.
The lsd executable lacks `com.apple.private.kernel.override-cpumon`; the warning restores
default limits, rather than killing the process. Audio property messages alone do not
prove an audio-server restart. Firmware log timeouts after CPU handoff do not prove a hang.

The next candidate adds XNU runtime USB logging; see `LANCELOT-RUNTIME-LOG.md`.
Require actual evidence
of negotiated transfer settings, zero sustained I/O errors, expected core processes and
progress beyond the former Apple-logo stall before calling the combined fix passed/pushing.
Host compilation alone is not that result.

If it still stalls, capture the new `bootdiag` lines and SpringBoard/backboardd errors first.
Next candidates are version-specific injection hooks or a clearly named missing service;
the wifid timer is not a reason to remove that daemon. After stable boot, continue dedicated
writable-data image support, MTK input/display integration and performance validation.
