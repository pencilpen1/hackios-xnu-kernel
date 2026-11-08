#!/usr/bin/env python3
"""Build the repository's MT6768 firmware overlay in a fresh source directory."""
from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, os, re, subprocess, sys, shutil

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    script = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:] + '/tools/build-lancelot-uefi.py'
    args = [('/mnt/' + value[0].lower() + value[2:].replace('\\', '/'))
            if re.match(r'^[A-Za-z]:[\\/]', value) else value for value in sys.argv[1:]]
    raise SystemExit(subprocess.call(['wsl', '-d', 'Ubuntu', '--exec', 'python3', '-B', script, *args]))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mu-source', type=Path, required=True)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
out = args.out or ROOT / 'out/lancelot' / ('mu-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
assert args.mu_source.resolve() != out.resolve() and args.mu_source.resolve() not in out.resolve().parents
out.mkdir(parents=True, exist_ok=False)
source = out / 'source'
pinned_source = out / 'pinned-base'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
runs = []

def run(name, command, cwd):
    log = out / (name + '.log')
    with log.open('xb') as file:
        result = subprocess.run(list(map(str, command)), cwd=cwd, stdout=file, stderr=subprocess.STDOUT,
                                env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
    runs.append({'name': name, 'exit': result.returncode, 'log': log.name, 'sha256': sha(log)})
    print(name, result.returncode, flush=True)
    if result.returncode:
        raise SystemExit('Firmware build failed; evidence retained in ' + str(out))

run('clone', ['git', 'clone', '--local', '--no-hardlinks', '--no-checkout', args.mu_source, pinned_source], ROOT)
run('checkout', ['git', '-C', pinned_source, 'checkout', '--detach', '83f641fd2a642968aac4fb36986956db7076118e'], ROOT)
run('overlay', ['python3', '-B', ROOT / 'tools/apply-lancelot-fw-lease.py',
                '--source', pinned_source, '--dest', source, '--dependencies',
                '--dependencies-source', args.mu_source], ROOT)
run('build', ['python3', '-B', source / 'Platforms/Xiaomi/lancelotPkg/DeviceBuild.py',
              'TARGET=RELEASE', 'ENABLE_SECUREBOOT=0', 'FD_BASE=0x40080000', 'FD_SIZE=0x200000',
              'FD_BLOCKS=0x200', 'DEVICE_MODEL=0'], source)
fd = source / 'Build/lancelotPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd'
assert fd.is_file() and fd.stat().st_size == 0x200000
artifact = out / 'SILICIUM_UEFI.fd'
shutil.copy2(fd, artifact)
with (out / 'result.json').open('x', encoding='utf-8') as file:
    json.dump({'passed': True, 'source_tree': str(source), 'runs': runs,
               'overlay': json.loads((source / 'gpu-lease-source-audit.json').read_text(encoding='utf-8')),
               'firmware': {'path': str(artifact), 'bytes': artifact.stat().st_size, 'sha256': sha(artifact)},
               'device_access': False, 'deployed': False}, file, indent=2)
print('MT6768 firmware ready:', artifact, flush=True)
