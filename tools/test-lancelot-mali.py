#!/usr/bin/env python3
"""Run imported fixtures against the actual main-project Mali production sources."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    root = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', root + '/tools/test-lancelot-mali.py'], check=True)
    sys.exit(0)
inputs = list((ROOT / 'xnu/files/bsd/dev').glob('lancelot_mali*'))
inputs += list((ROOT / 'xnu/files/iokit/Kernel').glob('*Mali*'))
inputs += list((ROOT / 'xnu/files/iokit/Kernel').glob('lancelot_mali*'))
hashes = lambda: {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
before = hashes()
test = ROOT / 'tools/mali-tests'
with tempfile.TemporaryDirectory(prefix='lancelot-mali-') as directory:
    out = Path(directory)
    cc = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
    commands = [
        ('smoke-build', cc + ['-I' + str(ROOT / 'xnu/files/bsd/dev'),
                             str(test / 'smoke_test.c'), '-o', str(out / 'smoke')]),
        ('smoke', [str(out / 'smoke')]),
        ('platform-build', cc + [str(test / 'platform_test.c'), '-o', str(out / 'platform')]),
        ('platform', [str(out / 'platform')]),
        ('core-build', cc + ['-DLM_HOST_TEST', '-I' + str(ROOT / 'xnu/files/bsd/dev'),
                            str(test / 'core_test.c'), str(ROOT / 'xnu/files/bsd/dev/lancelot_mali_core.c'), '-o', str(out / 'core')]),
        ('core', [str(out / 'core')]),
        ('core-quarantine', [str(out / 'core'), 'quarantine']),
        ('core-mmio', [str(out / 'core'), 'mmio']),
        ('private-dma-quarantine', [str(out / 'core'), 'private-quarantine']),
        ('private-clear', [str(out / 'core'), 'private-clear']),
        ('client-build', ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                          '-fsanitize=address,undefined', str(test / 'client_logic_host.c'), '-o', str(out / 'client')]),
        ('client', [str(out / 'client')]),
        ('export-build', ['c++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                          '-fsanitize=address,undefined', '-I' + str(test / 'stubs'),
                          '-I' + str(ROOT / 'xnu/files/iokit/Kernel'), str(test / 'client_export_host.cpp'), '-o', str(out / 'export')]),
        ('export', [str(out / 'export')]),
    ]
    for name, command in commands:
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = 'detect_leaks=0' if name in ['core-quarantine', 'core-mmio', 'private-dma-quarantine'] else 'detect_leaks=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        print(name, flush=True)
        subprocess.run(command, cwd=ROOT, env=env, check=True)
assert hashes() == before
print('Main-project core/platform/client/export regressions PASS; production source hashes unchanged; no hardware accessed')
