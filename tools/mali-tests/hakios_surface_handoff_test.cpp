#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <new>
#include <vector>
#include <algorithm>
#include <stdarg.h>
#include <string.h>
#include "actual-wait-state.h"
static unsigned wait_reports;
static void IOLog(const char *format,...) {
    char line[256];va_list args;va_start(args,format);int n=vsnprintf(line,sizeof(line),format,args);va_end(args);
    assert(n>0 && n<(int)sizeof(line));++wait_reports;
}

#define LMAL_OK 0
#define LMAL_EIO (-5)
#define LMAL_EBUSY (-16)
#define LMAL_ENOMEM (-12)
#define LMAL_ENOTSUP (-95)
#define LMAL_EINVAL (-22)
#define LMAL_ETIMEDOUT (-110)
#define PAGE_SIZE 4096u
#define IONewZero(type,count) new (std::nothrow) type[count]()
#define IONew(type,count) new (std::nothrow) type[count]
#define IODelete(pointer,type,count) do { (void)(count); delete[] (pointer); } while (0)

static uint64_t fake_clock;
static std::vector<uintptr_t> acquire_order;
static std::vector<uintptr_t> cpu_order;
static uint64_t lmq_clock() { return fake_clock; }
static uint64_t mach_absolute_time() { return fake_clock; }
static void absolutetime_to_nanoseconds(uint64_t absolute,uint64_t *ns) { *ns=absolute; }
extern "C" void lancelot_hakios_cache_sync(void *, size_t, bool) {}

struct FakeRoot {
    size_t length=2*PAGE_SIZE;
    size_t getLength() const { return length; }
};

class LancelotMaliCpuLease {
    FakeRoot fStorage;
    FakeRoot *fRoot;
    uint8_t fCache[2*PAGE_SIZE]{};
    uint64_t fNextCookie=0;
public:
    unsigned refs=1,beginCalls=0,endCalls=0,publishCalls=0,active=0,cpuReaders=0;
    bool readerHeld=false,forceTimeout=false,lastWaitCpu=false;
    bool lastWriter=true;
    uint64_t waitCost=0;
    LancelotMaliCpuLease *cpuNeeds=nullptr;
    explicit LancelotMaliCpuLease(FakeRoot *root=nullptr):fRoot(root?root:&fStorage){}
    void setRoot(FakeRoot *root){fRoot=root;}
    void retain(){++refs;}
    void release(){assert(refs);--refs;}
    FakeRoot *root(){return fRoot;}
    void *cacheAddress(){return fCache;}
    int beginGpu(uint64_t remaining,uint64_t *cookie,bool requireBlock,bool writer,bool waitCpu,bool *firstGpu,
        void (*firstSync)(void *,size_t),lmal_cpu_wait_state *wait=nullptr) {
        assert(cookie&&remaining&& !requireBlock);lastWriter=writer;
        ++beginCalls;lastWaitCpu=waitCpu;acquire_order.push_back((uintptr_t)this);
        if(readerHeld) {
            if(!waitCpu)return LMAL_EBUSY;
            if(forceTimeout || (cpuNeeds && cpuNeeds->active) || waitCost>=remaining) {
                fake_clock+=remaining;if(wait){*wait={};wait->phase=1;wait->readers=1;}return LMAL_ETIMEDOUT;
            }
            fake_clock+=waitCost;readerHeld=false;
        }
        if(!active && firstSync)firstSync(fCache,fRoot->length);
        if(firstGpu)*firstGpu=active==0;
        *cookie=++fNextCookie;++active;return LMAL_OK;
    }
    int markLegacy(uint64_t cookie){assert(cookie&&active);++publishCalls;return LMAL_OK;}
    int endGpu(uint64_t cookie,int,bool,void (*sync)(void *,size_t)) {
        assert(cookie&&active);if(active==1&&sync)sync(fCache,fRoot->length);
        --active;++endCalls;return LMAL_OK;
    }
    int beginCpuRead(uint64_t remaining) {
        assert(remaining);cpu_order.push_back((uintptr_t)this);++cpuReaders;retain();return LMAL_OK;
    }
    void endCpuRead(){assert(cpuReaders);--cpuReaders;release();}
};

struct MiniIOSurface { LancelotMaliCpuLease *maliCpuLease; uint32_t sid=17; };
using user_addr_t=uintptr_t;
static int copyin(user_addr_t from,void *to,size_t bytes) {
    if(from==1)return 1;
    memcpy(to,(const void *)from,bytes);return 0;
}
#include "actual-hakios-submit-roles.h"

#include "actual-hakios-surface-handoff.inc"
#include "actual-cpu-read-group.inc"

static void addReverse(HakiosSurfaceHandoff &handoff,MiniIOSurface &a,MiniIOSurface &b) {
    if((uintptr_t)a.maliCpuLease<(uintptr_t)b.maliCpuLease) {
        assert(handoff.add(&b)==LMAL_OK);assert(handoff.add(&a)==LMAL_OK);
    } else {
        assert(handoff.add(&a)==LMAL_OK);assert(handoff.add(&b)==LMAL_OK);
    }
}

int main() {
    {
        HakiosSubmitRoles legacy;assert(legacy.init(0,3)==LMAL_OK&&legacy.writer(0));
        uint32_t flags[]={0,1,1};HakiosSubmitRoles roles;
        assert(roles.init((uintptr_t)flags,3)==LMAL_OK);
        flags[1]=0;assert(roles.writer(0)&&!roles.writer(1)&&!roles.writer(2)&&roles.writer(3));
        uint32_t invalid=2;HakiosSubmitRoles bad;
        assert(bad.init((uintptr_t)&invalid,1)==LMAL_EINVAL);
        HakiosSubmitRoles fault;assert(fault.init(1,3)==LMAL_EINVAL);
    }
    {
        fake_clock=1;LancelotMaliCpuLease root;MiniIOSurface a{&root},alias{&root};
        HakiosSurfaceHandoff h;assert(h.init(3));assert(h.add(&a,false)==LMAL_OK);
        assert(h.add(&alias,false)==LMAL_OK&&h.count()==1);
        assert(h.acquire(100,true)==LMAL_OK&&!root.lastWriter);
        assert(h.finish(LMAL_OK,true,true)==LMAL_OK);
    }
    for(unsigned order=0;order<2;order++) {
        fake_clock=1;LancelotMaliCpuLease root;MiniIOSurface a{&root},alias{&root};
        HakiosSurfaceHandoff h;assert(h.init(3));assert(h.add(&a,order!=0)==LMAL_OK);
        assert(h.add(&alias,order==0)==LMAL_OK&&h.count()==1);
        assert(h.acquire(100,true)==LMAL_OK&&root.lastWriter);
        assert(h.finish(LMAL_OK,true,true)==LMAL_OK);
    }
    {
        fake_clock=1;
        LancelotMaliCpuLease a,b;
        auto *first=(uintptr_t)&a<(uintptr_t)&b?&a:&b;
        auto *second=first==&a?&b:&a;
        second->readerHeld=true;second->cpuNeeds=first;second->waitCost=10;
        MiniIOSurface sa{&a},sb{&b};HakiosSurfaceHandoff h;assert(h.init(2));addReverse(h,sa,sb);
        assert(h.acquire(100,true)==LMAL_OK);
        assert(!second->readerHeld&&first->active==1&&second->active==1);
        assert(first->endCalls>=1); /* released before waiting for the CPU */
        assert(h.publish()==LMAL_OK);assert(h.finish(LMAL_OK,true,true)==LMAL_OK);
        assert(!a.active&&!b.active);
    }

    {
        fake_clock=1;cpu_order.clear();
        FakeRoot roots[2];LancelotMaliCpuLease a,b;
        if((uintptr_t)&a<(uintptr_t)&b){a.setRoot(&roots[1]);b.setRoot(&roots[0]);}
        else {a.setRoot(&roots[0]);b.setRoot(&roots[1]);}
        assert(((uintptr_t)&a<(uintptr_t)&b) != ((uintptr_t)a.root()<(uintptr_t)b.root()));
        LancelotMaliReadGroup readers;
        if((uintptr_t)a.root()<(uintptr_t)b.root()) {
            assert(readers.add(&b));assert(readers.add(&a));
        } else {
            assert(readers.add(&a));assert(readers.add(&b));
        }
        assert(readers.acquire(100)==LMAL_OK);
        assert(cpu_order.size()==2&&cpu_order[0]<cpu_order[1]);
        readers.releaseReaders();assert(!a.cpuReaders&&!b.cpuReaders);
        /* Root descriptor addresses are deliberately reversed. CPU and GPU
         * must still acquire the same actual gate order. */
        acquire_order.clear();
        MiniIOSurface sa{&a},sb{&b};
        HakiosSurfaceHandoff handoff;assert(handoff.init(2));addReverse(handoff,sa,sb);
        assert(handoff.acquire(100,true)==LMAL_OK);
        assert(acquire_order==cpu_order);
        assert(handoff.resetPrepared()==LMAL_OK);
    }
    {
        fake_clock=100;acquire_order.clear();
        LancelotMaliCpuLease a,b;
        LancelotMaliCpuLease *second=(uintptr_t)&a<(uintptr_t)&b?&b:&a;
        second->readerHeld=true;second->waitCost=200;
        MiniIOSurface sa{&a},sb{&b};
        HakiosSurfaceHandoff handoff;assert(handoff.init(4));addReverse(handoff,sa,sb);
        assert(handoff.add(&sa)==LMAL_OK&&handoff.count()==2);
        assert(handoff.acquire(1000,true)==LMAL_OK);
        assert(a.beginCalls>=1&&b.beginCalls>=1&&second->lastWaitCpu);
        assert(acquire_order.size()>=2);
        assert(handoff.publish()==LMAL_OK);
        bool safe=false;assert(handoff.finish(LMAL_OK,true,true,&safe)==LMAL_OK&&safe);
        assert(a.active==0&&b.active==0&&a.endCalls>=1&&b.endCalls>=1);
        assert(a.publishCalls==1&&b.publishCalls==1);
    }
    {
        fake_clock=10;acquire_order.clear();
        LancelotMaliCpuLease a,b;
        LancelotMaliCpuLease *second=(uintptr_t)&a<(uintptr_t)&b?&b:&a;
        second->readerHeld=true;second->forceTimeout=true;
        MiniIOSurface sa{&a},sb{&b};
        HakiosSurfaceHandoff handoff;assert(handoff.init(2));addReverse(handoff,sa,sb);
        assert(handoff.acquire(20,true)==LMAL_ETIMEDOUT);
        handoff.reportWait();assert(wait_reports==3);
        assert(handoff.resetPrepared()==LMAL_OK);
        assert(a.active==0&&b.active==0&&a.endCalls+b.endCalls==1);
    }
    {
        fake_clock=50;acquire_order.clear();
        LancelotMaliCpuLease a,b;MiniIOSurface sa{&a},sb{&b};
        HakiosSurfaceHandoff handoff;assert(handoff.init(2));addReverse(handoff,sa,sb);
        assert(handoff.acquire(500,true)==LMAL_OK);
        /* This is the submit-side close/cancel path before publication. */
        assert(handoff.resetPrepared()==LMAL_OK);
        assert(a.active==0&&b.active==0&&a.endCalls>=1&&b.endCalls>=1);
        assert(a.publishCalls==0&&b.publishCalls==0);
    }
    puts("PASS Hakios multi-root fair wait: stable order, no root reacquire, timeout/cancel release every cookie");
}
