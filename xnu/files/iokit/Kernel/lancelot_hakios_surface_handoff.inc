/* SPDX-License-Identifier: APSL-2.0
 * Lancelot's presenter reads IOSurface pixels on the CPU. Keep those roots WB,
 * exclude CPU aliases during DMA, and maintain the same-PA cache view. Private
 * donor BOs remain WC and do not enter this handoff.
 */
extern "C" void lancelot_hakios_cache_sync(void *, size_t, bool);
/* Only the pixel subrange is mapped into the GPU. The leading shared-state
 * page remains CPU-owned; invalidating it could discard concurrent seed or
 * property updates through the kernel's cache-view alias. Keep full-root VM
 * exclusion, but limit both PoC operations to the actual GPU-visible pages. */
static void hakios_first_gpu_sync(void *address,size_t length) {
    if(length>PAGE_SIZE)
        lancelot_hakios_cache_sync((uint8_t *)address+PAGE_SIZE,length-PAGE_SIZE,false);
}
static void hakios_last_gpu_sync(void *address,size_t length) {
    if(length>PAGE_SIZE)
        lancelot_hakios_cache_sync((uint8_t *)address+PAGE_SIZE,length-PAGE_SIZE,true);
}
class HakiosSurfaceHandoff {
    struct Entry { LancelotMaliCpuLease *lease; uint64_t cookie; };
    Entry *fEntries = NULL;
    uint32_t fCapacity = 0, fCount = 0;
    bool fFinished = false, fDmaSafe = true;
public:
    bool init(uint32_t capacity) {
        fCapacity=capacity;fEntries=IONewZero(Entry,capacity);return fEntries!=NULL;
    }
    uint32_t count() const {return fCount;}
    int add(MiniIOSurface *surface) {
        if(!surface)return LMAL_OK;
        LancelotMaliCpuLease *lease=surface->maliCpuLease;
        if(!lease)return LMAL_ENOTSUP;
        for(uint32_t i=0;i<fCount;i++)if(fEntries[i].lease==lease)return LMAL_OK;
        if(fCount==fCapacity)return LMAL_ENOMEM;
        lease->retain();fEntries[fCount++].lease=lease;return LMAL_OK;
    }
    int acquire(uint64_t end,bool waitCpu = true) {
        for(uint32_t i=1;i<fCount;i++) {
            Entry e=fEntries[i];uint32_t j=i;
            /* Match LancelotMaliReadGroup and both other GPU submission
             * paths: the gate/lease address is the shared lock order. */
            while(j && (uintptr_t)fEntries[j-1].lease>(uintptr_t)e.lease) {
                fEntries[j]=fEntries[j-1];--j;
            }
            fEntries[j]=e;
        }
        for(uint32_t i=0;i<fCount;i++) {
            uint64_t now=lmq_clock();if(!now||now>=end)return LMAL_ETIMEDOUT;
            int error=fEntries[i].lease->beginGpu(end-now,&fEntries[i].cookie,false,true,waitCpu,NULL,
                hakios_first_gpu_sync);
            if(error)return error;
        }
        return LMAL_OK;
    }
    /* Nothing has been enqueued. Drop every partial reservation after the
     * client lock is reacquired, before any handle can be published. */
    int resetPrepared() {
        int result=LMAL_OK;
        for(uint32_t i=fCount;i>0;i--) {
            Entry &e=fEntries[i-1];if(!e.cookie)continue;
            int error=e.lease->endGpu(e.cookie,LMAL_OK,true,hakios_last_gpu_sync);
            if(error){result=error;fDmaSafe=false;}
            e.cookie=0;
        }
        return result;
    }
    int publish() {
        for(uint32_t i=0;i<fCount;i++) {
            int error=fEntries[i].lease->markLegacy(fEntries[i].cookie);
            if(error)return error;
        }
        return LMAL_OK;
    }
    int finish(int result,bool dmaSafe,bool executed,bool *safeResult=NULL) {
        (void)executed; /* Cache ownership is shared by queued reservations. */
        if(fFinished){if(safeResult)*safeResult=fDmaSafe;return result;}
        fFinished=true;
        fDmaSafe=fDmaSafe&&dmaSafe;
        for(uint32_t i=fCount;i>0;i--) {
            Entry &e=fEntries[i-1];if(!e.cookie)continue;
            int error=e.lease->endGpu(e.cookie,result,dmaSafe,hakios_last_gpu_sync);
            if(error){result=error;fDmaSafe=false;}
            e.cookie=0;
        }
        if(safeResult)*safeResult=fDmaSafe;
        return result;
    }
    ~HakiosSurfaceHandoff() {
        if(!fFinished)(void)finish(LMAL_EIO,true,false); /* preparation only; nothing enqueued */
        for(uint32_t i=0;i<fCount;i++)fEntries[i].lease->release();
        if(fEntries)IODelete(fEntries,Entry,fCapacity);
    }
};
