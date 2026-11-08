from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,subprocess,sys,shutil
P=Path(__file__).resolve().parent;R=P.parent
if os.name=='nt':
 path='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/' + Path(__file__).name
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',path]))
E=R/'out/lancelot/tests';E.mkdir(parents=True,exist_ok=True)
O=E/('msdc-write-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir()
sources=[R/'xnu/files/bsd/dev/lancelot_msdc.h',R/'xnu/files/bsd/dev/lancelot_mmc_mode.h']
pins={p.relative_to(R).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
for p in sources:shutil.copy2(p,O/p.name)
shutil.copy2(R/'tools/lancelot-data/tests/msdc_write_test.c',O/'write.c')
(O/'read.c').write_text((R/'tools/test-lancelot-msdc.c').read_text().replace('../xnu/files/bsd/dev/lancelot_mmc_mode.h','lancelot_mmc_mode.h'))
runs=[];passed=True
for cc in ['gcc','clang']:
 for source in ['write','read']:
  exe=O/(cc+'-'+source)
  for name,cmd in [(cc+'-'+source+'-build',[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(O),
    '-Wno-unused-function','-fsanitize=address,undefined',O/(source+'.c'),'-o',exe]),(cc+'-'+source+'-run',[exe])]:
   p=subprocess.run([str(x) for x in cmd],capture_output=True,timeout=90)
   (O/(name+'.log')).write_bytes(p.stdout+p.stderr);runs.append({'name':name,'exit':p.returncode})
   print(name,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-2200:],flush=True)
   if p.returncode:passed=False;break
  if not passed:break
 if not passed:break
assert all(hashlib.sha256((R/n).read_bytes()).hexdigest()==h for n,h in pins.items())
(O/'result.json').write_text(json.dumps({'passed':passed,'sources':pins,'runs':runs,
 'actual_production_read_write_transport':True,'register_model':True,'device_write':False},indent=2))
print(O,flush=True);raise SystemExit(0 if passed else 1)
