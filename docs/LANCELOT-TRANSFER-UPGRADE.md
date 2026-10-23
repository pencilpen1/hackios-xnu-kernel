# Local transfer upgrade — 2026-10-23

**Local commits only. Do not push this candidate to GitHub until its functionality
and stability have been validated.** The phone's existing boot is left running.

## Implemented

- Native reads use CMD23 (set block count) followed by CMD18 for 2–128 sectors.
  A 64 KiB read needs two card commands rather than 128 CMD17 commands. Single-sector
  and unaligned head/tail reads retain CMD17. Larger requests are split into batches;
  the 64 KiB batch bounds PIO interrupt-off latency, not the total request size.
- The kernel FIFO adapter transfers words in bursts instead of making an indirect
  register-read callback per word. Aligned destinations use word-sized copies; arbitrary
  destination alignment remains supported.
- On initialization, verify the original superblock read first, then negotiate SDR
  width up to the board's wired 8 bits. Test 8, then 4, retaining 1 bit if necessary.
  CMD6 changes only volatile BUS_WIDTH; the host's SDC_CFG width field changes with it.
- Clock selection uses the actual source-clock frequency, supplied by the EFI app via
  read-only calls to the existing MediaTek clock protocol. It chooses the fastest existing
  source/divisor setting within the card's current timing capability: up to 52 MHz for
  HS_TIMING=1 with HS52 support, otherwise 26 MHz. For a 208 MHz source, this permits
  52 MHz instead of the former 50 MHz request rounding down to 26 MHz.
- Every mode change and rollback requires EXT_CSD read-only fields and the known Ubuntu
  superblock SHA-256 to agree. Do not require BUS_WIDTH to echo its written value: mode
  bytes need not read back like ordinary read/write registers. CMD13 checks delayed
  SWITCH_ERROR and ready/TRAN state after R1b busy completion.
- Failed/short transfers use bounded CMD12/CMD13 recovery and FIFO cleanup. No CMD18 is
  sent if CMD23 fails. The caller never treats a partial read as complete.
- A counted-read superblock probe enables batching only if its digest matches. A failed
  probe can retain CMD17 only after verified recovery and a fresh matching single-read probe.

## Scope and limits

The firmware source/image is unchanged. Native driver configuration changes are made
after handoff; there is no firmware mailbox or second CPU. Media writes/erase and partition
switching are absent from the command whitelist. Persistent writable-data support remains
a pending requirement, separate from volatile bus configuration.

This implements wide SDR/HS52 and multiblock PIO. HS200/HS400 activation requires proper
voltage/tuning/strobe support, and DMA needs descriptors/cache coherency; neither is claimed
by this candidate. A faster/tuned/DDR mode inherited from a different validated firmware
is preserved instead of being reinterpreted or downgraded. Unknown source frequency preserves
the inherited clock while still permitting verified width negotiation.

The loader/kernel handoff v2 is 96 bytes and includes RCA, board width, source clock and
known firmware timing. The pinned MT6768 firmware sets RCA 1, HS timing, and muxes DAT0–DAT7;
the Android DT independently declares an 8-bit bus. Legacy 72-byte v1 metadata is accepted
with its known RCA-1 contract but no width promotion or inferred source frequency. Deploy
the matching new loader and kernel together to enable the complete upgrade.

## Diagnostics

Continuous boot remains the default. Optional boot arguments for fault isolation:

- `lancelot_mmc_tune=0`: retain the inherited width/clock.
- `lancelot_mmc_width=1|4|8`: ceiling for widening (within the loader's wired width).
- `lancelot_mmc_blocks=1..128`: transfer batch size; 1 selects CMD17 only.

New `kern.redwood` counters: `lancelot_mmc_clock_hz`, `lancelot_mmc_batch_blocks`,
`lancelot_mmc_commands`, `lancelot_mmc_multi_reads`, `lancelot_mmc_recoveries`, and
`lancelot_mmc_mode_fallbacks`. `lancelot_mmc_bus_width` now reports the active verified width.
These are available only in the candidate kernel, not the phone's currently running build.

## Validation

`tools/test-lancelot-msdc.py` compiles the actual transport/negotiation code under
ASan/UBSan. It checks every sector count 1–128, all eight buffer alignments, both FIFO
adapters, exact bytes/canaries, command counts, timeouts/CRC/R1/short-read/overflow failures,
CMD23 rejection, failed recovery, prior interrupt state restoration, 8/4/1-bit fallbacks,
clock rollback, unknown clock source and preservation of an inherited tuned mode.
The simulated BUS_WIDTH byte deliberately does not echo its setting, and SWITCH_ERROR
can arrive through CMD13 after CMD6's initial response.

Host tests passing is not proof of hardware speed/stability. The candidate has not been
flashed, and no live eMMC throughput or boot-time improvement is asserted here.

The final kernel build passed (`make build` exit 0), including linked checkpoint-code
verification and continuous Ubuntu-root payload validation. Local artifacts are in
`local/lancelot-transfer-upgrade-20261023/`, with logs and `build-report.json`:

| Artifact | SHA-256 |
|---|---|
| `mach.lancelot` | `8b7c530a432097dfb5c4a5f39a22f6058a77e120d5a31621c5b419e4447e45d0` |
| `EFI/HackiOS/loader.efi` | `fc206157a91a469cf8cde9c59f47917dc88180d0078929066f7dd7dfbc7c26eb` |
| `EFI/HackiOS/boot.xnup` | `25b120b39cb8c71b3c29aa42f292c114c81cd10045a2e8ca57ee7116333f88f8` |

The running phone has not received these files. Keep this candidate local until its
negotiated width/clock, sustained counted reads and normal boot are verified on hardware.
