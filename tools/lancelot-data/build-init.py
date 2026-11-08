"""Build an isolated static init; preserve all original userland sources."""
from pathlib import Path
from datetime import datetime,timezone
import argparse,hashlib,json,os,re,shutil,subprocess,sys
P=Path(__file__).resolve().parent;R=P.parents[1]
if os.name=='nt':
 p='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/build-init.py'
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',p,*sys.argv[1:]]))
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--reference',type=Path,default=Path(os.environ.get('LANCELOT_REFERENCE',str(R))));parser.add_argument('--out',type=Path);parser.add_argument('--prior-init',type=Path);args=parser.parse_args()
W=args.reference;O=args.out or R/'out/lancelot'/('init-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();inputs={};runs=[]
paths=[W/'userland/src/init.c',W/'userland/libmini/mini.c',W/'userland/libmini/mini.h',W/'userland/libmini/start.S',
 R/'tools/lancelot-data/apply-init.py',R/'tools/lancelot-data/lancelot_data_init.inc']
for p in paths:
 inputs[str(p)]=sha(p)
 target=O/('base-init.c' if p.name=='init.c' else p.name);shutil.copy2(p,target)
cc=W/'xnu-deps/host/bin/xnu-clang';flags=['-arch','arm64','-Os','-static','-nostdlib','-ffreestanding',
 '-fno-stack-protector','-fno-builtin','-Wall','-Wextra','-I'+str(O)]
def run(name,args):
 p=subprocess.run([str(x) for x in args],capture_output=True,timeout=180);log=O/(name+'.log');log.write_bytes(p.stdout+p.stderr)
 runs.append({'name':name,'exit':p.returncode,'log':log.name,'sha256':sha(log)})
 print(name,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-2000:],flush=True)
 if p.returncode:
  with (O/'failure.json').open('x') as f:json.dump({'stage':name,'runs':runs,'inputs':inputs,'device_access':False},f,indent=2)
  raise SystemExit(p.returncode)
run('overlay',['python3','-B',O/'apply-init.py',O/'base-init.c',O/'init.c'])
run('mini',[cc,*flags,'-c',O/'mini.c','-o',O/'mini.o'])
run('start',[cc,*flags,'-c',O/'start.S','-o',O/'start.o'])
link=['-Wl,-e,_start','-Wl,-pagezero_size,0x100000000']
run('base-build',[cc,*flags,*link,'-o',O/'base-init',O/'base-init.c',O/'mini.o',O/'start.o'])
run('init-build',[cc,*flags,*link,'-o',O/'init',O/'init.c',O/'mini.o',O/'start.o'])
run('init-undefined',['llvm-nm','--undefined-only',O/'init'])
assert not (O/'init-undefined.log').read_text().strip(),'static init unresolved imports'
old=args.prior_init
if old is not None:assert old.is_file()
for p in paths:assert sha(p)==inputs[str(p)]
with (O/'result.json').open('x') as f:json.dump({'passed':True,'inputs':inputs,'runs':runs,
 'init':{'path':(O/'init').relative_to(R).as_posix(),'bytes':(O/'init').stat().st_size,'sha256':sha(O/'init')},
 'base_init_sha256':sha(O/'base-init'),'deployed_old_init_sha256':sha(old) if old is not None else None,
 'base_build_matches_old_init':sha(O/'base-init')==sha(old) if old is not None else None,'original_userland_unchanged':True,
 'overlay_init_source_sha256':sha(O/'init.c'),'device_access':False},f,indent=2)
print('PASS',O,flush=True)
