"""Build actual DRM completion paths and the extracted native timing query."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,shutil,subprocess,sys
P=Path(__file__).resolve().parent;R=P.parent
if os.name=='nt':
 p='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/' + Path(__file__).name
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',p]))
E=R/'out/lancelot/tests';E.mkdir(parents=True,exist_ok=True)
O=E/('completion-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
D=R/'tools/mesa-lancelot/lancelot-drm';sources={}
for p in D.rglob('*'):
 if p.is_file():
  target=O/p.relative_to(D);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
  sources[p.relative_to(R).as_posix()]=sha(p)
for name in ['lancelot_mali_uapi.h','lancelot_mali_queue_uapi.h']:
 p=R/'xnu/files/iokit/Kernel'/name;shutil.copy2(p,O/name);sources[p.relative_to(R).as_posix()]=sha(p)
shutil.copy2(R/'tools/mali-tests/completion_test.c',O/'completion-test.c')
native=R/'xnu/files/bsd/dev/lancelot_hakios_mali.inc';s=native.read_text()
a=s.index('int lancelot_hakios_mali_profile(uint64_t sequence',s.index('int lancelot_hakios_mali_profile(uint64_t sequence')+1)
b=s.index('\n}\n',a)+3
query=s[a:b];sources[native.relative_to(R).as_posix()]=sha(native)
(O/'native-query.c').write_text('''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#define LM_OK 0
#define LM_EINVAL (-22)
#define LM_EBUSY (-16)
static pthread_mutex_t mali_queue_lock=PTHREAD_MUTEX_INITIALIZER;
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
struct hkmt_completed_profile {uint64_t sequence,start_ns,end_ns,jm_ns,worker_ns;uint32_t hz;int32_t error;};
static struct hkmt_completed_profile hkmt_profiles[1024];
static uint64_t mali_queued_seq,mali_done_seq;
'''+query+'''
int main(void){uint64_t times[4]={1,2,3,4};uint32_t hz=5;int32_t error=6;
assert(lancelot_hakios_mali_profile(0,times,&hz,&error)==LM_EINVAL);
mali_queued_seq=2;mali_done_seq=1;
assert(lancelot_hakios_mali_profile(2,times,&hz,&error)==LM_EBUSY&&times[0]==1&&hz==5&&error==6);
assert(lancelot_hakios_mali_profile(3,times,&hz,&error)==LM_EINVAL);
hkmt_profiles[2]=(struct hkmt_completed_profile){2,10,30,18,21,850000000,-5};mali_done_seq=2;
assert(!lancelot_hakios_mali_profile(2,times,&hz,&error)&&times[0]==10&&times[1]==30&&times[2]==18&&times[3]==21&&hz==850000000&&error==-5);
hkmt_profiles[2].sequence=1026;mali_queued_seq=mali_done_seq=1026;
assert(lancelot_hakios_mali_profile(2,times,&hz,&error)==LM_EINVAL);
assert(lancelot_hakios_mali_profile(1026,0,&hz,&error)==LM_EINVAL);
puts("PASS actual native timing query: pending/future/retained-error/overwritten/null outputs");}
''')
runs=[];passed=True
for cc in ['gcc','clang']:
 for source in ['completion-test','native-query']:
  exe=O/(cc+'-'+source)
  for name,cmd in [(cc+'-'+source+'-build',[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
      '-Wno-unused-function','-fsanitize=address,undefined','-I'+str(O),O/(source+'.c'),'-o',exe]),(cc+'-'+source+'-run',[exe])]:
   p=subprocess.run([str(x) for x in cmd],capture_output=True,timeout=90,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
   log=O/(name+'.log');log.write_bytes(p.stdout+p.stderr)
   runs.append({'name':name,'exit':p.returncode,'log':log.name,'sha256':sha(log)})
   print(name,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-2400:],flush=True)
   if p.returncode:passed=False;break
  if not passed:break
 if not passed:break
assert all(sha(R/n)==h for n,h in sources.items())
with (O/'result.json').open('x') as f:json.dump({'passed':passed,'sources':sources,'runs':runs,
 'actual_DRM_source':True,'native_query_extracted':True,'native_worker_execution_modeled':True,'device_access':False},f,indent=2)
print(O,flush=True);raise SystemExit(0 if passed else 1)
