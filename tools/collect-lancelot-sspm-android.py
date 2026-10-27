#!/usr/bin/env python3
"""Collect passive SSPM/PMIC context. Never read active diagnostic attributes.

All output stays on the PC. No devmem, IPI request, phone file, clock/rail change,
firmware update, debugfs mount, reboot or flash. Partition hashes identify stored
images, NOT the running SSPM image or ownership after a different boot chain.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import subprocess


COMMANDS = (
    ('kernel.txt', 'uname -a'),
    ('cmdline.txt', 'cat /proc/cmdline'),
    ('dmesg.raw', 'dmesg'),
    ('interrupts.txt', 'cat /proc/interrupts'),
    ('iomem.txt', 'cat /proc/iomem'),
    ('modules.txt', 'cat /proc/modules'),
    # ls/readlink inspect metadata only: do NOT cat sspm_alive or other attributes.
    ('bindings.txt', 'ls -l /sys/bus/platform/drivers/sspm '
     '/sys/bus/platform/drivers/scp /sys/bus/platform/drivers/mt-pmic-pwrap; '
     'ls -l /sys/devices/platform/*sspm* /sys/devices/platform/*scp*'),
    ('sspm-attribute-names.txt', 'find /sys -maxdepth 5 -iname "*sspm*"'),
    ('fdt.dtb', 'cat /sys/firmware/fdt'),
    ('boot-hash.txt', 'sha256sum /dev/block/by-name/boot'),
    ('sspm-partition-hash.txt', 'sha256sum /dev/block/by-name/sspm_1'),
    ('scp-partition-hash.txt', 'sha256sum /dev/block/by-name/scp1'),
)


class Capture:
    def __init__(self, adb, serial, out):
        self.adb, self.serial, self.out = adb, serial, out
        self.manifest = {
            'time': datetime.datetime.now().astimezone().isoformat(),
            'serial': serial, 'records': [], 'complete': False,
            'phone_writes': False, 'raw_mmio': False, 'ipi_requests': False,
            'read_sspm_alive': False, 'running_firmware_identity_proven': False,
            'xnu_mailbox_handoff_proven': False,
        }

    def save(self):
        (self.out / 'manifest.json').write_text(
            json.dumps(self.manifest, indent=2) + '\n', encoding='utf-8')

    def run(self, name, command, root=True):
        argv = [self.adb, '-s', self.serial, 'exec-out']
        argv += ['su', '-c', command] if root else ['sh', '-c', command]
        try:
            result = subprocess.run(argv, capture_output=True, timeout=40)
            stdout, stderr, status = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, status = error.stdout or b'', error.stderr or b'', None
        (self.out / name).write_bytes(stdout)
        (self.out / (name + '.stderr')).write_bytes(stderr)
        self.manifest['records'].append({
            'file': name, 'remote_command': command, 'root': root,
            'exit': status, 'timed_out': status is None, 'bytes': len(stdout),
            'sha256': hashlib.sha256(stdout).hexdigest(),
            'stderr_bytes': len(stderr), 'stderr_sha256': hashlib.sha256(stderr).hexdigest(),
        })
        self.save()  # Preserve failed/partial captures, not just the happy path.
        print(f'{name}: exit={status} bytes={len(stdout)}', flush=True)
        return status, stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--serial', default='116745f30503')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('Use an empty evidence directory; existing files are never overwritten')
    args.out.mkdir(parents=True, exist_ok=True)
    capture = Capture(args.adb, args.serial, args.out)
    try:
        status, device = capture.run('device.txt', 'getprop ro.product.device', root=False)
        if status != 0 or device.strip() != b'lancelot':
            raise RuntimeError('Not confirmed lancelot; no privileged collection performed')
        status, uid = capture.run('root.txt', 'id')
        if status != 0 or b'uid=0(' not in uid:
            raise RuntimeError('Existing root unavailable; no bypass attempted')
        for name, command in COMMANDS:
            capture.run(name, command)
        capture.manifest['complete'] = all(
            r['exit'] == 0 for r in capture.manifest['records'])
    except (OSError, RuntimeError) as error:
        capture.manifest['error'] = str(error)
    finally:
        capture.save()
    return 0 if capture.manifest['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
