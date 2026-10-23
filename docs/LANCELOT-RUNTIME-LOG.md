# Lancelot runtime USB log — 2026-10-23

**Hardware update:** runtime USB works: boot `8000000003f74d4f`, 1,908,135 bytes
received in a 300-second capture with no reported CRC errors or cursor gaps. The
first implementation missed framebuffer-only kernel printf. Read
`LANCELOT-SERVICE-LOG-2026-10-23.md` for the corrected candidate and actual service failures.

## Implementation

The original LLOG1 interface runs in the UEFI MUSB poll callback. That callback cannot
execute while XNU owns the single CPU. The new kernel services MUSB itself, after an
explicit `/chosen/lancelot-usb-log` offer from the EFI application. It requires `cpus=1`
and an already initialized full-speed controller. `lancelot_usb_log=0` disables takeover.

- EFI supplies the MAC address and boot ID; no firmware callbacks/pointers are invoked.
- XNU briefly detaches and re-enumerates the same VID/PID/serial/interface. Existing
  Windows fastboot driver binding is retained. This is USB bulk diagnostics, not a COM port.
- The controller remains PIO/full-speed with firmware-calibrated PHY/clocks. A 1 ms
  thread call services EP0 and bulk transfers with a 250 us maximum reply burst per tick.
- KLOG2 snapshots carry boot ID, absolute start/end, payload length and CRC32. Each
  snapshot is immutable while sent through standard fastboot `upload`/`get_staged`.
  The kernel also supports the existing LLOG1 `oem log` format.
- The first 256 KiB and most recent 1 MiB of console are retained. Cursor gaps explicitly
  report overruns; this is bounded RAM logging, not lossless storage for an unlimited boot.
- Bootdiag forwards incremental, unfiltered SpringBoard/backboardd file content using
  root-only `kern.redwood.usb_log`, bypassing framebuffer rendering. It includes file offsets
  and samples lsd/mediaserverd, window-server PIDs, storage and USB counters every 30 s.
  Its dedicated RAM file logs still have the 4 MiB truncation cap.
- On return/panic, XNU detaches USB. The EFI application reattaches for a new host bus
  reset and firmware log retrieval. No Mu-Silicium firmware source was changed.

## Candidate and checks

`local/lancelot-runtime-log-20261023/build-report.json` records exact artifacts:

| Artifact | SHA-256 |
|---|---|
| Kernel | `1f9ad145f1622568410f73a00f84a6f7e398712c2062bbd6c9a17891f7818a41` |
| Loader | `dfd858d942762afcf9118cf3ffaf7a73188ee908e175848254b2a5a34cc4a260` |
| XNUP | `d35b0660c3cf6556fb71d5a3e6618f46329d190f265fa182d43b381b977fdbee` |
| root.apfs | `2a77f18e5f8870133baf00094201634de5467221164f9ad7f0296731f1c959bc` |

Root path: `/hackios/20H392/runtime-log-20261023/root.apfs`, 4 MiB, 414 launchd
plists, 466 APFS entries checked with apfsck and the kernel APFS reader. Prior profiles
remain at their original paths. The MSDC upgrade is included. Use the same authorized
MSDC-fixed UEFI (`a23bdf6d...`), not the old `MuUEFI-lancelot.img` baseline.

Host checks passed: actual MUSB EP0 status/address sequencing, FS descriptors,
backpressure, resets, malformed/oversize requests, binary upload byte integrity,
CRC/length rejection, and production console head/tail/wrap/future-cursor cases under
ASan/UBSan. XNU `make build` and continuous XNUP validation passed. These checks are
not desktop validation. USB takeover has subsequently passed the capture above;
the combined boot has not passed.

## Capture

```powershell
python tools/read-lancelot-log.py --runtime --quiet `
  --fastboot 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\fastboot.exe' `
  --out local/lancelot-runtime-log-20261023/xnu-runtime.log --duration 600
```

Keep the reader running through firmware/kernel USB re-enumeration. It now queries
`log-version`, captures LLOG1 during firmware loading and switches to KLOG2 on handoff.
It reports received-byte progress every 15 seconds instead of treating the firmware's
unsupported `oem klog` response as a repeated error. It saves the decoded stream plus `.fastboot`
command diagnostics. Legacy firmware-only capture omits `--runtime`.

First analysis targets: actual MMC `width/hz/batch/errors`, successful continuous reads,
SpringBoard/backboardd/mediaserverd/lsd lifetimes, first failures in the detailed window-server
output and guarded 20H392 hook mismatches. Push only after the combined hardware result
passes; compilation and a working log transport alone do not establish a stable desktop.
