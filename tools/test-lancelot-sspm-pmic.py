#!/usr/bin/env python3
"""ASan/UBSan regression of the unconnected, read-only SSPM PMIC protocol core."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    root = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', root + '/tools/test-lancelot-sspm-pmic.py'], check=True)
    sys.exit(0)

source = ROOT / 'xnu/files/bsd/dev/lancelot_sspm_pmic.c'
header = source.with_suffix('.h')
before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, header)]
with tempfile.TemporaryDirectory(prefix='lancelot-sspm-') as directory:
    for cc in ['cc', 'clang']:
        binary = Path(directory) / ('sspm-' + cc)
        subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I' + str(source.parent),
                        str(ROOT / 'tools/mali-tests/sspm_pmic_test.c'), str(source),
                        '-o', str(binary)], check=True)
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = 'detect_leaks=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        subprocess.run([str(binary)], env=env, check=True)
assert before == [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, header)]
print('SSPM PMIC core GCC/Clang PASS; source hashes unchanged; no XNU backend registered')
