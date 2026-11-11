#!/usr/bin/env python3
"""Build isolated persistent-data init and 20H392 Metal injection from a local reference."""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
if os.name == 'nt':
    script = '/mnt/' + HERE.drive[0].lower() + HERE.as_posix()[2:] + '/build-runtime.py'
    raise SystemExit(subprocess.call(['wsl', '-d', 'Ubuntu', '--exec', 'python3', '-B', '-u', script, *sys.argv[1:]]))

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference', type=Path, default=Path(os.environ.get('LANCELOT_REFERENCE', str(REPO))))
parser.add_argument('--out', type=Path)
parser.add_argument('--prior-init', type=Path, help='optional previously installed init to reproduce before adding Metal')
parser.add_argument('--prior-injection', type=Path, help='optional previously installed libredwood.dylib to reproduce')
args = parser.parse_args()
reference = args.reference.resolve()
compiler = reference / 'xnu-deps/host/bin/xnu-clang'
sdk = reference / 'local-toolchain/theos/sdks/iPhoneOS16.5.sdk'
if not compiler.is_file() or not sdk.is_dir():
    parser.error('reference must contain the XNU compiler and iPhoneOS16.5 SDK')

output = args.out or REPO / 'out/lancelot' / ('metal-runtime-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
output = output.resolve()
output.mkdir(parents=True, exist_ok=False)
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
paths = [reference / 'userland/src' / name for name in ['init.c', 'inject.c', 'inject_sb.c', 'sb_startup_20h392.h', 'redwood_input.h', 'redwood_gl.h']]
paths += [reference / 'userland/libmini' / name for name in ['mini.c', 'mini.h', 'start.S']]
paths += [REPO / 'tools/lancelot-data' / name for name in ['apply-init.py', 'lancelot_data_init.inc']]
paths += [HERE / name for name in ['apply-init.py', 'apply-inject.py', 'src/lancelot_metal_init.inc', 'src/gpu_present.m', 'src/present_policy.h', 'src/present_cache_hook.h', 'apply-springboard.py']]
inputs = {str(path): digest(path) for path in paths}
inputs[str(Path(__file__))] = digest(Path(__file__))
for path in paths:
    group = 'metal' if path.is_relative_to(HERE) else 'data' if path.is_relative_to(REPO / 'tools/lancelot-data') else 'base'
    directory = output / group
    directory.mkdir(exist_ok=True)
    shutil.copy2(path, directory / path.name)
for name in ['mini.c', 'mini.h', 'start.S', 'sb_startup_20h392.h', 'redwood_input.h', 'redwood_gl.h']:
    shutil.copy2(output / 'base' / name, output / name)
shutil.copy2(output / 'data/lancelot_data_init.inc', output / 'lancelot_data_init.inc')
shutil.copy2(output / 'metal/lancelot_metal_init.inc', output / 'lancelot_metal_init.inc')
shutil.copy2(output / 'metal/present_policy.h', output / 'present_policy.h')
flags = ['-arch', 'arm64', '-Os', '-static', '-nostdlib', '-ffreestanding', '-fno-stack-protector',
         '-fno-builtin', '-Wall', '-Wextra', '-I' + str(output)]
runs = []

def run(name, command):
    log = output / (name + '.log')
    with log.open('xb') as stream:
        proc = subprocess.run(list(map(str, command)), cwd=REPO, stdout=stream, stderr=subprocess.STDOUT,
                              timeout=180, env={**os.environ, 'PATH': str(compiler.parent) + ':' + os.environ['PATH']})
    runs.append({'stage': name, 'exit': proc.returncode, 'log': log.name, 'sha256': digest(log)})
    print(name, proc.returncode, flush=True)
    if proc.returncode:
        (output / 'failure.json').write_text(json.dumps({'stage': name, 'inputs': inputs, 'runs': runs,
                                                       'device_access': False}, indent=2) + '\n')
        print(log.read_text(errors='replace')[-5000:], flush=True)
        raise SystemExit(proc.returncode)

run('data-init', ['python3', '-B', output / 'data/apply-init.py', output / 'base/init.c', output / 'data-init.c'])
run('metal-init', ['python3', '-B', output / 'metal/apply-init.py', output / 'data-init.c', output / 'init.c'])
run('inject', ['python3', '-B', output / 'metal/apply-inject.py', output / 'base/inject.c', output / 'inject.c'])
for name in ['mini.c', 'start.S']:
    run(name, [compiler, *flags, '-c', output / name, '-o', output / (name + '.o')])
link = ['-Wl,-e,_start', '-Wl,-pagezero_size,0x100000000']
run('prior-init-build', [compiler, *flags, *link, '-o', output / 'prior-init', output / 'data-init.c',
                         output / 'mini.c.o', output / 'start.S.o'])
if args.prior_init:
    assert digest(output / 'prior-init') == digest(args.prior_init), 'persistent init baseline did not reproduce'
run('init-build', [compiler, *flags, *link, '-o', output / 'init', output / 'init.c',
                   output / 'mini.c.o', output / 'start.S.o'])
run('init-undefined', ['llvm-nm', '--undefined-only', output / 'init'])
assert not (output / 'init-undefined.log').read_text().strip(), 'static init has unresolved imports'

dynamic = ['-arch', 'arm64', '-isysroot', str(sdk), '-O1', '-dynamiclib', '-install_name',
           '/redwood/lib/libredwood.dylib', '-framework', 'CoreFoundation', '-framework', 'IOKit',
           '-lobjc', '-I' + str(output)]
run('prior-inject-build', [compiler, *dynamic, '-o', output / 'prior-inject.dylib', output / 'base/inject.c'])
if args.prior_injection:
    assert digest(output / 'prior-inject.dylib') == digest(args.prior_injection), '20H392 injection baseline did not reproduce'
run('inject-build', [compiler, *dynamic, '-fblocks', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
    '-o', output / 'libredwood.dylib', output / 'inject.c', output / 'metal/gpu_present.m'])
run('springboard-patch',['python3','-B',output/'metal/apply-springboard.py',output/'base/inject_sb.c',output/'inject_sb.c'])
sbflags=list(dynamic);sbflags[sbflags.index('/redwood/lib/libredwood.dylib')]='/redwood/lib/libredwood-sb.dylib'
run('springboard-build',[compiler,*sbflags,'-o',output/'libredwood-sb.dylib',output/'inject_sb.c'])
assert all(digest(Path(path)) == expected for path, expected in inputs.items())

def metadata(path):
    return {'path': path.relative_to(REPO).as_posix(), 'bytes': path.stat().st_size, 'sha256': digest(path)}

record = {'passed': True, 'inputs': inputs, 'runs': runs, 'init': metadata(output / 'init'),
          'injection': metadata(output / 'libredwood.dylib'),
          'springboard_injection':metadata(output/'libredwood-sb.dylib'),
          'prior_init_reproduced': True if args.prior_init else None,
          'prior_injection_reproduced': True if args.prior_injection else None,
          'existing_20H392_hooks_preserved': True, 'original_reference_sources_unchanged': True,
          'device_access': False, 'deployed': False}
(output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
print('RUNTIME_READY', output / 'result.json', flush=True)
