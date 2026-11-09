#!/usr/bin/env python3
"""Check accepted boot defaults and reject truncation/unsafe argument overrides."""
from pathlib import Path
import importlib.util, json

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lancelot_profile', ROOT / 'tools/lancelot-profile.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
profile = json.loads((ROOT / 'tools/lancelot/accepted-profile.json').read_text(encoding='utf-8'))
line = module.render(profile, {})
assert len((module.PREFIX + line).encode('ascii')) == 603
for word in ['cpus=8', 'small_cores=0', 'small_cores_after=0', 'lancelot_touch=3',
             'lancelot_mali=4', 'lancelot_hakios=1', 'lancelot_gpu_lease=1', 'lancelot_gpu_dvfs=2',
             'metal=1', 'camtl=1', 'gpu=0']:
    assert word in line.split(), word
assert profile['gpu']['GPU_complete'] and profile['gpu']['basis'] == 'user_report_and_runtime_capture'
assert profile['gpu']['full_CoreAnimation_Metal_path_verified']
assert profile['storage']['deployed'] and profile['storage']['runtime_write_failures'] == 0
assert profile['metal_test']['hardware_verified'] and profile['metal_test']['round_acceptance'] == 'passed'
assert 'lancelot_data_map=' + 'a' * 64 in module.render(profile, {'LANCELOT_DATA_MAP': 'a' * 64})
assert 'lancelot_mali=1' in module.render(profile, {'LANCELOT_MALI_MODE': '1'}).split()
high = module.render(profile, {'LANCELOT_GPU_MAX_MHZ':'950', 'LANCELOT_MALI_TRACE':'2'})
assert 'lancelot_gpu_dvfs=2' in high.split() and 'mali_trace=2' in high.split()
assert len((module.PREFIX + high).encode('ascii')) == 603
metal = json.loads((ROOT/'tools/lancelot/metal-profile.json').read_bytes())
metal_line = module.render(metal,{})
assert len((module.PREFIX+metal_line).encode('ascii'))==603
assert metal_line == line
assert 'metal=1' in metal_line.split() and 'camtl=1' in metal_line.split()
assert 'gpu=0' in metal_line.split()
for broken in [metal_line.replace('metal=1','metal=0'),metal_line.replace('camtl=1','camtl=2'),
               metal_line.replace(' gpu=0',''),metal_line.replace(' gpu=0',' gpu=1')]:
    try:module.render(metal,{'LANCELOT_BOOT_CMDLINE':broken})
    except ValueError:pass
    else:raise AssertionError('Metal profile accepted invalid discovery/compositor coupling or lost the media prewarm guard')
assert 'lancelot_gpu_dvfs=1' in module.render(profile, {'LANCELOT_GPU_MAX_MHZ':'850'}).split()
without_data = module.render(profile, {'LANCELOT_DATA': '0'})
assert 'lancelot_data=0' in without_data.split() and 'lancelot_data_map=' not in without_data
cases = [{'LANCELOT_DATA_MAP': 'a' * 63}, {'LANCELOT_DATA_MAP': 'A' * 64},
         {'LANCELOT_MALI_MODE': '2'}, {'LANCELOT_GPU_TRIANGLE': '2'},
         {'LANCELOT_BOOT_CMDLINE': line.replace('cpus=8', 'cpus=1')},
         {'LANCELOT_BOOT_CMDLINE': line + ' cpus=8'},
         {'LANCELOT_BOOT_CMDLINE': line + ' loader_return_after=5'},
         {'LANCELOT_GPU_MAX_MHZ':'951'}, {'LANCELOT_MALI_TRACE':'3'},
         {'LANCELOT_BOOT_CMDLINE':line.replace('lancelot_gpu_dvfs=2','lancelot_gpu_dvfs=3')},
         {'LANCELOT_BOOT_CMDLINE': line + ' diagnostic=' + 'x' * 100}]
for overrides in cases:
    try:
        module.render(profile, overrides)
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted invalid/oversized overrides: ' + str(overrides))
legacy = json.loads((ROOT / 'tools/lancelot/high-load-profile.json').read_bytes())
legacy_line = module.render(legacy, {})
assert len((module.PREFIX + legacy_line).encode('ascii')) == 601
assert 'metal=1' not in legacy_line.split() and 'gpu=0' in legacy_line.split()
print('Accepted R18.1 lancelot Metal boot: 8 CPUs, 950MHz policy, exact data-map SHA, 603/607 bytes; retained legacy601-byte profile and rejected invalid/oversized arguments')
