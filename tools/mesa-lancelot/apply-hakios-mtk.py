#!/usr/bin/env python3
"""Apply the public MTK shader behavior and one-library AIR integration.

LMAL mapping, completion errors, and owned IOSurface handoff stay in the native
adapter. The reference compiler passes are taken from the pinned public patch.
"""
from pathlib import Path
import hashlib, json, re, subprocess, sys

HERE = Path(__file__).resolve().parent
S = Path(sys.argv[1]).resolve()
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
patch = HERE / 'hakios-mtk-perf.patch'
text = patch.read_text()
parts = re.split(r'(?m)(?=^--- a/)', text)
selected = [p for p in parts if p.startswith('--- a/') and
            not p.startswith('--- a/src/gallium/drivers/panfrost/pan_job.c\n')]
assert len(selected) == 4
assert sum(p.startswith('--- a/src/gallium/drivers/panfrost/pan_job.c\n') for p in parts) == 1
for i, part in enumerate(selected):
    if part.startswith('--- a/src/gallium/drivers/panfrost/pan_shader.c\n'):
        hunks = re.split(r'(?m)(?=^@@)', part)
        assert len(hunks) == 4 and hunks[1].startswith('@@ -36,6 ')
        # The native overlay adds diagnostic includes between these headers.
        # Preserve the two executable hunks; add the same headers below.
        selected[i] = hunks[0] + ''.join(hunks[2:])
result = subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '--no-backup-if-mismatch',
                         '-p1', '-d', str(S)], input=''.join(selected).encode(), capture_output=True)
sys.stdout.buffer.write(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.returncode)

def change(name, old, new):
    p = S / name
    value = p.read_text()
    assert value.count(old) == 1, (name, old[:100], value.count(old))
    p.write_text(value.replace(old, new, 1))

change('src/gallium/drivers/panfrost/pan_shader.c', '#include "pan_context.h"',
       '#include "pan_context.h"\n#include "util/os_time.h"\n#include <string.h>\n'
       '#ifdef __APPLE__\n#include <sys/sysctl.h>\n#endif')

# The public counters are defined together; existing native completed-job
# metadata remains the authority for GPU time and error classification.
job = 'src/gallium/drivers/panfrost/pan_job.c'
change(job, '#include "pan_util.h"', '#include "pan_util.h"\n#include "util/os_time.h"\n'
       'uint64_t redwood_pan_submit_ns, redwood_pan_submits, redwood_pan_blend_compiles;\n'
       'uint64_t redwood_pan_variant_compiles, redwood_pan_variant_ns;')
change(job, '''panfrost_batch_submit(struct panfrost_context *ctx,
                      struct panfrost_batch *batch, unsigned reason)
{''', '''panfrost_batch_submit(struct panfrost_context *ctx,
                      struct panfrost_batch *batch, unsigned reason)
{
   int64_t redwood_began = os_time_get_nano();''')
# Count the existing native submit function at its common cleanup path.
change(job, '#endif\n   panfrost_batch_cleanup(ctx, batch);\n}\n\n/* Submit all batches */',
       '#endif\n   panfrost_batch_cleanup(ctx, batch);\n'
       '   redwood_pan_submit_ns += os_time_get_nano() - redwood_began;\n'
       '   redwood_pan_submits++;\n}\n\n/* Submit all batches */')
change('src/panfrost/lib/pan_blend.c',
       '   memcpy(variant->constants, state->constants, sizeof(variant->constants));',
       '   memcpy(variant->constants, state->constants, sizeof(variant->constants));\n'
       '#ifdef __APPLE__\n'
       '   extern uint64_t redwood_pan_blend_compiles;\n'
       '   redwood_pan_blend_compiles++;\n'
       '#endif')

# Equivalent to panfrost-osmesa-air.patch for this native OSMesa target. AIR
# and OpenGL share one DRM registry at runtime via the libredwoodair alias.
target = 'src/gallium/targets/osmesa/meson.build'
change(target, "  'osmesa_target.c',", "  ['osmesa_target.c', '../../../compiler/air/air_metal.c'],")
change(target, '    inc_gallium_drivers,',
       '    inc_gallium_drivers, inc_mapi, inc_mesa, inc_gallium_winsys_sw, inc_panfrost,')
change(target, 'driver_swrast, driver_panfrost, dep_libdrm, idep_mesautil,',
       'driver_swrast, driver_panfrost, dep_libdrm, idep_air, idep_mesautil,')

shader = (S / 'src/gallium/drivers/panfrost/pan_shader.c').read_text()
assert 'inputs.pushable_ubos = 0;' not in shader
assert 'inputs.pushable_ubos = ~0u;' in shader
compiler = (S / 'src/panfrost/compiler/bifrost_compile.c').read_text()
assert 'redwood_widen_ubo16' in compiler and 'redwood_scratch_to_regs' in compiler
resource = (S / 'src/gallium/drivers/panfrost/pan_resource.c').read_text()
assert 'no transaction elimination' in resource
receipt = {'reference_commit': '3f31f3428528ca4435cdc4826ea97dc9415225b5',
           'patch_sha256': sha(patch), 'selected_reference_file_sections': len(selected),
           'counter_section_adapted': 1, 'push_all_UBOs': True, 'ubo16_widen': True,
           'scratch_to_registers': True, 'disable_transaction_elimination': True,
           'AIR_in_libOSMesa': True, 'runtime_verified': False, 'GPU_complete': False}
with (S / '.hakios-mtk.json').open('x') as f:
    json.dump(receipt, f, indent=2)
print('Applied public MTK compiler behavior and AIR/OSMesa integration')
