#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define LMAL_OK 0
#define LMAL_EBUSY (-16)
#define LMAL_ETIMEDOUT (-110)
#define LMAL_EIO (-5)
typedef uintptr_t task_t;
struct IOLock {bool held=false;};
static void IOLockLock(IOLock *p){assert(!p->held);p->held=true;}
static void IOLockUnlock(IOLock *p){assert(p->held);p->held=false;}
static void IOLockWakeup(IOLock *p,void *,bool){assert(p->held);}
struct Root {
    static constexpr unsigned SLOT_COUNT=4;
    IOLock gate;
    IOLock *fGate=&gate;
    uint32_t fGpuCount=0,fReaders=0,fWriters=0,fUserReads=0,fUserWrites=0,fCpuWaiters=0;
    struct User {task_t task=0;uint32_t reads=0,writes=0;} fUsers[SLOT_COUNT];
    int fError=0,fWriteError=0;
    bool fQuarantined=false;
    unsigned future_gpu=0,retired_gpu=0,refs=0,steps=0,finish_writer_step=0;
    uint64_t now=0;
    uint64_t deadline(uint64_t ns){return now+ns;}
    void retain(){refs++;}
    void release(){assert(refs);refs--;}
    bool sleepUntil(void *,uint64_t end) {
        assert(gate.held);
        steps++;now++;
        if(!cpuConflict(1,true)){fGpuCount++;future_gpu++;}
        if(fGpuCount){fGpuCount--;retired_gpu++;}
        if(finish_writer_step&&steps>=finish_writer_step){fUserWrites=0;fUsers[0].writes=0;}
        return now<end;
    }
#include "actual-cpu-gate.inc"
};
int main(int argc,char **)
{
    bool baseline=argc>1;
    Root wait;wait.fGpuCount=1;
    int result=wait.waitCpu(4);
    if(baseline)assert(result==LMAL_ETIMEDOUT&&wait.future_gpu>=4&&wait.retired_gpu>=4);
    else assert(result==LMAL_OK&&wait.future_gpu==0&&wait.retired_gpu==1&&wait.fCpuWaiters==0);
    Root read;read.fGpuCount=1;
    result=read.beginCpuRead(4);
    if(baseline)assert(result==LMAL_ETIMEDOUT&&read.fReaders==0);
    else {
        assert(result==LMAL_OK&&read.fReaders==1&&read.fCpuWaiters==0&&read.refs==1);
        assert(read.cpuConflict(1,true));
        read.endCpuRead();
        assert(read.fReaders==0&&read.refs==0&&!read.cpuConflict(1,true));
    }
    Root timeout;timeout.fWriters=1;timeout.fGpuCount=1;
    assert(timeout.beginCpuRead(3)==LMAL_ETIMEDOUT&&timeout.fCpuWaiters==0&&timeout.fReaders==0);
    timeout.fWriters=0;assert(!timeout.cpuConflict(1,true));
    Root error;error.fError=LMAL_EIO;error.fGpuCount=1;
    assert(error.waitCpu(3)==LMAL_EIO&&error.fCpuWaiters==0);
    assert(error.beginCpuRead(3)==LMAL_EIO&&error.fCpuWaiters==0);
    if(!baseline) {
        Root owner;owner.fCpuWaiters=1;
        owner.fUserWrites=1;owner.fUsers[0].task=1;owner.fUsers[0].writes=1;
        assert(!owner.cpuConflict(1,true)); /* existing logical writer must complete */
        owner.fUserWrites=0;owner.fUsers[0].writes=0;
        assert(owner.cpuConflict(1,true)&&!owner.cpuConflict(1,false));
        Root writing;writing.fGpuCount=1;writing.fUserWrites=1;
        writing.fUsers[0].task=1;writing.fUsers[0].writes=1;writing.finish_writer_step=2;
        assert(writing.beginCpuRead(10)==LMAL_OK&&writing.steps==3&&writing.fReaders==1);
        writing.endCpuRead();
        Root overflow;overflow.fCpuWaiters=UINT32_MAX;
        assert(overflow.waitCpu(3)==LMAL_EBUSY&&overflow.beginCpuRead(3)==LMAL_EBUSY);
    }
    puts(baseline?"PASS old code: CPU waiter starves despite completed GPU work":"PASS current code: queued GPU work drains, CPU reader progresses, writer dependency and timeout cleanup preserved");
}
