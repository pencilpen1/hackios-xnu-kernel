#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define LMAL_OK 0
#define LMAL_EBUSY (-16)
#define LMAL_ETIMEDOUT (-110)
#define LMAL_EIO (-5)
#define LMAL_EPERM (-1)
typedef void *task_t;
struct lmal_cpu_wait_state;
static int owner_identity;
static const task_t owner_task=&owner_identity;
#ifdef CURRENT_FAIRNESS
static unsigned task_refs;
static task_t current_task(){return owner_task;}
static void task_reference(task_t task){assert(task==owner_task);++task_refs;}
static void task_deallocate(task_t task){assert(task==owner_task&&task_refs);--task_refs;}
#endif
struct IOLock {bool held=false;};
static void IOLockLock(IOLock *p){assert(!p->held);p->held=true;}
static void IOLockUnlock(IOLock *p){assert(p->held);p->held=false;}
static void IOLockWakeup(IOLock *p,void *,bool){assert(p->held);}
struct Root {
    static constexpr unsigned SLOT_COUNT=4;
    IOLock gate;
    IOLock *fGate=&gate;
    uint32_t fGpuCount=0,fReaders=0,fWriters=0,fUserReads=0,fUserWrites=0,fCpuWaiters=0;
    uint32_t fGpuWaiters=0,fCpuPriorityWaiters=0;
    uint64_t fCpuTicket=0,fGpuBarrier=0;
    struct User {task_t task=0;uint32_t reads=0,writes=0;} fUsers[SLOT_COUNT];
    int fError=0,fWriteError=0;
    bool fQuarantined=false;
    enum Mode {NORMAL,GPU_READER_RELEASE,GPU_PRIOR_CPU_RELEASE,GPU_TIMEOUT} mode=NORMAL;
    unsigned future_gpu=0,retired_gpu=0,refs=0,steps=0,finish_writer_step=0;
    uint64_t prior_ticket=0;
    bool late_cpu_blocked=false;
    uint64_t now=0;
    uint64_t deadline(uint64_t ns){return now+ns;}
    void captureWaitLocked(lmal_cpu_wait_state *out,uint32_t,task_t){assert(!out);}
    void retain(){refs++;}
    void release(){assert(refs);refs--;}
    bool sleepUntil(void *,uint64_t end) {
        assert(gate.held);
        steps++;now++;
#ifdef CURRENT_FAIRNESS
        if(mode==GPU_READER_RELEASE) {
            assert(fGpuWaiters==1);
            uint64_t late=0;assert(addCpuWaiter(&late)==LMAL_OK);
            late_cpu_blocked=gpuTurnBlocksCpu(late);
            removeCpuWaiter(late);
            fReaders=0;
            return now<end;
        }
        if(mode==GPU_PRIOR_CPU_RELEASE) {
            assert(fGpuWaiters==1&&prior_ticket&&prior_ticket<=fGpuBarrier);
            removeCpuWaiter(prior_ticket);prior_ticket=0;
            return now<end;
        }
        if(mode==GPU_TIMEOUT)return false;
#endif
        if(!cpuConflict(owner_task,true)){fGpuCount++;future_gpu++;}
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
        assert(read.cpuConflict(owner_task,true));
        read.endCpuRead();
        assert(read.fReaders==0&&read.refs==0&&!read.cpuConflict(owner_task,true));
    }
    Root timeout;timeout.fWriters=1;timeout.fGpuCount=1;
    assert(timeout.beginCpuRead(3)==LMAL_ETIMEDOUT&&timeout.fCpuWaiters==0&&timeout.fReaders==0);
    timeout.fWriters=0;assert(!timeout.cpuConflict(owner_task,true));
    Root error;error.fError=LMAL_EIO;error.fGpuCount=1;
    assert(error.waitCpu(3)==LMAL_EIO&&error.fCpuWaiters==0);
    assert(error.beginCpuRead(3)==LMAL_EIO&&error.fCpuWaiters==0);
    if(!baseline) {
        Root owner;owner.fCpuWaiters=1;
        owner.fUserWrites=1;owner.fUsers[0].task=owner_task;owner.fUsers[0].writes=1;
        assert(!owner.cpuConflict(owner_task,true)); /* existing logical writer must complete */
        owner.fUserWrites=0;owner.fUsers[0].writes=0;
        assert(owner.cpuConflict(owner_task,true)&&!owner.cpuConflict(owner_task,false));
        Root writing;writing.fGpuCount=1;writing.fUserWrites=1;
        writing.fUsers[0].task=owner_task;writing.fUsers[0].writes=1;writing.finish_writer_step=2;
        assert(writing.beginCpuRead(10)==LMAL_OK&&writing.steps==3&&writing.fReaders==1);
        writing.endCpuRead();
        Root overflow;overflow.fCpuWaiters=UINT32_MAX;
        assert(overflow.waitCpu(3)==LMAL_EBUSY&&overflow.beginCpuRead(3)==LMAL_EBUSY);
#ifdef CURRENT_FAIRNESS
        /* G6 hardware: compositor PID 22 waited five seconds for a logical
         * read owned by SpringBoard. A sampled-only root can prepare, while
         * writes and CPU accesses through the kernel cache view still wait. */
        Root sampled;int other_identity;
        sampled.fUserReads=1;sampled.fUsers[0].task=&other_identity;sampled.fUsers[0].reads=1;
        assert(sampled.cpuConflict(owner_task,true));
        assert(!sampled.cpuConflict(owner_task,true,false,false));
        assert(sampled.cpuConflict(owner_task,false,false,false)); /* strict path unchanged */
        IOLockLock(sampled.fGate);
        assert(sampled.awaitGpuTurn(owner_task,true,10,false,nullptr,false)==LMAL_OK);
        assert(sampled.awaitGpuTurn(owner_task,true,10,false,nullptr,true)==LMAL_EBUSY);
        sampled.fWriters=1;
        assert(sampled.awaitGpuTurn(owner_task,true,10,false,nullptr,false)==LMAL_EBUSY);
        sampled.fWriters=0;sampled.fReaders=1;
        assert(sampled.awaitGpuTurn(owner_task,true,10,false,nullptr,false)==LMAL_EBUSY);
        sampled.fReaders=0;sampled.fUserWrites=1;sampled.fUsers[0].writes=1;
        assert(sampled.awaitGpuTurn(owner_task,true,10,false,nullptr,false)==LMAL_EBUSY);
        sampled.fUserWrites=0;sampled.fUsers[0].writes=0;
        uint64_t earlier=0;assert(sampled.addCpuWaiter(&earlier)==LMAL_OK);
        assert(sampled.awaitGpuTurn(owner_task,true,10,false,nullptr,false)==LMAL_EBUSY);
        sampled.removeCpuWaiter(earlier);IOLockUnlock(sampled.fGate);
        /* A nonblocking 1 ms retry has no reserved turn: a fresh reader can
         * repeatedly enter each gap. A queued GPU below closes that gap. */
        Root barging;
        for(unsigned i=0;i<8;i++) {
            assert(barging.beginCpuRead(10)==LMAL_OK);
            assert(barging.cpuConflict(owner_task,true));
            barging.endCpuRead();
        }
        Root gpu;gpu.fReaders=1;gpu.mode=Root::GPU_READER_RELEASE;
        IOLockLock(gpu.fGate);
        assert(gpu.awaitGpuTurn(owner_task,true,10,true)==LMAL_OK);
        assert(gpu.late_cpu_blocked&&gpu.fGpuWaiters==0&&gpu.fCpuPriorityWaiters==0);
        IOLockUnlock(gpu.fGate);
        Root prior;prior.mode=Root::GPU_PRIOR_CPU_RELEASE;
        IOLockLock(prior.fGate);
        assert(prior.addCpuWaiter(&prior.prior_ticket)==LMAL_OK);
        assert(prior.awaitGpuTurn(owner_task,true,10,true)==LMAL_OK);
        assert(!prior.prior_ticket&&prior.fCpuWaiters==0&&prior.fGpuWaiters==0);
        IOLockUnlock(prior.fGate);
        Root gpu_timeout;gpu_timeout.fReaders=1;gpu_timeout.mode=Root::GPU_TIMEOUT;
        IOLockLock(gpu_timeout.fGate);
        assert(gpu_timeout.awaitGpuTurn(owner_task,true,1,true)==LMAL_ETIMEDOUT);
        assert(gpu_timeout.fGpuWaiters==0&&gpu_timeout.fGpuBarrier==0&&
            gpu_timeout.fCpuPriorityWaiters==0);
        IOLockUnlock(gpu_timeout.fGate);
        Root batch;uint64_t early=0,late=0;
        IOLockLock(batch.fGate);
        assert(batch.addCpuWaiter(&early)==LMAL_OK);
        assert(batch.addGpuWaiter()==LMAL_OK&&batch.addGpuWaiter()==LMAL_OK);
        assert(batch.addCpuWaiter(&late)==LMAL_OK&&batch.gpuTurnBlocksCpu(late));
        batch.removeCpuWaiter(early);assert(batch.fCpuPriorityWaiters==0);
        batch.removeGpuWaiter();assert(batch.gpuTurnBlocksCpu(late));
        batch.removeGpuWaiter();assert(!batch.gpuTurnBlocksCpu(late));
        batch.removeCpuWaiter(late);
        assert(!batch.fCpuWaiters&&!batch.fGpuWaiters&&!batch.fCpuPriorityWaiters);
        batch.fCpuTicket=UINT64_MAX;
        assert(batch.addCpuWaiter(&late)==LMAL_OK&&late==1);
        batch.removeCpuWaiter(late);
        IOLockUnlock(batch.fGate);
        Root writer;
        assert(writer.beginCpuWrite(10)==LMAL_OK&&writer.fWriters==1);
        writer.endCpuWrite();assert(!writer.fWriters&&!writer.refs&&!writer.fCpuWaiters);
        Root user;
        assert(user.beginUserCpu(owner_task,true,10)==LMAL_OK&&user.fUserWrites==1);
        IOLockLock(user.fGate);assert(user.addGpuWaiter()==LMAL_OK);IOLockUnlock(user.fGate);
        assert(user.beginUserCpu(owner_task,true,10)==LMAL_OK&&user.fUserWrites==2);
        user.endUserCpu(owner_task,true);user.endUserCpu(owner_task,true);
        assert(!user.fUserWrites&&!user.fCpuWaiters&&!user.refs&&!task_refs);
        IOLockLock(user.fGate);user.removeGpuWaiter();IOLockUnlock(user.fGate);
#endif
    }
    puts(baseline?"PASS old code: CPU waiter starves despite completed GPU work":"PASS current code: queued GPU work drains, CPU reader progresses, writer dependency and timeout cleanup preserved");
}
