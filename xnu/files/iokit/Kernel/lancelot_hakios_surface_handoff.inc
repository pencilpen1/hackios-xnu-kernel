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
    struct Entry { LancelotMaliCpuLease *lease; uint64_t cookie; uint32_t sid; bool writer; };
    Entry *fEntries = NULL;
    uint32_t fCapacity = 0, fCount = 0;
    bool fFinished = false, fDmaSafe = true;
    lmal_cpu_wait_state fWait = {};
    uint32_t fWaitSid = 0;
    bool fWaitWriter = true;
public:
    bool init(uint32_t capacity) {
        fCapacity=capacity;fEntries=IONewZero(Entry,capacity);return fEntries!=NULL;
    }
    uint32_t count() const {return fCount;}
    int add(MiniIOSurface *surface, bool writer = true) {
        if(!surface)return LMAL_OK;
        LancelotMaliCpuLease *lease=surface->maliCpuLease;
        if(!lease)return LMAL_ENOTSUP;
        for(uint32_t i=0;i<fCount;i++)if(fEntries[i].lease==lease) {
            fEntries[i].writer |= writer;return LMAL_OK;
        }
        if(fCount==fCapacity)return LMAL_ENOMEM;
        lease->retain();fEntries[fCount].sid=surface->sid;fEntries[fCount].writer=writer;
        fEntries[fCount++].lease=lease;return LMAL_OK;
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
        uint32_t pivot=fCount;
        for(;;) {
            /* Never sleep for a second root while a first root is frozen:
             * a CPU owner of the second may need the first to finish. Wait
             * fairly on the contended root only with all other reservations
             * released, then try the complete set without blocking. */
            if(pivot<fCount) {
                uint64_t now=lmq_clock();if(!now||now>=end)return LMAL_ETIMEDOUT;
                int error=fEntries[pivot].lease->beginGpu(end-now,&fEntries[pivot].cookie,false,fEntries[pivot].writer,waitCpu,NULL,
                    hakios_first_gpu_sync,&fWait);
                fWaitSid=fEntries[pivot].sid;
                fWaitWriter=fEntries[pivot].writer;
                if(error)return error;
            }
            uint32_t busy=fCount;
            for(uint32_t i=0;i<fCount;i++) {
                if(fEntries[i].cookie)continue;
                uint64_t now=lmq_clock();if(!now||now>=end){resetPrepared();return LMAL_ETIMEDOUT;}
                int error=fEntries[i].lease->beginGpu(end-now,&fEntries[i].cookie,false,fEntries[i].writer,false,NULL,
                    hakios_first_gpu_sync,&fWait);
                fWaitSid=fEntries[i].sid;
                fWaitWriter=fEntries[i].writer;
                if(error) {
                    int reset=resetPrepared();if(reset)return reset;
                    if(error!=LMAL_EBUSY || !waitCpu)return error;
                    busy=i;break;
                }
            }
            if(busy==fCount)return LMAL_OK;
            pivot=busy;
        }
    }
    void reportWait() const {
        if(!fWait.phase)return;
        IOLog("lancelot-surface-wait: sid=%u phase=%u at=%llu bytes=%llu requester=%d error=%d gpu_writer=%u\n",
            fWaitSid,fWait.phase,fWait.at_ns/1000,fWait.bytes,fWait.requester_pid,fWait.error,fWaitWriter);
        IOLog("lancelot-surface-gate: sid=%u gpu=%u readers=%u writers=%u user_r=%u user_w=%u cpu_waiters=%u gpu_waiters=%u priority=%u\n",
            fWaitSid,fWait.gpu_count,fWait.readers,fWait.writers,fWait.user_reads,fWait.user_writes,
            fWait.cpu_waiters,fWait.gpu_waiters,fWait.priority_waiters);
        IOLog("lancelot-surface-ticket: sid=%u ticket=%llu barrier=%llu frozen=%u quarantine=%u owners=%u saved=%u\n",
            fWaitSid,fWait.cpu_ticket,fWait.gpu_barrier,fWait.frozen,fWait.quarantined,fWait.owners_total,fWait.owners_saved);
        for(uint32_t i=0;i<fWait.owners_saved;i++) {
            const auto &o=fWait.owners[i];
            IOLog("lancelot-surface-owner: sid=%u pid=%d reads=%u writes=%u requester=%u\n",fWaitSid,o.pid,o.reads,o.writes,o.requester);
        }
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
