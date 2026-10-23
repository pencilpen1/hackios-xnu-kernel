# Lancelot storage performance — 2026-10-23

## Observed running build

The user reports the native-storage build (`e4b4fec`, kernel SHA-256
`2e0575e34e008dac99227fe49e7e66433ea6c370cc59f1d31f08d8d468c3219d`)
reached an Apple logo/progress bar after roughly 20 minutes and is still progressing.
Do not reboot or replace that live run to benchmark it. This is a user-observed boot
time, not a captured storage throughput measurement or proof of a completed iOS boot.

## Confirmed implementation limits

1. `lancelot_emmc.c` splits every read into 512-byte CMD17 operations. Each operation
   waits for idle, clears FIFO/status, sets up a command, polls and toggles interrupts.
   PIO also consumes the single boot CPU. There is no multiblock read or DMA path.
2. The original `lancelot_ext4.h` re-read the external extent node on every 4 KiB file
   block. An 8 GiB file cannot fit its extent mapping inside the four inline records
   (one initialized extent is at most 128 MiB with 4 KiB blocks). Repeated external
   metadata reads are therefore relevant to this deployed container, not just tiny files.
3. `apfs_vfsops.c` recognized only UFS and virtio as costly storage. It incorrectly
   classified the native MTK block device as a RAM disk, including holding the APFS
   metadata lock across slow reads and not batching compressed reads as flash I/O.
4. The pinned firmware source at Mu-Silicium `251bd657` initializes bus width to one
   bit and requests a 50 MHz clock (`MsdcInit`/`CardSetBusMode`). This corresponds to
   at most 6.25 MB/s on the data line before overhead; it is not Android's HS200/HS400
   mode. Live clock/bus registers were not retrieved from the running kernel, so this
   is a source/configuration limit, not a measured current transfer rate.

The evidence points to substantial software/transport limitations. It does not diagnose
a slow/failing eMMC chip. The 20-minute time cannot yet be divided reliably between I/O,
single-core decompression/initialization and other startup work.

## Host-measured read amplification

`tools/test-lancelot-ext4-perf.py` compiles both the initial C reader from `e4b4fec`
and the updated reader. A valid synthetic 8 GiB extent tree provides deterministic
contents and physical gaps. The test checks every output byte and verifies that reads
do not cross physical gaps. ASan/UBSan are enabled. Figures count reader callbacks and
their 512-byte equivalents; **they are not eMMC latency/throughput measurements**.

| 128 KiB logical read | Initial callbacks / metadata reads / CMD17 equivalents | Updated |
|---|---:|---:|
| Cold extent node | 64 / 32 / 512 | 2 / 1 / 264 |
| Warm extent node | 64 / 32 / 512 | 1 / 0 / 256 |
| Across physical extent boundary | 66 / 33 / 520 | 2 / 0 / 256 |

For the cold case, physical bytes fall from 256 KiB to 132 KiB. Warm reads transfer
128 KiB. This establishes removal of redundant I/O, not a predicted 2x boot speedup.

## Prepared changes (not applied to the running phone)

- Eight-entry (32 KiB) read-only extent-node cache; invalidated by `ext4_open()`.
  It caches metadata, not file data. External allocation/metadata changes require reopening.
- Coalesced file callbacks up to 128 KiB, preserving physical-extent and sparse-hole boundaries.
- APFS identifies `/dev/lancelot_ios` as flash and releases its metadata lock during I/O.
  The default metadata cluster stays at one block for CMD17 PIO to avoid speculative read
  amplification; an explicit `apfs_meta_cluster` still overrides that setting.
- Native counters and inherited bus/clock register reporting. The CMD17 protocol, clock,
  bus width, firmware image and continuous-boot policy are unchanged.

The revised kernel/loader build passed, including the linked-code and payload checks.
Host-only artifacts and baseline/updated request counts are archived in
`local/lancelot-perf-20261023/` with `build-report.json`:

- Kernel: `6c5a730708ed3ddfc58b8e55c7a5b919bde087b84519a893c2c51575fd3505bc`.
- Loader: `52ea2919d514b52d3c7149a450523f128e284e43ce4947b5b42475df5091cb5a`.
- XNUP: `e3dbe710631d2ae71cc80e82fd8b239d1e11c26db3950a820c92a7bc51ec8de9`.

These files have not been deployed. Real ext4 fixtures (1/2/4 KiB, 32/64-bit descriptors,
sparse files, corrupt metadata and partial reads) still pass sanitizer checks. Boot time
and hardware throughput must be measured on a later controlled run.

## Measurements for the next controlled run

New read-only sysctls under `kern.redwood`:

- `lancelot_mmc_sectors`: successful physical 512-byte reads, including metadata.
- `lancelot_mmc_requests`, `lancelot_mmc_bytes`: logical image requests/bytes.
- `lancelot_mmc_pio_us`: time spent in native PIO transactions.
- `lancelot_mmc_read_us`, `lancelot_mmc_lock_us`: file-read processing and lock wait.
- `lancelot_mmc_meta_reads`, `lancelot_mmc_meta_hits`: extent-cache traffic.
- `lancelot_mmc_errors`, `lancelot_mmc_cfg`, `lancelot_mmc_bus_width`.

Use counter deltas from the same interval. Physical read amplification is
`512 * delta(sectors) / delta(bytes)`; native transfer throughput is
`512 * delta(sectors) / delta(pio_us)` bytes per microsecond. These are approximate
independent-counter snapshots. Existing `kern.apfs_stats` also reports device versus
inflate timings. The currently running older kernel does not contain the new counters.

Next throughput work is bounded multiblock reads and, after hardware validation, wider
bus/high-speed modes or DMA. Any firmware source change still needs the user's approval
and its own commit. Persistent writable data-image support remains a separate pending goal.

The subsequent local candidate implements counted multiblock PIO and verified wide-SDR/HS52
negotiation: see `LANCELOT-TRANSFER-UPGRADE.md`. Per the user's latest instruction, it stays
in local commits pending validation; it is not on the phone or pushed to GitHub.
