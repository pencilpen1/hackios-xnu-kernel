# Lancelot 20H392: first service-log boot and guarded media candidate

## Result and current state

The previously deployed `service-log-20261023` candidate has now booted on the
phone. The original MSDC-fixed UEFI was hash-verified, flashed to `boot`, and
rebooted; no firmware source was changed. The phone is currently in continuous
XNU boot with the `lancelot-uefi` **KLOG2** interface, not Android.

The next media-fix candidate is **built and checked, but not yet deployed or
boot-tested**. A physical return to native FASTBOOT is required to regain the
Android deployment path. No GitHub push is authorized by these partial results.

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

## Built candidate, pending device deployment

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
