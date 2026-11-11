#!/usr/bin/env python3
"""Execute the exact production AIR shader math, with strict host textures."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
 def linux(s):return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
P=R/'tools/lancelot-metal';sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
paths=[P/'src/shaders'/n for n in ['gaussian.ll','scale.ll']]+[P/'tests'/n for n in ['effects_host.c','effects_bridge.ll','present_policy_test.c']]+[P/'src/present_policy.h',Path(__file__)]
inputs={str(p.relative_to(R)):sha(p) for p in paths};runs=[]
def run(name,cmd):
 with (O/(name+'.log')).open('wb') as f:r=subprocess.run(list(map(str,cmd)),stdout=f,stderr=subprocess.STDOUT,timeout=90)
 runs.append({'stage':name,'exit':r.returncode});print(name,r.returncode,flush=True)
 if r.returncode:print((O/(name+'.log')).read_text());raise SystemExit(r.returncode)
run('host-bitcode',['clang','-std=c11','-O1','-g','-emit-llvm','-c',P/'tests/effects_host.c','-o',O/'host.bc'])
for name,path in [('gaussian',paths[0]),('scale',paths[1]),('bridge',P/'tests/effects_bridge.ll')]:run(name,['llvm-as',path,'-o',O/(name+'.bc')])
run('link-ir',['llvm-link',O/'host.bc',O/'bridge.bc',O/'gaussian.bc',O/'scale.bc','-o',O/'effects.bc'])
run('compile-ir',['clang','-O1','-fsanitize=address,undefined',O/'effects.bc','-lm','-o',O/'effects'])
run('execute-shaders',[O/'effects'])
for cc in ['clang','gcc']:
 run(cc+'-policy-build',[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(P/'src'),P/'tests/present_policy_test.c','-o',O/(cc+'-policy')])
 run(cc+'-policy',[O/(cc+'-policy')])
assert all(sha(R/n)==h for n,h in inputs.items())
(O/'result.json').write_text(json.dumps({'passed':True,'sources':inputs,'runs':runs,'exact_production_shader_IR_executed':True,'texture_callbacks_are_host_models':True,'hardware_verified':False},indent=2)+'\n')
