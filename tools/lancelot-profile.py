#!/usr/bin/env python3
"""Render the repository's accepted lancelot boot profile with bounded overrides."""
from pathlib import Path
import argparse, json, os, re

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'debug=0x8 rd=md0 '
LIMIT = 607

def render(profile, environment=None):
    env = os.environ if environment is None else environment
    if profile.get('device') != 'lancelot' or profile.get('soc') != 'MT6768':
        raise ValueError('Expected the lancelot MT6768 boot profile')
    words = env.get('LANCELOT_BOOT_CMDLINE', profile['cmdline']).split()

    def set_value(key, value):
        nonlocal words
        words = [word for word in words if not word.startswith(key + '=')]
        if value is not None:
            words.append(key + '=' + value)

    if 'LANCELOT_MALI_MODE' in env:
        mode = env['LANCELOT_MALI_MODE']
        if mode not in ('1', '3', '4'):
            raise ValueError('LANCELOT_MALI_MODE must be 1, 3 or 4')
        set_value('lancelot_mali', mode)
    if 'LANCELOT_GPU_TRIANGLE' in env:
        triangle = env['LANCELOT_GPU_TRIANGLE']
        if triangle not in ('0', '1'):
            raise ValueError('LANCELOT_GPU_TRIANGLE must be 0 or 1')
        set_value('lancelot_gpu_triangle', '1' if triangle == '1' else None)
    if 'LANCELOT_GPU_ARGS' in env:
        words = [word for word in words if not word.startswith('lancelot_gpu_')]
        words += env['LANCELOT_GPU_ARGS'].split()
    if 'LANCELOT_DATA_MAP' in env:
        set_value('lancelot_data_map', env['LANCELOT_DATA_MAP'])
    if 'LANCELOT_DATA' in env:
        if env['LANCELOT_DATA'] not in ('0', '1'):
            raise ValueError('LANCELOT_DATA must be 0 or 1')
        set_value('lancelot_data', env['LANCELOT_DATA'])
        if env['LANCELOT_DATA'] == '0':
            set_value('lancelot_data_map', None)

    values = {}
    for word in words:
        if '=' not in word:
            raise ValueError('Boot profile requires explicit key=value arguments: ' + word)
        key, value = word.split('=', 1)
        if key in values and key != 'glenv':
            raise ValueError('Duplicate boot argument: ' + key)
        values[key] = value
    for key, wanted in [('cpus', '8'), ('small_cores', '0'), ('small_cores_after', '0'),
                        ('lancelot_touch', '3'), ('root_apfs', 'lancelot_ios')]:
        if values.get(key) != wanted:
            raise ValueError('Continuous lancelot boot requires ' + key + '=' + wanted)
    if values.get('lancelot_mali') not in ('1', '3', '4'):
        raise ValueError('Unsupported Mali service mode')
    if values.get('lancelot_data') == '1' and not re.fullmatch('[0-9a-f]{64}', values.get('lancelot_data_map', '')):
        raise ValueError('Writable data volume requires its exact 64-digit map SHA256')
    if any(key.startswith(('loader_return_after', 'return_cpu', 'return_panic')) for key in values):
        raise ValueError('Continuous boot profile cannot request timed return/checkpoints')
    line = ' '.join(words)
    try:
        size = len((PREFIX + line).encode('ascii'))
    except UnicodeEncodeError as error:
        raise ValueError('Loader boot arguments must be ASCII') from error
    if size > LIMIT:
        raise ValueError(f'Effective loader command line is {size}/{LIMIT} bytes; shorten explicit overrides')
    return line

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, default=ROOT / 'tools/lancelot/accepted-profile.json')
    args = parser.parse_args()
    try:
        print(render(json.loads(args.profile.read_text(encoding='utf-8'))))
    except (ValueError, KeyError) as error:
        parser.exit(1, str(error) + '\n')

if __name__ == '__main__':
    main()
