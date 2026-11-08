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
assert len((module.PREFIX + line).encode('ascii')) == 601
for word in ['cpus=8', 'small_cores=0', 'small_cores_after=0', 'lancelot_touch=3',
             'lancelot_mali=4', 'lancelot_hakios=1', 'lancelot_gpu_lease=1', 'lancelot_gpu_dvfs=1']:
    assert word in line.split(), word
assert profile['gpu']['GPU_complete'] and profile['gpu']['basis'] == 'user_report'
assert profile['storage']['runtime_acceptance'] == 'pending' and not profile['storage']['deployed']
assert 'lancelot_data_map=' + 'a' * 64 in module.render(profile, {'LANCELOT_DATA_MAP': 'a' * 64})
assert 'lancelot_mali=1' in module.render(profile, {'LANCELOT_MALI_MODE': '1'}).split()
without_data = module.render(profile, {'LANCELOT_DATA': '0'})
assert 'lancelot_data=0' in without_data.split() and 'lancelot_data_map=' not in without_data
cases = [{'LANCELOT_DATA_MAP': 'a' * 63}, {'LANCELOT_DATA_MAP': 'A' * 64},
         {'LANCELOT_MALI_MODE': '2'}, {'LANCELOT_GPU_TRIANGLE': '2'},
         {'LANCELOT_BOOT_CMDLINE': line.replace('cpus=8', 'cpus=1')},
         {'LANCELOT_BOOT_CMDLINE': line + ' cpus=8'},
         {'LANCELOT_BOOT_CMDLINE': line + ' loader_return_after=5'},
         {'LANCELOT_BOOT_CMDLINE': line + ' diagnostic=' + 'x' * 100}]
for overrides in cases:
    try:
        module.render(profile, overrides)
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted invalid/oversized overrides: ' + str(overrides))
print('Accepted lancelot boot profile: 8 CPUs, GPU service/DVFS, exact data-map SHA, 601/607 bytes; rejected duplicate/oversized/invalid arguments')
