"""Exercise cache-call redirection and the actual timeout-state snapshot helper."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
 def linux(s):return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
lease=R/'xnu/files/iokit/Kernel/LancelotMaliCpuLease.h';text=lease.read_text()
start=text.index('struct lmal_cpu_wait_state {');end=text.index('\n};',start)+4;state=text[start:end]
start=text.index('void captureWaitLocked(');pos=text.index('{',start)+1;depth=1
while depth:depth+=(text[pos]=='{')-(text[pos]=='}');pos+=1
body=text[start:pos]
code='''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
struct Task{int pid;};using task_t=Task*;static bool gate;
static int task_pid(task_t task){assert(gate);return task->pid;}
'''+state+'''
struct Lease {
 enum {SLOT_COUNT=64};
 uint64_t fLength=4096,fCpuTicket=123,fGpuBarrier=100;
 uint32_t fGpuCount=3,fReaders=4,fWriters=5,fUserReads=6,fUserWrites=7;
 uint32_t fCpuWaiters=8,fGpuWaiters=1,fCpuPriorityWaiters=2;
 int fError=-5;bool fFrozen=true,fQuarantined=false;
 struct {task_t task;uint32_t reads,writes;} fUsers[64]={};
 static uint64_t nowNs(){return 987654;}
 '''+body+'''
};
int main(){
 Lease lease;Task tasks[64];for(unsigned i=0;i<64;i++){tasks[i].pid=100+i;lease.fUsers[i]={&tasks[i],i+1,i+2};}
 lmal_cpu_wait_state state={};lease.captureWaitLocked(nullptr,1,&tasks[0]);
 gate=true;lease.captureWaitLocked(&state,1,&tasks[3]);gate=false;
 assert(state.at_ns==987654&&state.bytes==4096&&state.requester_pid==103&&state.phase==1);
 assert(state.gpu_count==3&&state.readers==4&&state.writers==5&&state.user_reads==6&&state.user_writes==7);
 assert(state.cpu_waiters==8&&state.gpu_waiters==1&&state.priority_waiters==2&&state.cpu_ticket==123&&state.gpu_barrier==100);
 assert(state.owners_total==64&&state.owners_saved==8&&state.error==-5&&state.frozen&&!state.quarantined);
 for(unsigned i=0;i<8;i++)assert(state.owners[i].pid==(int)(100+i)&&state.owners[i].reads==i+1&&state.owners[i].writes==i+2&&state.owners[i].requester==(i==3));
 puts("PASS actual gate snapshot: conflicting counts/tickets/tasks, requester identity, bounded owners, no fast-path work");
}
'''
(O/'wait-state.cpp').write_text(code)
assert text.index('captureWaitLocked(wait,1,own);break;')<text.index('removeGpuWaiter();',text.index('int awaitGpuTurn('))
runs=[]
for cc,cxx in [('clang','clang++'),('gcc','g++')]:
 for name,compiler,src,extra in [('cache-hook',cc,R/'tools/lancelot-metal/tests/present_cache_hook_test.c',['-std=c11','-I'+str(R/'tools/lancelot-metal/src')]),('wait-state',cxx,O/'wait-state.cpp',['-std=c++17'])]:
  exe=O/(cc+'-'+name)
  for stage,cmd in [('build',[compiler,*extra,'-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)]),('run',[str(exe)])]:
   q=subprocess.run(cmd,capture_output=True);(O/(cc+'-'+name+'-'+stage+'.log')).write_bytes(q.stdout+q.stderr)
   assert not q.returncode,q.stderr
   runs.append({'compiler':compiler,'test':name,'stage':stage,'exit':q.returncode})
sources=[lease,R/'tools/lancelot-metal/src/present_cache_hook.h',R/'tools/lancelot-metal/tests/present_cache_hook_test.c',Path(__file__)]
(O/'result.json').write_text(json.dumps({'passed':True,'runs':runs,'sources':{p.relative_to(R).as_posix():sha(p) for p in sources},'hardware_verified':False},indent=2)+'\n')
print('PASS G6 boundary checks')
