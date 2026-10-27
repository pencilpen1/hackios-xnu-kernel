# Lancelot GPU features / private-DMA preparation — 2026-10-26

## Scope and publication policy

Continues `D:\AI_rom\iOSforLava\LANCELOT-GPU-AI-HANDOFF-2026-10-25.md`.
User reported Android at resume; ADB/device/root and fixed Android Boot hash
were verified. The newer user instruction is authoritative: **no GPU-related
cloud push until the GPU is fully repaired and accepted**, including new firmware
dependencies. Yesterday's already-pushed Mu commit is historical, not permission
for another push. No commit or push was performed in this work session.

Touch/SMP are retained baselines, not repeated tests. No GPT/profile formatting,
raw MMIO/development-memory access, userdata large files or firmware upload.

## Android cached property evidence

`local/lancelot-gpu-20261026/android-props/capture2/` contains a framed public
GET_GPUPROPS result from the running `r54p2-01eac0`, JM ABI `11.46` driver:

- GPU ID `0x74021000` (G52 r1p0), shader/tiler/L2/stack presence `3/1/1/3`.
- MMU_FEATURES `0x2823`: **35-bit GPU VA / 40-bit PA**, not CPU VA geometry.
- AS_PRESENT `0xff`, JS_PRESENT `7`; JS_FEATURES `0x20e/0x4fe/0x7e`.
  JS1 advertises SET_VALUE_JOB (bit2), but that is not job execution proof.
- L2_FEATURES `0x07120206`, CORE_FEATURES `2`, MEM_FEATURES `1`,
  TILER_FEATURES `0x209`, THREAD_FEATURES `0x0a046000`, TLS_ALLOC `0x400`.
- Raw property payload SHA-256:
  `bcaf9431a744da3aaa023342a50ace3796bc4903b94df2c13854c9c0d7598fe1`.

A small temporary helper issued VERSION_CHECK and property-size/fetch ioctls,
without requesting SET_FLAGS, VM allocations, mmap, jobs or power ioctls; the
helper was removed afterward. Internal driver open/handshake bookkeeping is not
claimed to be absent. The public ABI agrees with supplied older reference
headers; this is **not** proof that the running r54 binary matches those sources.

First capture (`android-props/capture/`) is explicitly rejected: remote ADB shell
merged stderr text into binary stdout. Its helper and raw/result were preserved,
not used for acceptance. The corrected helper uses a length/version envelope;
`tools/test-lancelot-gpu-props.py` rejects all truncations, text contamination,
duplicate/out-of-range keys, missing fields and changed ID.

## Live loader capture

`xnuloader/lancelot_gpu_features.h` defines exactly 42 RO G52 register words,
including all 16 JS features, texture features, 64-bit presence maps and TLS.
Two equal passes plus protection-state guards must succeed before publication.
GPU command/AS writes are still forbidden. This extends the validated firmware
power lifecycle; it does not create an XNU WACS2/SSPM ownership provider.

`local/lancelot-gpu-20261026/features/` is the first live attempt. ESP readback and
booting unchanged Mu/XNU succeeded, but the final firmware log tail was lost at
protocol handoff: **no power/feature summary was captured; acceptance FAILED**.
XNU's AO observation showed four MFG domains OFF. The log is retained at
`runtime.log`, SHA-256
`c10556ddefffcdaa496ee88c7ca864c9e6b33c4f83d33777a86d98dcc3e295a1`.
No DMA was started. Missing feature evidence must not be interpreted as zero
register values or a PASS.

`features/attempt2/` is the capture fix: after restoring TPL, the loader gives
LLOG1 15 seconds to drain the post-probe feature evidence before XNU handoff.
This adds no further GPU access. Loader build and fault regressions passed;
runtime acceptance must be read from its own `runtime-result.json` when present.
Mu image and XNU kernel are unchanged from the 2026-10-25 accepted artifacts.

### Attempt2 outcome and exact-binary discrepancy resolution

The second capture contains all 42 words, stable two-pass status, real ID/status,
32 allowlisted writes and proved owned power-off. Runtime log SHA-256:
`a16f9b1fc52a4fff646d71521dbe7bc9f9f49ce2abebbb48edba39f88786af08`.
There is no panic/CRC/ring-overrun signature in this finite capture.

An exact comparison initially FAILED: RO TEXTURE_FEATURES_3 at `0xbc` is
`0x10c6` in the loader, but Android's cached property is `0x20e`. The original
`attempt2/runtime-result.json` preserves that failure unchanged.

This discrepancy was then resolved from the **exact fixed Android kernel Image**
(`0610fe2a9b33cf6b0f03f2ee85c14af756c0b72c89012ac298fcb8b166285c20`),
not by assuming older public driver sources matched. Recovered kallsyms and
original-Image instruction checks show:

- `kbase_backend_gpuprops_get` reads four textures using enum32/33/34/**35**.
- Enum35 maps to MMIO `0xc0` (JS0); JS0 is read using that same enum35.
- The proper fourth-texture register is enum**693**, mapped to MMIO `0xbc`.
- The public cached texture3 value therefore aliases JS0=`0x20e`; the real
  texture3 remains the loader's stable `0x10c6`.

`texture-feature-audit/property-alias-proof.json` and its reproducible script
retain that proof. Local tools are vmlinux-to-elf1.4.0, capstone5.0.9 and NDK LLVM;
all binary work is PC-only, no Android patches, debug bypass or raw device MMIO.
`attempt2/runtime-result-resolved.json` accepts the live feature stage with the
explicit alias resolution; it still records the original exact mismatch. This
does **not** mark DMA, rendering or complete GPU support as passed.

## DMA preparation — host only, not deployed

The shared core now reads back AS root/memory attributes/translation config,
job NEXT head/affinity/config and cleared DONE before launch. Disabled-AS
readback and an idle NEXT command are required after completion. Dropped posted
writes, stale DONE and DONE-while-busy tests reject completion and require reset.

`xnu/files/bsd/dev/lancelot_mali_private_dma.h` is a single-transaction controller:
two private RW/NX pages separated by an unmapped guard VA, WRITE_VALUE at output
offset128, whole-output/job-tail guards and actual core reset/detach cleanup.
Host emulation walks the same four-level private page tables rather than merely
fabricating a DONE event. Guard corruption and timeout fail; reset/transport
failure preserves both data pages, all four table pages and metadata.

Actual platform/core/client/export ASan/UBSan regressions passed. These changes
exist only in Windows source plus host fixtures, **not** the deployed XNU/Mu or
WSL actual XNU source copies. They have not been committed or pushed.

Next: accept complete live features, then connect the real firmware private-page
allocator/normal-WB mapping checks/cache-to-PoC backend and tightly bounded GPU
command/AS transport to this controller. Unknown DMA must retain backing and
prevent an ordinary kernel handoff that could reclaim it. Only after actual DMA
and fault/reset evidence may pixels/Mesa/IOSurface/CoreAnimation/Metal proceed.
