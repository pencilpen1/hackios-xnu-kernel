from pathlib import Path
from datetime import datetime,timezone
import argparse,hashlib,json,os,shutil,subprocess,sys
P=Path(__file__).resolve().parent;R=P.parent
if os.name=='nt':
 p='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/' + Path(__file__).name
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',p,*sys.argv[1:]]))
E=R/'out/lancelot/tests';E.mkdir(parents=True,exist_ok=True)
O=E/('data-init-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
parser=argparse.ArgumentParser();parser.add_argument('--mini-header',type=Path,default=Path(os.environ.get('MINI_HEADER',str(R/'userland/libmini/mini.h'))));args=parser.parse_args()
paths=[R/'tools/lancelot-data/lancelot_data_init.inc',args.mini_header]
if not args.mini_header.is_file():raise SystemExit('Supply --mini-header from the configured hackios-xnu-userland checkout')
pins={str(p):sha(p) for p in paths}
for p in paths:shutil.copy2(p,O/p.name)
shutil.copy2(R/'tools/lancelot-data/tests/init_test.c',O/'test.c');runs=[];passed=True
for cc in ['gcc','clang']:
 exe=O/cc
 for name,cmd in [(cc+'-build',[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fsanitize=address,undefined',
    '-I'+str(O),O/'test.c','-o',exe]),(cc+'-run',[exe])]:
  p=subprocess.run([str(x) for x in cmd],capture_output=True,timeout=90);log=O/(name+'.log');log.write_bytes(p.stdout+p.stderr)
  runs.append({'name':name,'exit':p.returncode,'log':log.name,'sha256':sha(log)})
  print(name,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-2400:],flush=True)
  if p.returncode:passed=False;break
 if not passed:break
assert all(sha(Path(n))==h for n,h in pins.items())
with (O/'result.json').open('x') as f:json.dump({'passed':passed,'sources':pins,'runs':runs,
 'actual_persistent_init_helpers':True,'Darwin_syscalls_and_persistence_modeled':True,'device_access':False},f,indent=2)
print(O,flush=True);raise SystemExit(0 if passed else 1)
