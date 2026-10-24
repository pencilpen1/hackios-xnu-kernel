# Lancelot 20H392: first service-log boot and guarded media candidate

后续启动路径诊断见 `LANCELOT-SB-STARTUP-2026-10-24.md`；其最新部署/实测记录优先。

## Result and current state

The previously deployed `service-log-20261023` candidate has now booted on the
phone. The original MSDC-fixed UEFI was hash-verified, flashed to `boot`, and
rebooted; no firmware source was changed. The phone is currently in continuous
XNU boot with the `lancelot-uefi` **KLOG2** interface, not Android.

**Final continuation state:** both the media-fix candidate and graphics-mode
control payload were deployed/readback-verified and boot-tested for 900 seconds
each. The phone is now in **continuous graphics-mode XNU**, showing an Apple
logo/loading screen. KLOG2 responds after capture; a stable lock screen/desktop
has not passed. A prior USB physical reconnect failure remains unresolved.
No GitHub push is authorized by these partial results.

Evidence: `local/lancelot-service-firstboot-20261024/`.

## Hardware evidence from the 900-second capture

- Boot ID: `8000000003ca8167`; reported 1,235,462 KLOG2 runtime bytes and
  1,243,076 bytes overall. No reported CRC failures or cursor gaps.
- Loader verified the actual service-log profile SHA-256 `8089e522...`.
- **backboardd successfully spawned as PID 13, UID 501**. No backboardd EACCES
  appears in this capture, versus 49 recorded failures before the permission fix.
- The five observed process samples retain SpringBoard PID 8/start 56.128524 and
  backboardd PID 13/start 56.450015. The later lsd samples retain PID 28/start
  97.885818. No SpringBoard exit 255 or lsd crash is established in this capture.
  These samples do not prove a functional/stable lock screen or desktop.
- Five eMMC samples: width=8, 48,000,000 Hz, batch=128, errors=0, fallback=0.
  Last counters: sectors=3,251,210; commands=96,164; PIO time=165,496,378 us.
- Kernel `console_write` observation works: native storage messages, 705 lsd CPU
  monitor entitlement denials, and **53 fatal mediaserverd user faults** are now
  captured. All recorded media faults have PC=0 and LR=0x1aad3058c.
- Keep CPU entitlement checking intact. The lsd denials alone are not evidence of
  a terminated process; they reset monitoring parameters to defaults.
- CoreData still initially reports absent knowledgeC.db and IdentityServices/PDS
  paths. PDSCDCache explicitly reports successful recovery. No corruption or
  justification for deleting a database has been established.
- File chunks were observed for SpringBoard, backboardd and mediaserverd only.
  Do **not** claim that all seven detailed streams were exercised. The other
  four may be empty/unstarted; further observer checks are needed.
- Window-server/SpringBoard hooks still reject old 20F66 addresses. Especially
  `window server hooks INCOMPLETE` remains; this candidate does not adapt them.

Important files: `boot-manifest.json`, `boot-flash.log`, `capture-console.txt`,
`services-first-boot.log(.fastboot)`, `services-first-boot.analysis.json`, and
`hardware-observation.json`. Keep the raw log; console output interleaves with
file markers and launchd flushes earlier timestamps late.

## Exact media symbolication, not an audio-property guess

CoreFoundation backtraces provide the slide independently of media-hook logs:

```text
CoreFoundation UUID: 55B9BA28-4C5C-3FE7-9C47-4983337D6E83
cache __TEXT base:   0x186de8000
runtime frame:      0x19cff9e10 = runtime image base + 106000
shared-cache slide: 0x161f8000
runtime media LR:   0x1aad3058c
unslid media LR:    0x194b3858c
exact function:     CI::GLContext::init() + 0xcc
CoreImage UUID:     CF8D2341-91CA-31C0-9FEE-11721D31D710
```

The instruction at LR-4 is `blr x8`; the preceding code loads a function pointer
from the GL table and sets the parameter to 0xb71 (GL_DEPTH_TEST). The subsequent
calls disable GL_SCISSOR_TEST, GL_DITHER and GL_CULL_FACE. This is a NULL
`glDisable` call during CoreImage GL initialization, not a fault inferred from
RedwoodAudio's unsupported-property probes.

The matching local-symbol file was selectively extracted from the existing
20H392 cryptex on WSL, without changing system/phone files. 7z's expected ZBM
unsupported-method exit left the resource fork and decmpfs metadata; `unzbm`
reconstructed the file successfully. It is 592,101,376 bytes, SHA-256:
`e763b0eadab7aaa283e3075fc4ebcd9e824d35c834101e4dd99554265126b308`.
Its UUID **51CCBA45-1FC2-3F19-AED0-47050BDF009A** matches the main cache header.

WSL symbols:
`local/lancelot-service-firstboot-20261024/symbols/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.symbols`.

`tools/inspect-dyld-cache.py` reads split mappings, v2 pointer rebasing, the
ObjC v16 global selector base, class/metaclass method lists, and UUID-matched
local symbols. Eight synthetic regression tests pass. Evidence includes:
`fault-exact-symbolication.json`, `fault-callsite.disasm.txt`,
`CoreImage-GL-init.disasm.txt`, `prewarm.disasm.txt`, `media-context.disasm.txt`,
and `CMCapture-BWCoreImageFilterRenderer.json`.

## Narrow fix and provenance

The actual 20H392 CMCapture method metadata confirms:

```text
CMCapture UUID: 45D37488-2CC7-3028-8A4D-936BCE7A220B
+[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:]
IMP: 0x1a2165b8c (unslid)
ABI: v24@0:8I16B20 (unsigned int version, BOOL styles)
```

Its code creates an EAGLContext and invokes CoreImage while prewarming portrait
filters. The original 20F66 workaround already bypasses this irrelevant GPU-less
prewarming, but its old expected IMP is 0x1a2115c0c and therefore it was refused.

The new exception applies **only to this method**, with all of:

1. Exact 20H392 IMP plus actual shared-cache slide.
2. Exact method type encoding, image path, CMCapture Mach-O UUID and arm64 header.
3. Explicit `gpu=0` boot argument.

Other media methods and their old address checks remain unchanged. No empty audio
properties, service deletion, CPU-check relaxation or firmware modification was
used. Whether this bypass removes the entire observed crash loop remains a
hardware test, not a static-analysis conclusion.

Source patch: `common/patches/userland-20h392-media-prewarm.patch`.
ASan/UBSan tests execute the actual guard against 22 matching/mismatching header,
UUID, ABI, IMP, image and GPU fixtures. iOS arm64 dynamic-library compilation and
reverse-apply validation also pass.

Important provenance correction to yesterday's handoff: userland **does** have
a separate Git database at `.userland.git`; normal `git -C userland` uses the
parent kernel repository. Use:

```text
git --git-dir=/home/pencilqwq/hackios-lancelot/.userland.git --work-tree=/home/pencilqwq/hackios-lancelot ...
```

The original deployed library was the prebuilt SHA-256
`8df90a3426f2802cc063db2ad3548d85237140a678699c742bc1ba31552cd0d9`, not a fresh
build of all current media sources. The new candidate compiles the current
media/camera source units with the narrow patch. It records the new source and
binary hashes; additional existing source functionality is not thereby declared
hardware-tested. Do not overwrite `prebuilt/userland` with an untested candidate.

The focused userland source change is saved in local commit `9df1db8`; existing
`userland/build.sh` and `userland/mkjobs.py` modifications were left untouched.
The kernel repository carries the matching patch and evidence document. All
commits remain local until the combined hardware test passes.

## Media candidate artifacts (now deployed and boot-tested)

Host artifacts: `local/lancelot-media-prewarm-20261024/` (Windows and WSL).
The new tree is copied from the **final verified**
`local/lancelot-service-log-20261023/profile/root`, not stale `userland/root`.
Only `lib/libredwood-media.dylib` differs in that tree. All 414 jobs and the
mobile-owned RAM log seed are retained. apfsck is clean and the kernel APFS
reader verifies all 467 entries.

| File | SHA-256 |
|---|---|
| root.apfs, 4 MiB | `48dde0f5296da5a4fb65a64cd281ad60586875e07e686ae89af4235b99fd125c` |
| profile.json | `2ebf7934a86d6324a195d011527184bd4710d66095778f02ff6ddc2f5705fbd0` |
| libredwood-media.dylib | `2b027166a1a849926e2438245bcf600f2f3edbcff5102b0518b5a9d194bfe1e2` |
| loader.efi | `ce92351ee1bc3e14bf80eaa96168669357701e5faaef1cd54a1b5b5291aef199` |
| boot.xnup | `ba4f41757b574a8633791a6eef9da5e2331c6124e90422cb16e934f638f3b89e` |

The kernel/XNUP is unchanged. The new EFI application's only change is its pinned
root path/hash; continuous-boot payload validation passes. UEFI stays the known
608,256-byte image with SHA-256
`a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c`.

Next device steps, checking serial and every result:

1. Physical power + volume-down to native FASTBOOT. The KLOG2 interface cannot
   flash or reboot; do not send unsupported commands repeatedly.
2. Flash the verified Android `E:\shuaji\Lancelot\boot.img` to boot and reboot.
3. Check root/Android/partition/unmount state. Deploy new profile with
   `tools/deploy-lancelot-profile.py --profile media-prewarm-20261024`, and its
   matching `EFI/HackiOS` files with `tools/deploy-lancelot.py`.
   The bounded Base64 transport and no-overwrite behavior are retained.
4. Verify read-only readback, then return to native FASTBOOT, flash known UEFI,
   reboot and capture a **new file** for at least 900 seconds.
5. Require the UUID/IMP/ABI verification log, absence of the prior CI GL fault,
   sustained mediaserverd PID/start, and complete service/storage observations.
   Then investigate remaining window-server 20H392 adaptations and activation.

Neither a successful build nor the backboardd spawn fix is an overall pass.

## Second hardware boot: media fix observed, overall pass still false

`local/lancelot-media-prewarm-20261024/` now also contains successful
`profile-deployment.log(.json)`, `esp-deployment.log`, `deployment-audit.txt`,
`deployment-complete.json`, `boot-uefi.log`, `boot-manifest.json`,
`services-first-boot.log(.fastboot)`, analysis and `hardware-observation.json`.
Independent read-only readback matches the artifact table above; root/adbd
mounts are clean, and ext4 state=1 before the UEFI boot.

- New boot ID: `8000000003b69fdb`; 900 seconds, 1,078,075 reported runtime bytes.
- Exact UUID/IMP/ABI verification and the prewarm replacement are logged.
- **Zero `user-fault:` events**, versus 53 before the fix. Four observed media
  samples keep PID 30/start 102.473459. SpringBoard 8, backboardd 13, lsd 27 and
  wifid 11 also keep their recorded PID/start times.
- Six eMMC samples: width=8, hz=48000000, batch=128, errors=0, fallback=0.
  Last sectors=4,045,714, commands=120,420, PIO time=252,054,153 us.
- PDSCDCache again recovers. New CloudKit errors explicitly identify **no iCloud
  account** (CKAccountStatusNoAccount / NSCocoaErrorDomain 134400), not corruption.
- The user still sees scrolling text, not a lock screen/desktop. Display logs
  already contain IOSurface swaps with BGRA frames, including surfaces 1 and 3.
  Do not equate rejected diagnostic hooks with proof that no frames are produced.

### USB end-of-capture failure

The old PC reader cancelled a `get_staged` transaction at its global deadline;
the next getvar consumed pending CoreData payload as an invalid fastboot status.
After the user's USB unplug/replug, a 180-second follow-up received zero bytes.
Neither fastboot nor Windows present USB-device enumeration detected the phone.
The reason the device failed to re-enumerate is **not established**; no kernel
panic was captured. Do not claim replug recovery passed or repeatedly send
commands into a pending binary reply.

The reader now lets an already-started snapshot complete with its normal command
timeout; the global duration prevents starting the next snapshot. A real KLOG2
timeout stops capture with failure, preserves partial raw replies, and asks for
reconnection instead of consuming them as another command. Six reader regression
tests pass, including deadline and timeout-stop cases. Rebooted hardware must
confirm the fix and investigate reconnect separately.

## Next control test: graphics instead of framebuffer text

This control test has now run; the result is recorded in the next section.

Before changing any window-server hook, test the existing frame path without the
forced `-v`. `common/build-lancelot.sh boot` previously always added it, despite
the generic loader's default graphics behavior. The RAM USB console tap is
before rendering suppression, so removing framebuffer text does not remove
kernel/service fault logging.

`common/build-lancelot.sh boot` now omits `-v`; checkpoint modes remain verbose.
The payload validator's new `--expect-graphics` checks this policy, and generated
continuous/checkpoint/timer/USB/graphics rejection tests pass. This is a control
experiment, **not a declaration that the console caused every desktop problem**.

Artifacts: `local/lancelot-graphics-20261024/`:

| File | SHA-256 |
|---|---|
| loader.efi, 76800 bytes | `db8faee496e835172485d72dfcb42bd21799cc6efbde993625a6a4118d975829` |
| boot.xnup, 15185416 bytes | `70dbb378ccdda87b78e202aaae1ca7c16e79553a7ee027ed71dc1ef7d2e2cce8` |

The profile is still the **already deployed** media-prewarm root with SHA-256
`48dde0f5...`, and the kernel remains `099d6539...`; no root redeployment is needed.
Regain native FASTBOOT by physical buttons, restore Android, deploy only these
matching ESP files with readback checks, then flash the same known UEFI and
capture a fresh 900-second log using the corrected reader.

If graphics mode still stalls, the next investigation has exact offline evidence:
QuartzCore UUID `9E40B2EB-260C-3AF1-AE45-A1A551A8C1B9`, EAGLServer vtable
`0x1d5b39ed0`, renderer slot `0x1d5b3a080` pointing to
`CA::WindowServer::EAGLServer::renderer()` at `0x1882eaf3c`, and
`-[CAWindowServer _detectDisplays]` at `0x1884e3150`, ABI `v16@0:8`.
See `QuartzCore-renderer-slot.json`. These have **not** been installed as hooks.
Also investigate actual GOP/physical scanout geometry and console handoff: the
driver reports 1125x2436 logical / 1080x2340 GOP panel / stride 4352. Do not assume
this matches the Redmi 9 physical panel or change firmware without permission.

## Third hardware boot: current deployed graphics-mode result

`local/lancelot-graphics-20261024/` contains `esp-deployment.log`, independent
`deployment-audit.txt`, `deployment-complete.json`, `boot-manifest.json`,
`services-first-boot.log(.fastboot)`, analysis and `hardware-observation.json`.
The already valid media profile was **not rewritten**. ESP/root readback matched,
and Ubuntu state=1 before reboot. Boot still uses the original known UEFI.

- Boot ID **800000000380974f**; 900 seconds; **2,305,683 reported runtime bytes**.
- Actual command line has no `-v`, checkpoint or timed return. Kernel/profile
  hashes are unchanged; the current deployed ESP hashes are the graphics table.
- No reported CRC mismatch/cursor gap, **zero user-fault records**, and the media
  UUID/IMP/ABI guard is still confirmed active.
- SpringBoard PID 8/start 52.568320 in 27 samples; backboardd PID 13/start
  52.766119 in 26; mediaserverd PID 36/start 110.323014 in 25; lsd PID 26/start
  90.621361 in 26; wifid PID 11/start 52.639224 in 27. No observed restart of these
  processes. mobileactivationd starts as PID 51 and also remains sampled alive.
- All **27 eMMC samples** remain width=8, hz=48000000, batch=128, errors=0,
  fallback=0. Last sectors=4,234,122, commands=125,706, PIO time=269,578,224 us.
- With framebuffer text disabled, the observer produced 27 samples, versus 6
  during the preceding verbose run with the same kernel/media profile. This is
  evidence of substantial console overhead, not a controlled storage benchmark.
- The corrected reader finishes without the prior truncated-DATA status problem;
  `fastboot -s lancelot-uefi getvar log-version` immediately returns KLOG2 afterwards.
  This validates normal capture completion, **not physical USB unplug/replug**.
- Android geometry was independently read before this boot: `wm size` physical
  1080x2340; fb0 virtual size 1088x7104; depth 32; stride 4352. The GOP panel
  dimensions/stride agree with Android. Logical 1125x2436 is the iPhone mode scaled
  by the display adapter, not itself proof of a wrong physical framebuffer.
- The user explicitly reports **Apple logo/loading screen**, not a lock screen
  or desktop. Overall pass is still false. Do not push the commits yet.

## Next targeted work: SpringBoard startup compatibility ABI

`local/lancelot-graphics-20261024/SpringBoard-compat-metadata.json` records actual
20H392 metadata and ivar layout, read from the UUID-matched cache without changing
any hook:

```text
Image: /System/Library/PrivateFrameworks/SpringBoard.framework/SpringBoard
UUID:  22034ABA-9A9A-3B94-93A5-A0DE02BEFD2E

-[SBLockdownManager _setupActivationState]
IMP: 0x1c5991a78; ABI: v16@0:8
instance size: 32; _state: int at +8; _settingUpActivationState: BOOL at +12

-[SBSetupManager _setSetupRequiredReason:]
IMP: 0x1c5aa608c; ABI: B24@0:8Q16
instance size: 48; _setupRequiredReason: unsigned long at +8
```

The original `inject_sb.c` compatibility layer still refuses both new IMPs.
Crucially, its setup setter is declared **void(long)**, but the actual 20H392
method is **BOOL(unsigned long)**. Merely replacing the expected address would
lose a meaningful return value and is not a valid ABI adaptation.

Continue by inspecting the actual method code/callers and startup/activation
state, then adapt only proven startup compatibility behavior with image UUID,
method ABI and ivar-layout guards. Keep other graphics/media/snapshot/battery
guards unchanged. The observed Apple logo does not yet prove either of these
methods is its sole cause. Physical reboots still require native FASTBOOT;
normal USB logging is currently available, and no collector is left running.
