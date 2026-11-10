#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#define LMAL_OK 0
#define LMAL_EINVAL (-22)
#define LMAL_ENOTSUP (-95)
#define LMAL_ETIMEDOUT (-110)
#define LMAL_EIO (-5)
#define LMAL_EBUSY (-16)
#define PAGE_SIZE 4096u
#define kIOReturnSuccess 0
#define kIOMapDefaultCache 0
#define LMAL_VM_PREPARING 1
#define LMAL_VM_BOUND_IDLE 0
#define LMAL_VM_ACTIVE 2
#define LMAL_VM_RETIRING 3
using task_t = void *;
using IOReturn = int;
using mach_vm_size_t = uint64_t;
struct IOMemoryDescriptor {};
static task_t current_task() { return nullptr; }
struct IOLock { std::mutex mutex; std::thread::id owner; };
static void IOLockLock(IOLock *lock) { lock->mutex.lock();lock->owner=std::this_thread::get_id(); }
static void IOLockUnlock(IOLock *lock) { lock->owner={};lock->mutex.unlock(); }
static void IOLockWakeup(IOLock *,void *,bool) {}
static int lmal_cpu_vm_binding_verify(void *,void *,void *,bool) { return 0; }
static int lmal_cpu_vm_phase(void *,void *,int) { return 0; }
static std::atomic<uint64_t> fake_clock_ns{100};
struct Holder {
    void *fRedirUPL=nullptr;
    int redirect(IOMemoryDescriptor *,unsigned,uint64_t) { fRedirUPL=this;return 0; }
};
struct Lease {
    enum {SLOT_COUNT=64};
    IOLock gate; IOLock *fGate=&gate;
    Holder holder;Holder *fHolder=&holder;
    void *fObject=nullptr,*fBinding=nullptr;
    uint64_t fLength=3*PAGE_SIZE,fNextCookie=0,fLastWriter=0;
    uint32_t fGpuCount=0,fReaders=0;
    struct {uint64_t cookie=0,pair=0;bool writer=false,published=false;} fSlots[SLOT_COUNT];
    struct {task_t task=nullptr;unsigned reads=0,writes=0;} fUsers[SLOT_COUNT];
    bool fFrozen=false,fQuarantined=false,fGpuRetain=false;
    bool proof=true,cacheReady=true;
    int fError=0,fWriteError=0;
    std::atomic<unsigned> refs{1};
    unsigned reopens=0;
    uint8_t cache[3*PAGE_SIZE]{};
    static uint64_t nowNs(){return fake_clock_ns.load();}
    static uint64_t deadline(uint64_t ns){return nowNs()+ns;}
    bool cacheValid(){return true;}
    bool objectValid(bool,bool=false){return true;}
    bool blockedProof(){return proof;}
    bool prefaultCache(){return cacheReady;}
    int awaitGpuTurn(task_t,bool,uint64_t,bool){return 0;}
    void retain(){++refs;}
    void release(){assert(refs);--refs;}
    void *cacheAddress(){return cache;}
    bool reopen(){++reopens;holder.fRedirUPL=nullptr;fFrozen=false;return true;}
    void poison(int error){fError=error?error:LMAL_EIO;fQuarantined=true;}
#include "actual-gpu-prepare.inc"
#include "actual-gpu-retire.inc"
};

struct Range {uintptr_t address;size_t length;bool cpu;};
static std::vector<Range> ranges;
static int mali_trace=2;
static uint64_t hkmt_cache_bytes[2],hkmt_cache_ns[2];
static uint64_t clock_ns(void *){return fake_clock_ns.load();}
static void for_device(void *,void *address,size_t length){ranges.push_back({(uintptr_t)address,length,false});fake_clock_ns+=1000;}
static void for_cpu(void *,void *address,size_t length){ranges.push_back({(uintptr_t)address,length,true});fake_clock_ns+=2000;}
extern "C" void lancelot_hakios_cache_sync(void *,size_t,bool);
#include "actual-cache-operation.inc"
#include "actual-surface-cache-callbacks.inc"

static std::mutex control;
static std::condition_variable changed;
static bool syncEntered,peerProbed,syncFinished,peerBeforeClean;
static unsigned cleanCalls;
static Lease *activeLease;
static void blockedClean(void *,size_t length) {
    assert(length==3*PAGE_SIZE);
    std::unique_lock<std::mutex> lock(control);
    ++cleanCalls;syncEntered=true;changed.notify_all();
    changed.wait(lock,[]{return peerProbed;});
    syncFinished=true;
}
static int acquire(Lease &lease,uint64_t *cookie,void (*clean)(void *,size_t)) {
    bool first=false;
#ifdef BEFORE_SYNC
    int error=lease.beginGpu(1000,cookie,false,true,true,&first);
    if(!error&&first&&clean)clean(lease.cacheAddress(),lease.fLength);
    return error;
#else
    return lease.beginGpu(1000,cookie,false,true,true,&first,clean);
#endif
}
static void countClean(void *,size_t){++cleanCalls;}
static void expireClean(void *,size_t){fake_clock_ns+=1000;}
static void retireSync(void *,size_t) {
    assert(activeLease&&activeLease->fFrozen&&activeLease->reopens==0);
    assert(activeLease->gate.owner==std::this_thread::get_id());
    ++cleanCalls;
}
int main() {
    {
        Lease lease;uint64_t first=0,second=0;
        syncEntered=peerProbed=syncFinished=peerBeforeClean=false;cleanCalls=0;
        std::thread a([&]{assert(acquire(lease,&first,blockedClean)==LMAL_OK);});
        std::thread b([&]{
            {std::unique_lock<std::mutex> lock(control);changed.wait(lock,[]{return syncEntered;});}
            bool unlocked=lease.gate.mutex.try_lock();
            if(unlocked) {
                lease.gate.mutex.unlock();
                assert(acquire(lease,&second,countClean)==LMAL_OK);
                std::lock_guard<std::mutex> lock(control);peerBeforeClean=!syncFinished;
            }
            {std::lock_guard<std::mutex> lock(control);peerProbed=true;changed.notify_all();}
            if(!unlocked)assert(acquire(lease,&second,countClean)==LMAL_OK);
        });
        a.join();b.join();
#ifdef BEFORE_SYNC
        assert(peerBeforeClean);
#else
        assert(!peerBeforeClean);
#endif
        assert(cleanCalls==1&&lease.fGpuCount==2&&lease.refs==2);
        assert(lease.markLegacy(first)==0&&lease.markLegacy(second)==0);
        activeLease=&lease;cleanCalls=0;
        assert(lease.endGpu(first,0,true,retireSync)==0&&cleanCalls==0);
        assert(lease.endGpu(second,0,true,retireSync)==0&&cleanCalls==1);
        assert(!lease.fFrozen&&!lease.fGpuCount&&lease.refs==1&&lease.reopens==1);
    }
    {
        Lease lease;uint64_t cookie=0;
        int error=acquire(lease,&cookie,expireClean);
#ifdef BEFORE_SYNC
        assert(error==0&&cookie);assert(lease.endGpu(cookie,0,true)==0);
#else
        assert(error==LMAL_ETIMEDOUT&&!cookie&&!lease.fFrozen&&!lease.fGpuCount&&lease.refs==1);
#endif
    }
    {
        Lease lease;uint64_t cookie=0;cleanCalls=0;lease.cacheReady=false;
        assert(acquire(lease,&cookie,countClean)==LMAL_EIO&&!cookie&&!cleanCalls);
        assert(!lease.fGpuCount&&!lease.fFrozen&&lease.refs==1);
    }
    {
        Lease lease;uint64_t cookie=0;cleanCalls=0;
        assert(acquire(lease,&cookie,countClean)==0);
        activeLease=&lease;
        assert(lease.endGpu(cookie,LMAL_EIO,false,retireSync)==LMAL_EBUSY);
        assert(lease.fQuarantined&&lease.fFrozen&&lease.fGpuCount==1&&cleanCalls==1);
    }
    {
        uint8_t root[3*PAGE_SIZE]{};ranges.clear();root[24]=0x5a;
        hakios_first_gpu_sync(root,sizeof(root));
        hakios_last_gpu_sync(root,sizeof(root));
        assert(ranges.size()==2&&!ranges[0].cpu&&ranges[1].cpu);
        for(const auto &range:ranges) {
            assert(range.address==(uintptr_t)root+PAGE_SIZE&&range.length==2*PAGE_SIZE);
            /* Simulate final invalidation dropping dirty cached bytes. The
             * GPU never maps the metadata page, so its seed must survive. */
            if(range.cpu)for(size_t i=0;i<range.length;i++)((uint8_t *)range.address)[i]=0;
        }
        assert(root[24]==0x5a);
        assert(hkmt_cache_bytes[0]==2*PAGE_SIZE&&hkmt_cache_bytes[1]==2*PAGE_SIZE);
        assert(hkmt_cache_ns[0]==1000&&hkmt_cache_ns[1]==2000);
        hakios_first_gpu_sync(root,PAGE_SIZE);hakios_last_gpu_sync(root,0);
        assert(ranges.size()==2);
        mali_trace=0;hakios_first_gpu_sync(root,sizeof(root));
        assert(ranges.size()==3&&hkmt_cache_bytes[0]==2*PAGE_SIZE&&hkmt_cache_ns[0]==1000);
    }
    puts(
#ifdef BEFORE_SYNC
        "PASS: W7 publication race and uncharged clean budget reproduced; retire proof and pixel range tested"
#else
        "PASS: atomic first clean, overlapping reservations, clean budget, proof failure, quarantine, final invalidate and metadata exclusion"
#endif
    );
}
