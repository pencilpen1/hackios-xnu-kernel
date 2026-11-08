#!/usr/bin/env python3
"""Register the owned CPU-lease VM proof C unit in a generated XNU tree.

Windows xnu/files remains authoritative. This changes only the generated OSFMK
file list; it never rewrites redwood.patch, driver/core code or a device.
"""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('tree', type=Path)
args = parser.parse_args()
path = args.tree / 'osfmk/conf/files.arm64'
text = path.read_text()
unit = 'osfmk/arm64/lancelot_mali_cpu_lease_vm.c'
line = unit + '\t\toptional iokitcpp\n'
matches = [entry for entry in text.splitlines() if unit in entry]
if matches:
    if matches != [line.rstrip('\n')]:
        raise SystemExit('CPU lease VM registration differs; refusing to overwrite')
else:
    anchor = 'osfmk/arm64/machine_task.c\t\tstandard\n'
    if text.count(anchor) != 1:
        raise SystemExit('XNU8796 OSFMK machine_task registration anchor differs')
    path.write_text(text.replace(anchor, anchor + line, 1))
print('Registered owned CPU-lease VM proof:', path)
