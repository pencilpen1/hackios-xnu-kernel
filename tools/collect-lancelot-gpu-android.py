#!/usr/bin/env python3
"""Read existing Android exports only; no devmem, debug commands or phone writes."""
from pathlib import Path
import argparse
import datetime
import gzip
import hashlib
import json
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--adb', required=True)
parser.add_argument('--serial', default='116745f30503')
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
if (args.out / 'manifest.json').exists():
    parser.error('Output already contains a capture; use a fresh evidence subdirectory')
records = []

def capture(name, command, root=False, binary=False):
    remote = ['exec-out'] + (['su', '-c', command] if root else ['sh', '-c', command])
    result = subprocess.run([args.adb, '-s', args.serial, *remote],
                            capture_output=True, timeout=40)
    path = args.out / name
    path.write_bytes(result.stdout)
    (args.out / (name + '.stderr')).write_bytes(result.stderr)
    records.append({'file': name, 'remote_command': command, 'root': root,
                    'exit': result.returncode, 'bytes': len(result.stdout),
                    'sha256': hashlib.sha256(result.stdout).hexdigest()})
    print(f'{name}: exit={result.returncode} bytes={len(result.stdout)}', flush=True)
    return result.stdout if binary else result.stdout.decode('utf-8', 'replace')

device = capture('device.txt', 'getprop ro.product.device').strip()
if device != 'lancelot':
    raise SystemExit('Not lancelot; no privileged collection performed')
uid = capture('root.txt', 'id', root=True)
if 'uid=0(' not in uid:
    raise SystemExit('Existing root unavailable; no bypass attempted')
capture('kernel.txt', 'uname -a')
capture('cmdline.txt', 'cat /proc/cmdline', root=True)
dmesg = capture('dmesg.raw', 'dmesg', root=True)
gpu_lines = [line for line in dmesg.replace('\0', '').splitlines()
             if re.search(r'mali|mfg|gpufreq|pwrap|pmic.wrap|sspm|scp|mt6358|vgpu|vsram', line, re.I)]
(args.out / 'dmesg-gpu.txt').write_text('\n'.join(gpu_lines) + '\n', encoding='utf-8')
config_raw = capture('config.gz', 'cat /proc/config.gz', root=True, binary=True)
try:
    config = gzip.decompress(config_raw).decode()
except (OSError, EOFError):
    config = ''
(args.out / 'config.txt').write_text(config, encoding='utf-8')
(args.out / 'config-gpu.txt').write_text('\n'.join(line for line in config.splitlines()
    if re.search(r'MALI|GPU|PMIC|PWRAP|SSPM|TINYSYS|SCP|MT6768|ARM_PSCI', line)) + '\n', encoding='utf-8')
capture('exports.txt', 'ls -ld /sys/class/misc/mali* /sys/devices/platform/*mali* '
        '/sys/devices/platform/*gpu* /proc/gpufreq /sys/kernel/debug/pm_genpd '
        '/sys/kernel/debug/regulator /sys/kernel/debug/clk /sys/bus/platform/drivers/*pwrap*; '
        'ls -l /proc/gpufreq /sys/class/misc/mali*/device/ /sys/class/devfreq/', root=True)
capture('platform-drivers.txt', 'ls -l /sys/bus/platform/drivers', root=True)
capture('dt-candidates.txt', 'find /sys/firmware/devicetree/base -type d '
        '-iname "*mali*" -o -type d -iname "*gpu*" -o -type d -iname "*pwrap*" '
        '-o -type d -iname "*sspm*" -o -type d -iname "*scp*"', root=True)
# DT blob and live config are data, not register probes.
capture('fdt.dtb', 'cat /sys/firmware/fdt', root=True, binary=True)
capture('boot-hash.txt', 'sha256sum /dev/block/by-name/boot', root=True)
manifest = {'time': datetime.datetime.now().astimezone().isoformat(),
            'serial': args.serial, 'device': device, 'records': records,
            'phone_writes': False, 'raw_mmio': False, 'gpu_workload_started': False,
            'firmware_ownership_proven': False}
(args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
