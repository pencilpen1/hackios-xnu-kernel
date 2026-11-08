"""Compile the actual independent sampler with controlled scheduler/clock inputs."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,shutil,subprocess,sys
P=Path(__file__).resolve().parent;R=P.parent
if os.name=='nt':
 p='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/' + Path(__file__).name
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',p]))
E=R/'out/lancelot/tests';E.mkdir(parents=True,exist_ok=True)
O=E/('cpu-sampler-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir()
source=R/'xnu/files/bsd/dev/lancelot_mali.c';text=source.read_text()
a=text.index('static void cpu_metrics_tick(uint64_t now)');b=text.index('static void cpu_metrics_sample(',a)
body=text[a:b].replace('__asm__ volatile("dsb sy":::"memory")','host_barrier()')
assert 'dsb sy' not in body
pins={source.relative_to(R).as_posix():hashlib.sha256(source.read_bytes()).hexdigest()}
pins[Path(__file__).relative_to(R).as_posix()]=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
for name in ['lancelot_mali_metrics.h','lancelot_mali_cpu_profile.h']:
 p=R/'xnu/files/bsd/dev'/name;shutil.copy2(p,O/name);pins[p.relative_to(R).as_posix()]=hashlib.sha256(p.read_bytes()).hexdigest()
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "lancelot_mali_metrics.h"
#include "lancelot_mali_cpu_profile.h"
#define NSEC_PER_SEC 1000000000ull
#define CPU_STATE_USER 0
#define CPU_STATE_SYSTEM 1
#define CPU_STATE_IDLE 2
#define CPU_STATE_NICE 3
#define PROCESSOR_CPU_LOAD_INFO_COUNT 4
#define PROCESSOR_CPU_LOAD_INFO 1
#define KERN_INVALID_ARGUMENT 4
#define LMP_BANK_APMIXED 3
typedef void*host_t;typedef unsigned*processor_info_t;typedef unsigned mach_msg_type_number_t;
struct processor_cpu_load_info{uint32_t cpu_ticks[4];};
struct fake_cpu{unsigned id;uint32_t ticks[4];};typedef struct fake_cpu*processor_t;
static struct fake_cpu cores[8];static processor_t processor_array[8];
typedef struct{uint32_t cpu_id,phys_id,cluster_id,cluster_type,l2_access_penalty;}ml_topology_cpu_t;
typedef struct{unsigned num_cpus;ml_topology_cpu_t cpus[8];}ml_topology_info_t;
static ml_topology_info_t topology;static const ml_topology_info_t*ml_get_topology_info(void){return &topology;}
static int failed_cpu=-1;static int processor_info(processor_t p,int flavor,host_t*h,processor_info_t out,mach_msg_type_number_t*n){
 assert(flavor==1);*h=NULL;*n=4;if((int)p->id==failed_cpu)return 4;memcpy(out,p->ticks,16);return 0;}
static uint32_t cpu_previous[8][4];static uint64_t cpu_previous_at[8];
static struct lancelot_cpu_profile cpu_profile;static pthread_mutex_t cpu_metrics_lock=PTHREAD_MUTEX_INITIALIZER;
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
static uint32_t cfg_space[256];static uintptr_t cpu_clock_bank;
static unsigned register_reads[2],barriers;static bool unstable;
static void host_barrier(void){barriers++;}
static int read_bank(void*ctx,unsigned bank,unsigned offset,uint32_t*out){
 assert(!ctx&&bank==LMP_BANK_APMIXED);unsigned cluster=offset<0x210?1:0;
 assert(offset==0x208||offset==0x20c||offset==0x218||offset==0x21c);
 if(!(offset&4))*out=1;
 else{register_reads[cluster]++;*out=cluster?1228800:819200;if(unstable&&!cluster&&register_reads[cluster]==2)(*out)++;}
 return 0;
}
static unsigned topology_lines,load_lines,clock_lines;
static int capture_printf(const char *format,...){
 if(strstr(format,"lancelot-cpu-topology:")==format)topology_lines++;
 if(strstr(format,"lancelot-cpu-load:")==format)load_lines++;
 if(strstr(format,"lancelot-cpu-clock:")==format)clock_lines++;
 return 0;
}
#define printf capture_printf
'''
post=r'''
int main(void){topology.num_cpus=8;cpu_clock_bank=(uintptr_t)cfg_space;
cfg_space[0x2a0/4]=cfg_space[0x2a4/4]=1u<<9;
for(unsigned i=0;i<8;i++){cores[i].id=i;processor_array[i]=&cores[i];topology.cpus[i]=(ml_topology_cpu_t){i,i*0x100,i>=6,0,0};}
cpu_metrics_tick(100000000);assert(!cpu_profile.valid_mask&&cpu_profile.hz_valid_mask==3);
assert(topology_lines==8&&load_lines==8&&clock_lines==2);
assert(cpu_profile.hz[0]==1300000000ull&&cpu_profile.hz[1]==1950000000ull&&cpu_profile.domain[6]==1&&cpu_profile.domain[5]==0);
for(unsigned i=0;i<8;i++){cores[i].ticks[0]=7;cores[i].ticks[1]=3;cores[i].ticks[2]=10;}
cores[1].ticks[0]=cores[1].ticks[1]=cores[1].ticks[2]=0;failed_cpu=3;
cpu_metrics_tick(350000000);assert((cpu_profile.valid_mask&3)==1&&!(cpu_profile.valid_mask&(1u<<3))&&cpu_profile.busy_bps[0]==5000);
assert(cpu_profile.from_ns[0]==100000000&&cpu_profile.sampled_ns==350000000);
unstable=true;register_reads[0]=register_reads[1]=0;failed_cpu=-1;
cpu_metrics_tick(2000000000);assert(cpu_profile.hz_valid_mask==2&&!cpu_profile.hz[0]&&cpu_profile.hz[1]==1950000000ull);
assert(topology_lines==8&&load_lines==16&&clock_lines==4);
topology.cpus[1].cluster_id=2;cpu_metrics_tick(3100000000);
assert(topology_lines==16&&load_lines==24&&clock_lines==6);
topology.num_cpus=7;cpu_metrics_tick(4200000000);
assert(topology_lines==23&&load_lines==32&&clock_lines==8&&cpu_profile.domain[7]==-1);
assert(barriers>0);puts("PASS actual sampler: unchanged topology suppressed, changed topology reported, load/clock cadence retained, MPIDR domains and unstable clocks checked; reads only");}
'''
(O/'test.c').write_text(pre+body+post);runs=[];passed=True
for cc in ['gcc','clang']:
 exe=O/cc
 for name,cmd in [(cc+'-build',[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-format',
  '-fsanitize=address,undefined','-pthread','-I'+str(O),O/'test.c','-o',exe]),(cc+'-run',[exe])]:
  p=subprocess.run([str(x) for x in cmd],capture_output=True,timeout=90);log=O/(name+'.log');log.write_bytes(p.stdout+p.stderr)
  runs.append({'name':name,'exit':p.returncode,'log':log.name,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
  print(name,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-1700:],flush=True)
  if p.returncode:passed=False;break
 if not passed:break
assert all(hashlib.sha256((R/n).read_bytes()).hexdigest()==h for n,h in pins.items())
with (O/'result.json').open('x') as f:json.dump({'passed':passed,'sources':pins,'runs':runs,
 'actual_independent_CPU_sampler_extracted':True,'scheduler_clock_inputs_and_ARM_barriers_modeled':True,'device_access':False},f,indent=2)
print(O,flush=True);raise SystemExit(0 if passed else 1)
