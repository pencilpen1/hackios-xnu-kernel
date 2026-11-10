#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define SWAP_LAYERS 2
#define SWAP_GPU_MS 1000
#define SWAP_QUEUE 4
#define SWAP_LAYERS_SET 0
#define SWAP_ID 1
#define SWAP_SURFACE(layer) ((layer)+2)
#define NSEC_PER_SEC 1000000000ull
#define LMAL_OK 0
static uint64_t now=1000000000,waited;
static unsigned cpuReaders,queueReadersHeld,draws,gpuDuringBeat;
static bool gpuDone=true,failQueue=false,failCpu=false,gOnTheBeat=true,heldDuringBeat;
static uint64_t mach_absolute_time(){return now;}
static uint64_t micros(uint64_t value){return value/1000;}
static void nanoseconds_to_absolutetime(uint64_t value,uint64_t *out){*out=value;}
static void IOLog(const char *,...){}
static void IOLockLock(void *){}
static void IOLockUnlock(void *){}
static void IOLockWakeup(void *,void *,bool){}
static uint32_t swapWord(const uint8_t *,unsigned index){return index+1;}
struct LancelotMaliCpuLease {};
struct MiniIOSurface {unsigned sid;LancelotMaliCpuLease *maliCpuLease;};
static bool MiniIOSurfaceDrawn(MiniIOSurface *,unsigned){now+=1000;return gpuDone;}
static bool MiniIOSurfaceMaliDrawn(MiniIOSurface *,unsigned){now+=1000;return gpuDone;}
struct LancelotMaliReadGroup {
    unsigned added=0,held=0;
    bool add(LancelotMaliCpuLease *lease){if(lease)++added;return true;}
    int acquire(uint64_t){now+=3000;if(failCpu)return -1;held=added;cpuReaders+=held;return 0;}
    void releaseReaders(){assert(cpuReaders>=held);cpuReaders-=held;held=0;}
    ~LancelotMaliReadGroup(){releaseReaders();}
};
struct LancelotMaliQueueReadGroup {
    unsigned held=0;
    bool add(MiniIOSurface *surface,uint64_t){if(!surface)return true;if(failQueue)return false;++held;++queueReadersHeld;return true;}
    void releaseReaders(){assert(queueReadersHeld>=held);queueReadersHeld-=held;held=0;}
    ~LancelotMaliQueueReadGroup(){releaseReaders();}
};
static void clock_delay_until(uint64_t deadline) {
    assert(deadline>=now);waited+=deadline-now;now=deadline;
    heldDuringBeat=cpuReaders||queueReadersHeld;
    if(!heldDuringBeat)++gpuDuringBeat;
}
struct Framebuffer {
    uint64_t fReady=now-1000000,fBeat=now-1000000;
    uint64_t fStatWaiting=0,fStatBeatWait=0;
    unsigned fStatOnBeat=0,fSwapFirst=0,fSwapsWaiting=1,fStatLost=0;
    void *fSwapLock=nullptr;
    unsigned refreshHz(){return 60;}
    void show(const uint8_t *swap,MiniIOSurface **surfaces,uint64_t submitted) {
#include "actual-presenter-prefix.inc"
        (void)set;(void)nanos;(void)ready;(void)shown;(void)drawn;(void)gap_us;
        (void)layers;(void)recorded;(void)report;
        assert(cpuReaders==1&&queueReadersHeld==1);
        ++draws; /* Stand-in for first pixel access after the production prefix. */
    }
};
static void reset(){cpuReaders=queueReadersHeld=draws=gpuDuringBeat=0;waited=0;heldDuringBeat=false;gpuDone=true;failQueue=failCpu=false;gOnTheBeat=true;}
int main(){
    LancelotMaliCpuLease lease;MiniIOSurface surface{1,&lease};
    MiniIOSurface *surfaces[2]={&surface,nullptr};uint8_t swap[8]{};
    reset();Framebuffer frame;uint64_t entered=now;frame.show(swap,surfaces,now);(void)entered;
    assert(draws==1&&waited>0&&!cpuReaders&&!queueReadersHeld);
#ifdef BEFORE_PACING
    assert(heldDuringBeat&&!gpuDuringBeat);
#else
    assert(!heldDuringBeat&&gpuDuringBeat==1);
    assert(frame.fStatWaiting==5000&&frame.fStatBeatWait==waited);
    assert(frame.fStatWaiting+frame.fStatBeatWait==now-entered);
#endif
    for(unsigned failure=0;failure<3;failure++){
        reset();Framebuffer rejected;
        if(failure==0)gpuDone=false;
        if(failure==1)failQueue=true;
        if(failure==2)failCpu=true;
        rejected.show(swap,surfaces,now);
        assert(!draws&&!cpuReaders&&!queueReadersHeld&&rejected.fStatLost==1);
        if(failure==0)assert(!waited);
    }
    reset();gOnTheBeat=false;Framebuffer unpaced;unpaced.show(swap,surfaces,now);
    assert(draws==1&&!waited&&!cpuReaders&&!queueReadersHeld);
    puts(
#ifdef BEFORE_PACING
        "PASS: W7 reader ownership blocks GPU throughout refresh delay reproduced"
#else
        "PASS: refresh wait permits GPU progress; both gates precede pixels; failures release all gates; timing excludes pacing"
#endif
    );
}
