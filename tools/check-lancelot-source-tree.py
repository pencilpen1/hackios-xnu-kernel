#!/usr/bin/env python3
"""Compare every shipped overlay against the actual XNU build tree."""
from pathlib import Path
import argparse, hashlib, json

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('tree', type=Path)
parser.add_argument('--receipt', type=Path)
args = parser.parse_args()
sources, different = {}, []
for source in sorted((ROOT / 'xnu/files').rglob('*')):
    if not source.is_file():
        continue
    relative = source.relative_to(ROOT / 'xnu/files')
    target = args.tree / relative
    data = source.read_bytes()
    sources[relative.as_posix()] = hashlib.sha256(data).hexdigest()
    if not target.is_file() or target.read_bytes() != data:
        different.append(relative.as_posix())
if different:
    parser.exit(1, 'XNU source tree differs from repository overlays:\n' + '\n'.join(different) + '\n')
registration = args.tree / 'osfmk/conf/files.arm64'
unit = 'osfmk/arm64/lancelot_mali_cpu_lease_vm.c'
if registration.read_text(encoding='utf-8').count(unit) != 1:
    parser.exit(1, 'CPU lease VM unit must be registered exactly once\n')
result = {'passed': True, 'source_count': len(sources), 'overlay_SHA256': sources,
          'CPU_lease_registered_once': True, 'device_access': False}
if args.receipt:
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    with args.receipt.open('x', encoding='utf-8') as file:
        json.dump(result, file, indent=2)
print(f'All {len(sources)} repository overlays match XNU build sources; CPU lease registration verified')
