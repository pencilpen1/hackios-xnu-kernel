# Lancelot service-log analysis / handoff — 2026-10-23

## Actual hardware results

Runtime transport commit `22a2efb` was deployed with verified Ubuntu/ESP readback,
using the previously authorized MSDC-fixed UEFI. No UEFI source was changed.
Boot ID: `8000000003f74d4f` (XNU's cursor domain).

Evidence: `local/lancelot-runtime-log-20261023/`:

- `xnu-runtime-services.log`: **1,908,135 decoded bytes** during 300 seconds,
  with CRC32 validation and no reported cursor gaps/CRC failures.
- `xnu-runtime-follow.log`: preceding 180-second capture, 409,876 decoded bytes.
- `.fastboot` files retain command replies; `hardware-observation.json` and
  `xnu-runtime-services.analysis.json` record observations and line references.
- Initial `uefi.log` was too early/short to see enumeration. The initial runtime
  reader then printed repeated firmware command rejections during the firmware
  loading/window phase. It did eventually receive KLOG2, rather than permanently
  failing takeover. A subsequent `getvar log-version` returned `KLOG2` immediately.

### Storage

Three bootdiag samples reported **width=8, hz=48000000, batch=128, errors=0,
fallback=0**. Last observed sectors=2,809,418, commands=78,276, PIO time=187,755,813 us.
This validates mode negotiation and continued reads in this run; it is not a
controlled before/after throughput benchmark or proof that the desktop works.

### Service failures, not guesses from screen text

- **backboardd:** 49 observed `posix_spawn(/usr/libexec/backboardd)` failures,
  `0xd/EACCES`, followed by xpcproxy exit 78. Its binary is mode 0755 in the actual
  APFS listing. The diagnostic directory was incorrectly root:root 0755 while
  backboardd runs as mobile. This is a reproducible log-file permission defect;
  the revised profile fixes ownership and precreates logs. Hardware must confirm
  that it removes the spawn error rather than assuming every EACCES has this cause.
- **SpringBoard:** repeated exit 255 and PID changes (8 → 153 in samples). Its
  20F66-addressed hook checks reject the actual 20H392 method addresses. Preserve
  those guards; do not substitute logged pointers without validating the ABI.
- **mediaserverd:** definite **SIGSEGV** loop, not just unsupported audio properties.
  Launchd reported **26 successive crashes**, then throttled launches. Samples:
  PID 30 → 163 → 304. There were 80 audio-plugin initializations in the captured
  stream, consistent with repeated media-server startup.
- **RedwoodAudio:** captured selectors include `cust`, `clok`, `cstb`, `taps`,
  `accs`, `clk#`, `auto`, `cfsz`, `bcvl`, `lmod`, `srnd`, with object/scope/element.
  These are property probes; they do not identify the faulting PC. The existing
  media injection contains a guarded portrait-filter prewarm workaround for a
  GPU-less null-call failure, but applicability to this 20H392 crash is not yet
  established. Capture PC/LR and full media output before changing it.
- **lsd:** PID **28**, start **93**, remained the same in the later samples.
  No lsd crash loop is established. The previously verified missing cpumon
  entitlement restores default limits; it does not itself terminate the process.
- **CoreData:** `knowledgeC.db` and `IdentityServices/PDS/PDSCDCache` initially
  absent. `NSCocoaErrorDomain 512`, underlying file-create errno 2; the latter
  explicitly reports successful recovery after discovering a missing parent.
  This is not evidence of an APFS/SQLite corruption requiring database deletion.

## Revised candidate: service-log-20261023

1. Host reader auto-detects firmware LLOG1 versus kernel KLOG2 and prints positive
   byte progress every 15 seconds. Firmware-only data cannot count as runtime success.
2. Move the user-console tap from `kmoutput` to `console_write`, including the
   unbuffered path. This adds framebuffer-only kernel printf (including CPU monitor
   and storage messages), without double-logging user-space console output.
3. A failed user memory fault records PID/name, PC/LR/SP/FP, ESR/FAR and VM result
   before the existing Mach exception delivery. No user pointer is dereferenced;
   normal page-fault recovery and fatal exception behavior remain intact.
4. Detailed stdout/stderr/os_log files for SpringBoard, backboardd, mediaserverd,
   mobile/system lsd, wifid and coredatad. RedwoodAudio writes mediaserverd stderr.
   Each file is forwarded unfiltered over the RAM log, with file-offset markers.
   The observer prints UID/start microseconds, and schedules samples by wall time
   rather than counting forwarding iterations. Per-file cap remains 4 MiB, with
   an explicit unread-byte notice before truncation.
5. All seven files are seeded 0644 mobile:mobile in a 0755 mobile-owned directory.
   A real POSIX uid-501 fixture reproduced the old EACCES and opened all revised
   logs successfully. Explicit launch job / on-demand semantics are preserved.

Artifacts: `local/lancelot-service-log-20261023/build-report.json`.

| Artifact | SHA-256 |
|---|---|
| Kernel | `099d6539e24ebc95732ecbd11b45adf71c9cd4fd22eb43d02a1cde2868cb7d5c` |
| Loader | `ac98357adf12bb0e0f78cbcdbc6180d520c0e4e062a332435a3a87d758a28d30` |
| XNUP | `ba4f41757b574a8633791a6eef9da5e2331c6124e90422cb16e934f638f3b89e` |
| root.apfs | `8089e522be3d774cb568b853e380421c02ed3defacffd23e4a3b263f20c5ae64` |

New root path: `/hackios/20H392/service-log-20261023/root.apfs`. 4 MiB,
414 job plists, **467 APFS entries verified** by apfsck and the kernel reader.
Kernel build, payload continuous-boot checks, MUSB/ring ASan/UBSan checks and the
actual overlay reverse-apply check passed. Revised candidate deployment is pending.

## Next work

Deploy the new profile first, then its loader/XNUP and the known UEFI; verify every
readback and Ubuntu clean/unmounted state. Capture with `--runtime --quiet` for at
least 600 seconds, keeping continuous boot. Confirm kernel messages are present,
backboardd gets past spawn, and all seven service files export. Symbolicate actual
mediaserverd faults against the imported 20H392 cache before fixing media hooks.
Then address any remaining validated window-server/activation dependency failure.

All source commits stay local until the combined system achieves a stable desktop
or lock screen and sustained error-free I/O. Persistent media writes, input/display
integration and performance measurements remain subsequent work.
