#!/usr/bin/env python3
"""Build the public MTK Metal core/plugin in a new, SHA-bound output directory."""
from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, os, shutil, subprocess, sys

P = Path(__file__).resolve().parent
R = P.parents[1]
if os.name == 'nt':
    path = '/mnt/' + P.drive[0].lower() + P.as_posix()[2:] + '/build.py'
    raise SystemExit(subprocess.call(['wsl', '-d', 'Ubuntu', '--exec', 'python3', '-B', path, *sys.argv[1:]]))
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--reference', type=Path, default=Path(os.environ.get('LANCELOT_REFERENCE', str(R))))
ap.add_argument('--out', type=Path)
a = ap.parse_args()
W = a.reference
SDK = W / 'local-toolchain/theos/sdks/iPhoneOS16.5.sdk'
assert SDK.is_dir()
O = a.out or R / 'out/lancelot' / (
    'metal-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
O = O.resolve()
O.mkdir(parents=True, exist_ok=False)
(O / 'obj').mkdir()
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
core = sorted((P / 'src/metal').glob('*.cpp'))
plugin = P / 'src/redwoodmetal.m'
headers = [p for p in (P / 'src').rglob('*.h')]
headers.append(R / 'tools/mesa-lancelot/air/air_metal.h')
headers += [R/'xnu/files/iokit/Kernel/lancelot_mali_uapi.h', R/'xnu/files/iokit/Kernel/lancelot_mali_queue_uapi.h']
runtime = [P/'src/mps_runtime.m', P/'src/ca_runtime.m', P/'src/lancelot_metal_test.m', P/'src/metal_props.c', P/'src/lancelot_metal_init.inc']
shaders = sorted((P/'tests/shaders').glob('*.ll')) + sorted((P/'src/shaders').glob('*.ll'))
recipes = [Path(__file__), P/'build-shaders.py', P/'apply-init.py', P/'apply-inject.py']
inputs = {p.relative_to(R).as_posix(): sha(p) for p in core + headers + runtime + shaders + recipes + [plugin, P / 'src/metal_plugin.plist']}
frozen = O/'source'
for name, expected in inputs.items():
    dest=frozen/name;dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(R/name,dest);assert sha(dest)==expected
source=frozen/'tools/lancelot-metal'
with (O / 'inputs.json').open('x') as f:
    json.dump(inputs, f, indent=2)
CC = W / 'xnu-deps/host/bin/xnu-clang'
CXX = W / 'xnu-deps/host/bin/xnu-clang++'
common = ['-arch', 'arm64', '-isysroot', str(SDK), '-Os', '-fblocks',
          '-I' + str(frozen / 'tools/mesa-lancelot/air'), '-I' + str(source / 'src'),
          '-I' + str(frozen / 'xnu/files/iokit/Kernel')]
runs = []
def run(name, command):
    log = O / (name + '.log')
    with log.open('xb') as f:
        p = subprocess.run(list(map(str, command)), cwd=R, stdout=f, stderr=subprocess.STDOUT,
                           env={**os.environ, 'PATH': str(CC.parent) + ':' + os.environ['PATH']}, timeout=180)
    runs.append({'name': name, 'exit': p.returncode, 'log': log.name, 'sha256': sha(log)})
    print(name, p.returncode, flush=True)
    if p.returncode:
        print('\n'.join(log.read_text(errors='replace').splitlines()[-45:]), flush=True)
        with (O / 'failure.json').open('x') as f:
            json.dump({'stage': name, 'runs': runs, 'GPU_complete': False}, f, indent=2)
        raise SystemExit(p.returncode)
objects = []
for p in core:
    obj = O / 'obj' / (p.stem + '.o')
    run(p.stem, [CXX, *common, '-std=c++20', '-stdlib=libc++', '-fno-exceptions', '-fno-rtti', '-c', source/'src/metal'/p.name, '-o', obj])
    objects.append(obj)
bundle = O / 'RedwoodMetal.bundle'
bundle.mkdir()
binary = bundle / 'RedwoodMetal'
run('plugin', [CC, *common, '-bundle', '-framework', 'Foundation', '-framework', 'IOKit',
               '-framework', 'IOSurface', '-framework', 'Metal', '-lobjc', '-lc++',
               '-o', binary, source/'src/redwoodmetal.m', source/'src/mps_runtime.m', *objects])
shutil.copy2(source / 'src/metal_plugin.plist', bundle / 'Info.plist')
helper=O/'liblancelot-metal-ca.dylib'
run('ca-runtime',[CC,*common,'-dynamiclib','-framework','Foundation','-framework','Metal',
    '-lobjc','-install_name','/redwood/lib/liblancelot-metal-ca.dylib','-o',helper,source/'src/ca_runtime.m'])
test=O/'lancelot_metal_test'
run('metal-test',[CC,*common,'-framework','Foundation','-framework','Metal','-framework','IOKit','-framework','IOSurface','-lobjc',
    '-o',test,source/'src/lancelot_metal_test.m'])
props=O/'metal_props'
run('metal-props',[CC,*common,'-framework','CoreFoundation','-framework','IOKit','-o',props,source/'src/metal_props.c'])
run('test-shaders',['python3','-B',source/'build-shaders.py','--out',O/'shaders'])
run('effect-shaders',['python3','-B',source/'build-shaders.py','--effects','--out',O/'effects'])
assert all(sha(R / n) == h for n, h in inputs.items())
result = {'passed': True, 'inputs': inputs, 'runs': runs,
          'plugin': {'path': binary.relative_to(R).as_posix(), 'bytes': binary.stat().st_size, 'sha256': sha(binary)},
          'bundle_info': {'path': (bundle / 'Info.plist').relative_to(R).as_posix(), 'sha256': sha(bundle / 'Info.plist')},
          'public_MTK_core_files_compiled': len(core), 'runtime_20H392_verified': False,
          'ca_helper':{'path':helper.relative_to(R).as_posix(),'bytes':helper.stat().st_size,'sha256':sha(helper)},
          'device_test':{'path':test.relative_to(R).as_posix(),'bytes':test.stat().st_size,'sha256':sha(test)},
          'discovery_tool':{'path':props.relative_to(R).as_posix(),'bytes':props.stat().st_size,'sha256':sha(props)},
          'test_shaders':{'path':(O/'shaders/lancelot-test.metallib').relative_to(R).as_posix(),
              'sha256':sha(O/'shaders/lancelot-test.metallib'),'bytes':(O/'shaders/lancelot-test.metallib').stat().st_size},
          'effect_shaders':{'path':(O/'effects/lancelot-effects.metallib').relative_to(R).as_posix(),'sha256':sha(O/'effects/lancelot-effects.metallib')},
          'source_snapshot':str(frozen), 'fixed_20F66_device_test_addresses':False,
          'CoreAnimation_Metal_verified': False, 'GPU_complete': False,'deployed':False}
with (O / 'result.json').open('x') as f:
    json.dump(result, f, indent=2)
print('Built public MTK Metal core and plugin:', O, flush=True)
