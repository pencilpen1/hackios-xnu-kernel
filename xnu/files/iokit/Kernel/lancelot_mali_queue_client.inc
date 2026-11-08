/* SPDX-License-Identifier: APSL-2.0
 * Lancelot adaptation of oosexgang/hakios-mt6768@4f270371's RedwoodMali:
 * Panfrost DRM selectors, wired write-combined BOs, queued jobs, real fences.
 * This client uses the board's existing private-VM hardware adapter, rather
 * than the donor's shared AS0. No firmware/iOS images are part of this source.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/task.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include "MiniIOSurface.h"
#include "LancelotMaliExport.h"
#include "lancelot_mali_queue_uapi.h"
#include "../../bsd/dev/lancelot_mali_queue.h"

static uint64_t lmq_clock()
{
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;
}
static IOReturn lmq_return(int error)
{
    switch (error) {
    case LM_OK: return kIOReturnSuccess;
    case LM_ENOMEM: return kIOReturnNoMemory;
    case LM_EBUSY: return kIOReturnBusy;
    case LM_ETIMEDOUT: return kIOReturnTimeout;
    case LM_ENODEV: return kIOReturnNotReady;
    case LM_ENOTSUP: return kIOReturnUnsupported;
    case LM_EINVAL: return kIOReturnBadArgument;
    default: return kIOReturnError;
    }
}

class LancelotMaliQueueFence : public OSObject {
    OSDeclareDefaultStructors(LancelotMaliQueueFence);
    IOLock *fLock;
    bool fDone;
    int fResult;
    uint64_t fPairSequence[2];
    int32_t fPairError[2];
    uint32_t fPairStatus[2],fPairAttempted,fPairCompleted;
public:
    uint64_t token;
    static LancelotMaliQueueFence *create() {
        LancelotMaliQueueFence *f = OSTypeAlloc(LancelotMaliQueueFence);
        if (!f || !f->init()) { OSSafeReleaseNULL(f); return NULL; }
        f->fLock = IOLockAlloc();
        if (!f->fLock) { f->release(); return NULL; }
        return f;
    }
    int wait(uint64_t ns) {
        uint64_t began = lmq_clock(), end, absolute;
        if (ns > UINT64_C(5000000000)) ns = UINT64_C(5000000000);
        if (!began || began > UINT64_MAX - ns) return LM_EINVAL;
        end = began + ns; nanoseconds_to_absolutetime(end, &absolute);
        IOLockLock(fLock);
        while (!fDone && ns && lmq_clock() < end)
            IOLockSleepDeadline(fLock, &fDone, absolute, THREAD_UNINT);
        int result = fDone ? fResult : LM_ETIMEDOUT;
        IOLockUnlock(fLock); return result;
    }
    void finish(int error) {
        IOLockLock(fLock);
        fResult = error; fDone = true;
        IOLockWakeup(fLock, &fDone, false);
        IOLockUnlock(fLock);
    }
    void setPair(const uint64_t sequence[2],const int32_t errors[2],const uint32_t status[2],uint32_t attempted,uint32_t completed) {
        IOLockLock(fLock);
        for(unsigned i=0;i<2;i++){fPairSequence[i]=sequence[i];fPairError[i]=errors[i];fPairStatus[i]=status[i];}
        fPairAttempted=attempted;fPairCompleted=completed;IOLockUnlock(fLock);
    }
    void getPair(struct lmal_submit_pair_sync *q) {
        IOLockLock(fLock);
        for(unsigned i=0;i<2;i++){q->sequence[i]=fPairSequence[i];q->stage_error[i]=fPairError[i];q->job_status[i]=fPairStatus[i];}
        q->attempted_mask=fPairAttempted;q->completed_mask=fPairCompleted;IOLockUnlock(fLock);
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fLock) IOLockFree(fLock);
        OSObject::free();
    }
};
OSDefineMetaClassAndStructors(LancelotMaliQueueFence, OSObject);

static IOLock *lmq_surface_lock;
static uint32_t lmq_surface_changed;
extern "C" int lancelot_mali_queue_wait_room(uint64_t deadline_ns);
static LancelotMaliQueueFence *lmq_surface_fence(MiniIOSurface *surface)
{
    if (!surface || !lmq_surface_lock) return NULL;
    IOLockLock(lmq_surface_lock);
    LancelotMaliQueueFence *f = (LancelotMaliQueueFence *)surface->maliQueueFence;
    if (f) f->retain();
    IOLockUnlock(lmq_surface_lock); return f;
}
extern "C" bool LancelotMaliQueueSurfaceWait(MiniIOSurface *surface, uint64_t ns)
{
    LancelotMaliQueueFence *f = lmq_surface_fence(surface);
    if (!f) return true;
    int result = f->wait(ns); f->release(); return result == LM_OK;
}
extern "C" bool LancelotMaliQueueSurfaceReadBegin(MiniIOSurface *surface, uint64_t ns)
{
    if (!surface || !lmq_surface_lock) return surface != NULL;
    /* The presenter's existing LancelotMaliReadGroup acquires this real root.
     * Do not hold a queue reader while waiting for that root's GPU cookie. */
    if (surface->maliCpuLease) return true;
    uint64_t began = lmq_clock();
    for (;;) {
        LancelotMaliQueueFence *f = lmq_surface_fence(surface);
        uint64_t now = lmq_clock();
        int result = now < began || now - began >= ns ? LM_ETIMEDOUT :
            f ? f->wait(ns - (now - began)) : LM_OK;
        if (result) { OSSafeReleaseNULL(f); return false; }
        IOLockLock(lmq_surface_lock);
        bool same = surface->maliQueueFence == f;
        bool room = surface->maliQueueReaders != UINT32_MAX;
        if (same && room) ++surface->maliQueueReaders;
        IOLockUnlock(lmq_surface_lock);
        OSSafeReleaseNULL(f);
        if (same) return room;
    }
}
extern "C" void LancelotMaliQueueSurfaceReadEnd(MiniIOSurface *surface)
{
    if (!surface || !lmq_surface_lock) return;
    if (surface->maliCpuLease) return;
    IOLockLock(lmq_surface_lock);
    if (surface->maliQueueReaders) --surface->maliQueueReaders;
    IOLockWakeup(lmq_surface_lock, &lmq_surface_changed, false);
    IOLockUnlock(lmq_surface_lock);
}
extern "C" void LancelotMaliQueueSurfaceRelease(MiniIOSurface *surface)
{
    if (!surface || !lmq_surface_lock) return;
    IOLockLock(lmq_surface_lock);
    LancelotMaliQueueFence *f = (LancelotMaliQueueFence *)surface->maliQueueFence;
    surface->maliQueueFence = NULL;
    IOLockUnlock(lmq_surface_lock);
    OSSafeReleaseNULL(f);
}

struct lmq_buffer {
    IOMemoryDescriptor *memory;
    LancelotMaliExport *exported;
    LancelotMaliCpuLease *cpuLease;
    MiniIOSurface *surface;
    LancelotMaliQueueFence *last;
    uint64_t va,size;
    uint32_t pending,preparing,flags;
    bool mapped,prepared,quarantined,exportRequested;
};
class LancelotMaliQueuedUserClient;
struct lmq_transaction {
    struct lmq_job job;
    LancelotMaliQueuedUserClient *client;
    LancelotMaliQueueFence *fence;
    struct lmq_buffer **buffers;
    uint32_t capacity;
    LancelotMaliCpuLease **leases;
    uint64_t *cookies;
    bool *writers;
    uint32_t leaseCount;
};

static bool lmal_prepare_queued_client()
{
    if (!lm_gpu_ready() || lancelot_mali_queue_start()) return false;
    if (!lmq_surface_lock) lmq_surface_lock = IOLockAlloc();
    return lmq_surface_lock != NULL;
}

class LancelotMaliQueuedUserClient : public IOUserClient {
    OSDeclareDefaultStructors(LancelotMaliQueuedUserClient);
    task_t fTask;
    int32_t fPid;
    IOLock *fLock;
    struct lm_vm *fVM;
    struct lmq_buffer *fBuffers;
    LancelotMaliQueueFence **fFences;
    uint64_t fNextVA;
    uint32_t fPending,fPreparing;
    uint64_t fJobsSubmitted,fJobsCompleted,fFaults,fResets,fQuarantined;
    int fError;
    bool fClosing,fUnsafe;
    struct lmq_buffer *buffer(uint32_t h) {
        return fBuffers && h && h <= LMQ_MAX_BOS && fBuffers[h-1].memory &&
            !fBuffers[h-1].quarantined ? &fBuffers[h-1] : NULL;
    }
    int waitBuffer(struct lmq_buffer *b, uint64_t ns) {
        LancelotMaliQueueFence *f = b->last;
        if (!f) return LM_OK;
        IOMemoryDescriptor *memory=b->memory;
        memory->retain();f->retain(); IOLockUnlock(fLock);
        int result = f->wait(ns);
        IOLockLock(fLock);
        if(b->memory!=memory || b->last!=f || b->quarantined)result=LM_EBUSY;
        memory->release();f->release(); return result;
    }
    int waitPreparation(struct lmq_buffer *b,uint64_t ns) {
        auto *wanted=b->memory;wanted->retain();
        uint64_t now=lmq_clock(),absolute,end=now+ns;nanoseconds_to_absolutetime(end,&absolute);
        while(b->memory==wanted && b->preparing && lmq_clock()<end)
            IOLockSleepDeadline(fLock,&fPreparing,absolute,THREAD_UNINT);
        int e=b->memory!=wanted?LM_EBUSY:b->preparing?LM_ETIMEDOUT:LM_OK;
        wanted->release();return e;
    }
    bool releaseBuffer(struct lmq_buffer *b) {
        if (!b->memory) return true;
        if (b->pending || b->preparing || fUnsafe || b->quarantined) return false;
        if (b->exported && b->exported->revoke(true) != LM_OK) return false;
        if (b->mapped && (lm_gpu_vm_unmap(fVM,b->va,(uint32_t)(b->size/4096)) ||
            !lm_gpu_vm_safe_to_free(fVM))) return false;
        b->mapped = false;
        if (b->prepared && b->memory->complete() != kIOReturnSuccess) return false;
        b->prepared=false;
        OSSafeReleaseNULL(b->exported); OSSafeReleaseNULL(b->cpuLease);
        OSSafeReleaseNULL(b->memory);
        OSSafeReleaseNULL(b->last);
        if (b->surface) MiniIOSurfaceDrop(b->surface);
        bzero(b,sizeof(*b)); return true;
    }
    int newBuffer(IOMemoryDescriptor *memory, uint8_t *cpu, uint64_t size,
        MiniIOSurface *surface, uint32_t flags, uint32_t *handle, LancelotMaliCpuLease *lease = NULL) {
        if (!memory || !cpu || !size || (size & PAGE_MASK) || size > LMQ_MAX_BYTES)
            return LM_EINVAL;
        uint32_t h;
        for (h=1;h<=LMQ_MAX_BOS && fBuffers[h-1].memory;h++) {}
        if (h>LMQ_MAX_BOS) return LM_ENOMEM;
        uint64_t va=0;
        for(unsigned pass=0;pass<2 && !va;pass++) {
            uint64_t candidate=pass?LM_VA_MIN:fNextVA;
            for(uint32_t scan=0;scan<=LMQ_MAX_BOS;scan++) {
                if(candidate<LM_VA_MIN || candidate>LM_VA_END-size-65536)break;
                uint32_t i;
                for(i=0;i<LMQ_MAX_BOS;i++) {
                    const struct lmq_buffer *other=&fBuffers[i];
                    if(other->memory && candidate<other->va+other->size+65536 &&
                        other->va<candidate+size+65536) {
                        candidate=other->va+other->size+65536;break;
                    }
                }
                if(i==LMQ_MAX_BOS){va=candidate;break;}
            }
        }
        if(!va)return LM_ENOMEM;
        struct lm_page *pages = IONew(struct lm_page,(uint32_t)(size/4096));
        if (!pages) return LM_ENOMEM;
        int error=LM_OK;
        for (uint32_t i=0;i<size/4096;i++) {
            IOByteCount length=0;
            uint64_t pa=memory->getPhysicalSegment((uint64_t)i*4096,&length,kIOMemoryMapperNone);
            if (!pa || (pa&4095) || length<4096) {error=LM_EINVAL;break;}
            pages[i].pa=pa;pages[i].cpu=cpu+(uint64_t)i*4096;
        }
        if (!error) error=lm_gpu_vm_map(fVM,va,pages,(uint32_t)(size/4096),
            LM_MAP_WRITE | ((flags&LMQ_BO_NOEXEC)?0:LM_MAP_EXEC));
        IODelete(pages,struct lm_page,(uint32_t)(size/4096));
        if (error) return error;
        struct lmq_buffer *b=&fBuffers[h-1];
        b->memory=memory;b->surface=surface;b->va=va;b->size=size;b->flags=flags;b->cpuLease=lease;
        b->mapped=b->prepared=true;fNextVA=va+size+65536;*handle=h;return LM_OK;
    }
    int create(struct lmq_create *q) {
        if (!q->size || q->size>LMQ_MAX_BYTES || q->flags&~3u || q->pad || q->handle || q->offset)
            return LM_EINVAL;
        uint64_t size=round_page_64(q->size);
        IOBufferMemoryDescriptor *memory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut|kIOMemoryKernelUserShared|kIOMapWriteCombineCache,size,PAGE_SIZE);
        if (!memory) return LM_ENOMEM;
        if (memory->prepare()!=kIOReturnSuccess) {memory->release();return LM_EIO;}
        struct lmal_cpu_lease_why why={};
        LancelotMaliCpuLease *lease=LancelotMaliCpuLease::withRoot(memory,true,2,&why);
        lmal_lease_diag_create(0,size,q->flags,lease!=NULL,&why);
        if(!lease){memory->complete();memory->release();return LM_ENOTSUP;}
        int error=newBuffer(memory,(uint8_t *)lease->cacheAddress(),size,NULL,q->flags,&q->handle,lease);
        if (error) {lease->release();memory->complete();memory->release();return error;}
        q->size=(uint32_t)size;q->offset=buffer(q->handle)->va;return LM_OK;
    }
    int surface(struct lmq_surface *q) {
        MiniIOSurface *s=MiniIOSurfaceCopy(q->surface);
        if (!s) return LM_EINVAL;
        uint64_t size=0;
        if (!lmal_surface_metadata(s,&size)) {
            MiniIOSurfaceDrop(s);return LM_ENOTSUP;
        }
        q->size=size;q->offset=s->info.offset;q->rowBytes=s->info.bytesPerRow;
        q->width=s->info.width;q->height=s->info.height;q->pixelFormat=s->info.pixelFormat;q->planes=s->info.planeCount;
        if (q->handle==LMQ_DESCRIBE) {q->handle=0;MiniIOSurfaceDrop(s);return LM_OK;}
        for (uint32_t i=0;i<LMQ_MAX_BOS;i++) if (fBuffers[i].surface==s && buffer(i+1)) {
            q->handle=i+1;MiniIOSurfaceDrop(s);return LM_OK;
        }
        IOSubMemoryDescriptor *md=IOSubMemoryDescriptor::withSubRange(s->memory,PAGE_SIZE,size,kIODirectionInOut);
        if (!md) {MiniIOSurfaceDrop(s);return LM_ENOMEM;}
        if (md->prepare()!=kIOReturnSuccess) {md->release();MiniIOSurfaceDrop(s);return LM_EIO;}
        LancelotMaliCpuLease *lease=s->maliCpuLease;
        if(lease)lease->retain();
        int error=newBuffer(md,s->kpixels,size,s,LMQ_BO_NOEXEC,&q->handle,lease);
        if (error) {OSSafeReleaseNULL(lease);md->complete();md->release();MiniIOSurfaceDrop(s);}
        else lmal_lease_diag_import(q->handle,q->surface,size,lease!=NULL);
        return error;
    }
    static int ready(struct lmq_job *job) {
        LancelotMaliQueuedUserClient *c=(LancelotMaliQueuedUserClient *)job->owner;
        for(uint32_t tries=0;tries<5000;tries++) {
            if(IOLockTryLock(c->fLock)){int error=c->fError;IOLockUnlock(c->fLock);return error;}
            uint64_t now=lmq_clock();
            if(!now || now<job->queued_ns || now>=job->deadline_ns)return LM_ETIMEDOUT;
            IOSleep(1);
        }
        return LM_ETIMEDOUT;
    }
    static void complete(struct lmq_job *job,const struct lm_job_result *result,int error) {
        struct lmq_transaction *t=(struct lmq_transaction *)job;
        LancelotMaliQueuedUserClient *c=t->client;
        bool safe=!result[0].quarantined && (!job->fs_jc || !result[1].quarantined) && lm_gpu_vm_safe_to_free(job->vm);
        IOLockLock(c->fLock);
        if(job->fs_jc) {
            uint64_t sequence[2]={result[0].sequence,result[1].sequence};
            int32_t errors[2]={result[0].pair_stage_error,result[1].pair_stage_error};
            uint32_t status[2]={result[0].job_status,result[1].job_status},attempted=0,completed=0;
            for(unsigned i=0;i<2;i++)if(result[i].pair_stage_attempted){attempted|=1u<<i;if(!errors[i]&&sequence[i])completed|=1u<<i;}
            t->fence->setPair(sequence,errors,status,attempted,completed);
        }
        for(uint32_t i=t->leaseCount;i;i--)if(t->cookies[i-1]) {
            int ended=t->leases[i-1]->endGpu(t->cookies[i-1],error,safe);
            t->cookies[i-1]=0;
            if(ended){safe=false;if(!error)error=ended;}
        }
        if(error && !c->fError)c->fError=error;
        if(!error && safe)++c->fJobsCompleted;
        for(unsigned i=0;i<(job->fs_jc?2u:1u);i++){
            if(result[i].gpu_fault || result[i].mmu_fault)++c->fFaults;
            if(result[i].reset_completed)++c->fResets;
        }
        if (!safe) c->fUnsafe=true;
        for (uint32_t i=0;i<job->count;i++) {
            struct lmq_buffer *b=t->buffers[i];
            --b->pending;
            if (!safe && !b->quarantined){b->quarantined=true;++c->fQuarantined;}
        }
        --c->fPending;
        /* Success is visible only after core cache/AS retirement and BO state.
         * Unknown DMA keeps memory/VM/client alive, and reports an error. */
        t->fence->finish(safe?error:LM_EIO);
        IOLockWakeup(c->fLock,&c->fPending,false);
        IOLockUnlock(c->fLock);
        IODelete(job->ranges,struct lm_range,t->capacity);
        IODelete(t->buffers,struct lmq_buffer *,t->capacity);
        IODelete(t->leases,LancelotMaliCpuLease *,t->capacity);
        IODelete(t->cookies,uint64_t,t->capacity);IODelete(t->writers,bool,t->capacity);
        t->fence->release();IODelete(t,struct lmq_transaction,1);
        if (safe) c->release(); /* unsafe retains the queue's owner reference */
    }
    static bool presenterBusy(const struct lmq_transaction *t) {
        for(uint32_t i=0;i<t->job.count;i++)
            if(!t->buffers[i]->cpuLease && t->buffers[i]->surface && t->buffers[i]->surface->maliQueueReaders)return true;
        return false;
    }
    /* Called with fLock and preparation pins held. Success returns both locks;
     * failure returns only fLock. Every sleep releases the client lock so the
     * worker can retire a producer needed by the display's reader group. */
    int admitPresenter(struct lmq_transaction *t,uint64_t deadline) {
        uint64_t absolute;nanoseconds_to_absolutetime(deadline,&absolute);
        for(;;) {
            uint64_t now=lmq_clock();
            if(fClosing || fUnsafe)return LM_ENODEV;
            if(fError)return fError;
            if(!now || now>=deadline)return LM_ETIMEDOUT;
            IOLockUnlock(fLock);
            IOLockLock(lmq_surface_lock);
            if(presenterBusy(t) && lmq_clock()<deadline)
                IOLockSleepDeadline(lmq_surface_lock,&lmq_surface_changed,absolute,THREAD_UNINT);
            IOLockUnlock(lmq_surface_lock);
            IOLockLock(fLock);
            now=lmq_clock();
            if(fClosing || fUnsafe)return LM_ENODEV;
            if(fError)return fError;
            if(!now || now>=deadline)return LM_ETIMEDOUT;
            IOLockLock(lmq_surface_lock);
            if(!presenterBusy(t))return LM_OK;
            IOLockUnlock(lmq_surface_lock);
        }
    }
    int submit(struct lmq_submit *q, uint64_t readonly_address = 0,
        const uint32_t *local_handles=NULL,const uint32_t *local_readonly=NULL,
        uint64_t fs_jc=0,uint64_t budget=UINT64_C(5000000000)) {
        if (!q->jc || (q->jc&63) || q->requirements>1 || !q->bo_handle_count ||
            q->bo_handle_count>LMQ_MAX_BOS || !q->bo_handles || q->in_sync_count ||
            q->in_syncs || q->out_sync || !budget || budget>UINT64_C(5000000000) ||
            (fs_jc && ((fs_jc&63u)||q->requirements))) return LM_EINVAL;
        uint32_t fence_slot;
        for (fence_slot=0;fence_slot<LMQ_FENCE_SLOTS && fFences[fence_slot];fence_slot++) {}
        if (fence_slot==LMQ_FENCE_SLOTS) return LM_EBUSY;
        uint32_t *handles=IONew(uint32_t,q->bo_handle_count);
        uint32_t *readonly=(readonly_address||local_readonly)?IONew(uint32_t,q->bo_handle_count):NULL;
        struct lmq_transaction *t=IONewZero(struct lmq_transaction,1);
        if (!handles || !t || ((readonly_address||local_readonly)&&!readonly)) {
            if(handles)IODelete(handles,uint32_t,q->bo_handle_count);
            if(readonly)IODelete(readonly,uint32_t,q->bo_handle_count);
            if(t)IODelete(t,struct lmq_transaction,1);
            return LM_ENOMEM;
        }
        t->buffers=IONew(struct lmq_buffer *,q->bo_handle_count);
        t->capacity=q->bo_handle_count;
        t->job.ranges=IONew(struct lm_range,q->bo_handle_count);
        t->leases=IONew(LancelotMaliCpuLease *,q->bo_handle_count);
        t->cookies=IONewZero(uint64_t,q->bo_handle_count);
        t->writers=IONew(bool,q->bo_handle_count);
        t->fence=LancelotMaliQueueFence::create();
        int error=LM_OK;
        if (!t->buffers || !t->job.ranges || !t->fence || !t->leases || !t->cookies || !t->writers) error=LM_ENOMEM;
        if(!error && local_handles)memcpy(handles,local_handles,q->bo_handle_count*sizeof(*handles));
        else if (!error && copyin((user_addr_t)q->bo_handles,handles,q->bo_handle_count*sizeof(*handles))) error=LM_EINVAL;
        if(!error && local_readonly)memcpy(readonly,local_readonly,q->bo_handle_count*sizeof(*readonly));
        else if (!error && readonly && copyin((user_addr_t)readonly_address,readonly,q->bo_handle_count*sizeof(*readonly))) error=LM_EINVAL;
        bool jc=false,fs=!fs_jc;
        for (uint32_t i=0;!error && i<q->bo_handle_count;i++) {
            struct lmq_buffer *b=buffer(handles[i]);
            if (!b || !b->mapped || !b->prepared || b->pending==UINT32_MAX || b->preparing==UINT32_MAX ||
                (readonly && readonly[i]>1u)) {error=LM_EINVAL;break;}
            bool contains_job=q->jc>=b->va && q->jc-b->va<=b->size-64;
            bool contains_fs=fs_jc && fs_jc>=b->va && fs_jc-b->va<=b->size-64;
            if((contains_job||contains_fs) && readonly && readonly[i]){error=LM_EINVAL;break;}
            jc|=contains_job;fs|=contains_fs;
            uint32_t j;for(j=0;j<t->job.count && t->buffers[j]!=b;j++) {}
            uint32_t access=readonly && readonly[i]?LM_RANGE_READONLY:0;
            if(j<t->job.count){t->job.ranges[j].flags &= access;continue;}
            if(contains_job)jc=true;
            t->buffers[j]=b;t->job.ranges[j]=(struct lm_range){b->va,(uint32_t)(b->size/4096),access};
            ++t->job.count;
        }
        if (!error && (!jc||!fs)) error=LM_EINVAL;
        bool preparing=false;
        uint64_t now=lmq_clock(),deadline=0;
        if(!error && (!now || now>UINT64_MAX-UINT64_C(5000000000) || fPreparing==UINT32_MAX))error=LM_EINVAL;
        if(!error) {
            deadline=now+budget;++fPreparing;preparing=true;
            for(uint32_t i=0;i<t->job.count;i++)++t->buffers[i]->preparing;
            sealRanges(t,q->jc,fs_jc);
            collectLeases(t);
            IOLockUnlock(fLock);
            error=acquireLeases(t,deadline);
            IOLockLock(fLock);
            if(!error)error=admitPresenter(t,deadline);
        }
        if(error)IOLockLock(lmq_surface_lock);
        if (!error) {
            t->client=this;t->job.vm=fVM;t->job.jc=q->jc;t->job.fs_jc=fs_jc;t->job.requirements=q->requirements;
            t->job.owner=this;t->job.owner_pid=fPid;t->job.deadline_ns=deadline;
            t->job.complete=complete;t->job.ready=ready;
            for(;;) {
                for(fence_slot=0;fence_slot<LMQ_FENCE_SLOTS && fFences[fence_slot];fence_slot++) {}
                if(fence_slot==LMQ_FENCE_SLOTS){error=LM_EBUSY;break;}
                sealRanges(t,q->jc,fs_jc);
                retain();
                error=lancelot_mali_enqueue(&t->job,&t->fence->token);
                if (error) release();
                if(error!=LM_EBUSY)break;
                IOLockUnlock(lmq_surface_lock);IOLockUnlock(fLock);
                error=lancelot_mali_queue_wait_room(deadline);
                IOLockLock(fLock);
                if(!error)error=admitPresenter(t,deadline);
                if(error){IOLockLock(lmq_surface_lock);break;}
            }
        }
        if (!error) {
            fFences[fence_slot]=t->fence;t->fence->retain();++fPending;++fJobsSubmitted;
            for (uint32_t i=0;i<t->job.count;i++) {
                struct lmq_buffer *b=t->buffers[i];++b->pending;
                OSSafeReleaseNULL(b->last);b->last=t->fence;b->last->retain();
                if (b->surface) {
                    LancelotMaliQueueFence *old=(LancelotMaliQueueFence *)b->surface->maliQueueFence;
                    b->surface->maliQueueFence=t->fence;t->fence->retain();OSSafeReleaseNULL(old);
                }
            }
            q->jc=t->job.token; /* queue token, never advertised as completion */
        }
        IOLockUnlock(lmq_surface_lock);
        if(preparing) {
            for(uint32_t i=0;i<t->job.count;i++)--t->buffers[i]->preparing;
            --fPreparing;IOLockWakeup(fLock,&fPreparing,false);
        }
        IODelete(handles,uint32_t,q->bo_handle_count);
        if(readonly)IODelete(readonly,uint32_t,q->bo_handle_count);
        if (error) {
            for(uint32_t i=t->leaseCount;i;i--)if(t->cookies[i-1]) {
                if(t->leases[i-1]->endGpu(t->cookies[i-1],error,true)) {
                    fUnsafe=true;
                    for(uint32_t k=0;k<t->job.count;k++)t->buffers[k]->quarantined=true;
                }
            }
            if(t->buffers)IODelete(t->buffers,struct lmq_buffer *,q->bo_handle_count);
            if(t->job.ranges)IODelete(t->job.ranges,struct lm_range,q->bo_handle_count);
            if(t->leases)IODelete(t->leases,LancelotMaliCpuLease *,q->bo_handle_count);
            if(t->cookies)IODelete(t->cookies,uint64_t,q->bo_handle_count);
            if(t->writers)IODelete(t->writers,bool,q->bo_handle_count);
            OSSafeReleaseNULL(t->fence);IODelete(t,struct lmq_transaction,1);
        }
        return error;
    }
    static void collectLeases(struct lmq_transaction *t) {
        for(uint32_t i=0;i<t->job.count;i++) {
            LancelotMaliCpuLease *lease=t->buffers[i]->cpuLease;
            if(!lease || (t->job.ranges[i].flags&LM_RANGE_GPU_ONLY))continue;
            uint32_t j=0;while(j<t->leaseCount && (uintptr_t)t->leases[j]<(uintptr_t)lease)++j;
            bool writer=!(t->job.ranges[i].flags&LM_RANGE_READONLY);
            if(j<t->leaseCount && t->leases[j]==lease){t->writers[j]|=writer;continue;}
            for(uint32_t k=t->leaseCount;k>j;--k){t->leases[k]=t->leases[k-1];t->writers[k]=t->writers[k-1];}
            t->leases[j]=lease;t->writers[j]=writer;++t->leaseCount;
        }
    }
    static int acquireLeases(struct lmq_transaction *t,uint64_t deadline) {
        for(uint32_t i=0;i<t->leaseCount;i++) {
            uint64_t now=lmq_clock();if(!now || now>=deadline)return LM_ETIMEDOUT;
            int e=t->leases[i]->beginGpu(deadline-now,&t->cookies[i],false,t->writers[i]);
            if(e)return e;
        }
        for(uint32_t i=0;i<t->leaseCount;i++) {
            int e=t->leases[i]->markLegacy(t->cookies[i]);if(e)return e;
        }
        return LM_OK;
    }
    static void sealRanges(struct lmq_transaction *t,uint64_t jc,uint64_t fs_jc=0) {
        /* Preparation/wait_room may release fLock. Recheck never-exported WC
         * ownership immediately before acceptance while fLock is held. */
        for(uint32_t i=0;i<t->job.count;i++) {
            struct lmq_buffer *b=t->buffers[i];
            bool job=(jc>=b->va && jc-b->va<b->size)||(fs_jc && fs_jc>=b->va && fs_jc-b->va<b->size);
            bool owned=lmal_client_gpu_only_eligible(true,b->surface!=NULL,
                b->exportRequested || b->exported!=NULL,job);
            t->job.ranges[i].flags=(t->job.ranges[i].flags&LM_RANGE_READONLY)|(owned?LM_RANGE_GPU_ONLY:0);
        }
    }
    int pairUnion(struct lmal_submit_pair_sync *q,uint32_t *handles,uint32_t *readonly,
        uint32_t *count,struct lmal_pair_admission *admission) {
        bzero(admission,sizeof(*admission));admission->stage_index=LMAL_PAIR_STAGE_NONE;*count=0;
        if(!q->timeout_ns || q->timeout_ns>LMAL_MAX_WAIT_NS || q->in_sync_count || q->in_syncs || q->out_sync ||
            q->reserved[0] || q->reserved[1] || q->attempted_mask || q->completed_mask ||
            q->sequence[0] || q->sequence[1] || q->job_status[0] || q->job_status[1] ||
            q->stage_error[0] || q->stage_error[1])return LM_EINVAL;
        for(unsigned stage=0;stage<2;stage++) {
            const struct lmal_sync_pair_stage *s=&q->stage[stage];
            if(!s->jc || (s->jc&63u) || !s->bo_count || s->bo_count>LMAL_SYNC_PAIR_STAGE_BOS ||
                s->reserved || !s->bo_handles || !s->bo_readonly)return LM_EINVAL;
            uint32_t offset=*count;
            if(copyin((user_addr_t)s->bo_handles,handles+offset,s->bo_count*sizeof(*handles)) ||
                copyin((user_addr_t)s->bo_readonly,readonly+offset,s->bo_count*sizeof(*readonly)))return LM_EINVAL;
            bool jc=false;
            for(uint32_t i=0;i<s->bo_count;i++) {
                uint32_t h=handles[offset+i],ro=readonly[offset+i];
                struct lmq_buffer *b=buffer(h);
                if(!b || !b->mapped || !b->prepared || ro>1)return LM_EINVAL;
                bool job=s->jc>=b->va && s->jc-b->va<=b->size-64;
                if(job && ro)return LM_EINVAL;
                jc|=job;
                if(!b->cpuLease && !admission->reason) {
                    admission->reason=LMAL_PAIR_REASON_MISSING_LEASE;admission->stage_index=stage;
                    admission->bo_handle=h;admission->surface_id=b->surface?b->surface->sid:0;
                    admission->bo_flags=b->flags;
                    admission->bo_state=LMAL_PAIR_BO_MEMORY|LMAL_PAIR_BO_MAPPED|LMAL_PAIR_BO_PREPARED;
                }
                uint32_t j=0;while(j<*count && handles[j]!=h)++j;
                if(j<*count)readonly[j]&=ro;
                else {handles[*count]=h;readonly[*count]=ro;++*count;}
            }
            if(!jc)return LM_EINVAL;
        }
        admission->eligible=admission->reason==LMAL_PAIR_REASON_NONE;return LM_OK;
    }
    IOReturn pairMethod(uint32_t selector,IOExternalMethodArguments *args) {
        bool check=selector==LMAL_SELECTOR_CHECK_PAIR_SYNC;
        size_t bytes=check?sizeof(lmal_check_pair_sync):sizeof(lmal_submit_pair_sync);
        if(!args || current_task()!=fTask || !fLock || !args->structureInput || args->structureInputSize!=bytes ||
            !args->structureOutput || args->structureOutputSize<bytes || args->scalarInputCount || args->scalarOutputCount ||
            args->asyncReferenceCount || args->asyncWakePort || args->structureInputDescriptor || args->structureOutputDescriptor)return kIOReturnBadArgument;
        struct lmal_check_pair_sync q={};memcpy(&q,args->structureInput,bytes);
        const struct lmal_header *h=&q.submit.h;
        if(h->magic!=LMAL_UAPI_MAGIC || h->version!=LMAL_UAPI_VERSION || h->size!=bytes ||
            h->flags || h->reserved0 || h->reserved1 || h->result)return kIOReturnBadArgument;
        uint32_t capacity=LMAL_SYNC_PAIR_STAGE_BOS*2,count=0;
        uint32_t *handles=IONew(uint32_t,capacity),*readonly=IONew(uint32_t,capacity);
        retain();IOLockLock(fLock);
        int e=fClosing || fUnsafe || !lm_gpu_ready()?LM_ENODEV:fError?fError:LM_OK;
        if(!e && (!handles||!readonly))e=LM_ENOMEM;
        if(!e)e=pairUnion(&q.submit,handles,readonly,&count,&q.admission);
        if(!e && !check) {
            if(!q.admission.eligible)e=LM_ENOTSUP;
            else {
                struct lmq_submit raw={q.submit.stage[0].jc,0,0,0,(uint64_t)(uintptr_t)handles,count,0};
                e=submit(&raw,0,handles,readonly,q.submit.stage[1].jc,q.submit.timeout_ns);
                if(!e) {
                    uint32_t i=0;while(i<LMQ_FENCE_SLOTS && (!fFences[i]||fFences[i]->token!=raw.jc))++i;
                    if(i==LMQ_FENCE_SLOTS)e=LM_EIO;
                    else {
                        LancelotMaliQueueFence *f=fFences[i];f->retain();IOLockUnlock(fLock);
                        e=f->wait(q.submit.timeout_ns);f->getPair(&q.submit);
                        IOLockLock(fLock);
                        if(e!=LM_ETIMEDOUT && fFences[i]==f)OSSafeReleaseNULL(fFences[i]);
                        f->release();
                    }
                }
            }
        }
        q.submit.h.result=e;memcpy(args->structureOutput,&q,bytes);args->structureOutputSize=bytes;
        IOLockUnlock(fLock);release();
        if(handles)IODelete(handles,uint32_t,capacity);
        if(readonly)IODelete(readonly,uint32_t,capacity);
        return kIOReturnSuccess;
    }
public:
    bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *props) APPLE_KEXT_OVERRIDE {
        if (!lancelot_mali_queued_backend() || type != LMAL_OPEN_TYPE || !task || !IOUserClient::initWithTask(task,security,type,props)) return false;
        proc_t p=(proc_t)get_bsdtask_info(task);
        if (!p) return false;
        bool allowed=lmal_authorized_task(task);
        if (!allowed) return false;
        fTask=task;task_reference(task);fPid=proc_pid(p);return true;
    }
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (!lm_gpu_ready() || !IOUserClient::start(provider)) return false;
        fLock=IOLockAlloc();fBuffers=IONewZero(struct lmq_buffer,LMQ_MAX_BOS);
        fFences=IONewZero(LancelotMaliQueueFence *,LMQ_FENCE_SLOTS);fNextVA=LM_VA_MIN;
        if (!fLock || !fBuffers || !fFences) return false;
        fVM=lm_gpu_vm_new();return fVM!=NULL;
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
        if (!fLock) return kIOReturnSuccess;
        uint64_t now=lmq_clock(),absolute;nanoseconds_to_absolutetime(now+UINT64_C(5000000000),&absolute);
        IOLockLock(fLock);fClosing=true;
        IOLockLock(lmq_surface_lock);IOLockWakeup(lmq_surface_lock,&lmq_surface_changed,false);IOLockUnlock(lmq_surface_lock);
        while((fPending || fPreparing) && lmq_clock()<now+UINT64_C(5000000000))
            IOLockSleepDeadline(fLock,fPreparing?(void *)&fPreparing:(void *)&fPending,absolute,THREAD_UNINT);
        bool drained=!fPending && !fPreparing && !fUnsafe;
        IOLockUnlock(fLock);terminate();return drained?kIOReturnSuccess:kIOReturnBusy;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fLock) IOLockLock(fLock);
        bool safe=!fPending && !fPreparing && !fUnsafe;
        if (fBuffers) for(uint32_t i=0;i<LMQ_MAX_BOS;i++)
            if(!releaseBuffer(&fBuffers[i]))safe=false;
        if (safe && fVM && lm_gpu_vm_destroy(fVM))safe=false;
        if (!safe) { if(fLock)IOLockUnlock(fLock);IOLog("LancelotMali queue: retaining unsafe VM and backing at close\n");return; }
        if(fFences){for(uint32_t i=0;i<LMQ_FENCE_SLOTS;i++)OSSafeReleaseNULL(fFences[i]);IODelete(fFences,LancelotMaliQueueFence *,LMQ_FENCE_SLOTS);}
        if(fBuffers)IODelete(fBuffers,struct lmq_buffer,LMQ_MAX_BOS);
        if(fLock){IOLockUnlock(fLock);IOLockFree(fLock);}
        if(fTask)task_deallocate(fTask);IOUserClient::free();
    }
    IOReturn clientMemoryForType(UInt32 h,IOOptionBits *options,IOMemoryDescriptor **out) APPLE_KEXT_OVERRIDE {
        if (!fLock || current_task()!=fTask) return kIOReturnNotPermitted;
        IOLockLock(fLock);struct lmq_buffer *b=buffer(h);int error=LM_EINVAL;
        if(b && !fClosing && !fUnsafe){
            error=waitPreparation(b,UINT64_C(5000000000));
            if(!error)b->exportRequested=true;
            if(!error)error=waitBuffer(b,UINT64_C(5000000000));
            if(!error && (fClosing || fUnsafe))error=LM_ENODEV;
            if(!error)error=lm_gpu_vm_cpu_access(fVM,b->va,(uint32_t)(b->size/4096));
            if(!error && !b->exported)b->exported=LancelotMaliExport::withBacking(b->memory,fTask,b->cpuLease);
            if(!error && !b->exported)error=LM_ENOMEM;
            if(!error){b->exported->retain();*out=b->exported;*options=0;}
        }
        IOLockUnlock(fLock);return lmq_return(error);
    }
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,
        IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE {
        if(selector==LMAL_SELECTOR_CHECK_PAIR_SYNC || selector==LMAL_SELECTOR_SUBMIT_PAIR_SYNC)
            return pairMethod(selector,args);
        if(selector==LMAL_SELECTOR_GET_CAPS) {
            if(!args || current_task()!=fTask || !fLock || !args->structureInput ||
                args->structureInputSize!=sizeof(lmal_caps) || !args->structureOutput ||
                args->structureOutputSize<sizeof(lmal_caps) || args->scalarInputCount ||
                args->scalarOutputCount || args->asyncReferenceCount || args->asyncWakePort ||
                args->structureInputDescriptor || args->structureOutputDescriptor)return kIOReturnBadArgument;
            const lmal_caps *input=(const lmal_caps *)args->structureInput;
            if(input->h.magic!=LMAL_UAPI_MAGIC || input->h.version!=LMAL_UAPI_VERSION ||
                input->h.size!=sizeof(lmal_caps) || input->h.flags || input->h.reserved0 ||
                input->h.reserved1 || input->h.result)return kIOReturnBadArgument;
            lmal_caps caps={};caps.h=input->h;
            IOLockLock(fLock);
            caps.jobs_submitted=fJobsSubmitted;caps.jobs_completed=fJobsCompleted;
            caps.faults=fFaults;caps.resets=fResets;caps.quarantined=fQuarantined;
            IOLockUnlock(fLock);
            caps.max_bo_bytes=LMAL_MAX_BO_BYTES;caps.max_vm_bytes=LMAL_MAX_VM_BYTES;
            caps.flags=LMAL_CAP_QUEUED_DRM|LMAL_CAP_SUBMIT_ACCESS|LMAL_CAP_SUBMIT_PAIR_SYNC|LMAL_CAP_CHECK_PAIR_SYNC;
            memcpy(args->structureOutput,&caps,sizeof(caps));args->structureOutputSize=sizeof(caps);
            return kIOReturnSuccess;
        }
        size_t expected=0;
        switch(selector){
        case LMQ_CLOSE:expected=sizeof(lmq_close);break;
        case LMQ_SUBMIT:expected=sizeof(lmq_submit);break;
        case LMQ_SUBMIT_ACCESS:expected=sizeof(lmq_submit_access);break;
        case LMQ_WAIT_BO:expected=sizeof(lmq_wait_bo);break;
        case LMQ_CREATE:expected=sizeof(lmq_create);break;
        case LMQ_MMAP:expected=sizeof(lmq_mmap);break;
        case LMQ_PARAM:expected=sizeof(lmq_param);break;
        case LMQ_OFFSET:expected=sizeof(lmq_offset);break;
        case LMQ_MADVISE:expected=sizeof(lmq_madvise);break;
        case LMQ_WAIT:case LMQ_ACK:expected=sizeof(lmq_wait);break;
        case LMQ_SURFACE:expected=sizeof(lmq_surface);break;
        default:return kIOReturnUnsupported;
        }
        if(!args || current_task()!=fTask || !fLock || !args->structureInput ||
            args->structureInputSize!=expected || !args->structureOutput || args->structureOutputSize<expected ||
            args->scalarInputCount || args->scalarOutputCount || args->asyncReferenceCount || args->asyncWakePort ||
            args->structureInputDescriptor || args->structureOutputDescriptor)return kIOReturnBadArgument;
        union{lmq_close close;lmq_submit submit;lmq_submit_access access;lmq_wait_bo waitbo;lmq_create create;
            lmq_mmap mmap;lmq_param param;lmq_offset offset;lmq_madvise madvise;
            lmq_wait wait;lmq_surface surface;}q={};
        memcpy(&q,args->structureInput,expected);
        retain();IOLockLock(fLock);int error=fClosing || fUnsafe?LM_ENODEV:LM_OK;
        if(!error && fError && selector!=LMQ_WAIT && selector!=LMQ_WAIT_BO && selector!=LMQ_ACK && selector!=LMQ_CLOSE)error=fError;
        struct lmq_buffer *b=NULL;
        if(!error)switch(selector){
        case LMQ_PARAM:error=q.param.pad?LM_EINVAL:lm_gpu_param(q.param.param,&q.param.value);break;
        case LMQ_CREATE:error=create(&q.create);break;
        case LMQ_SURFACE:error=surface(&q.surface);break;
        case LMQ_SUBMIT:error=submit(&q.submit);break;
        case LMQ_SUBMIT_ACCESS:error=!q.access.bo_readonly?LM_EINVAL:submit(&q.access.submit,q.access.bo_readonly);break;
        case LMQ_MMAP:b=buffer(q.mmap.handle);error=!b || q.mmap.flags?LM_EINVAL:LM_OK;q.mmap.offset=(uint64_t)q.mmap.handle<<12;break;
        case LMQ_OFFSET:b=buffer(q.offset.handle);error=!b || q.offset.pad?LM_EINVAL:LM_OK;q.offset.offset=b?b->va:0;break;
        case LMQ_MADVISE:b=buffer(q.madvise.handle);error=!b || q.madvise.madv>1?LM_EINVAL:LM_OK;q.madvise.retained=!error;break;
        case LMQ_WAIT_BO:b=buffer(q.waitbo.handle);error=!b || q.waitbo.pad?LM_EINVAL:waitBuffer(b,q.waitbo.timeout_ns>0?(uint64_t)q.waitbo.timeout_ns:0);break;
        case LMQ_WAIT:case LMQ_ACK:{
            uint32_t i;for(i=0;i<LMQ_FENCE_SLOTS;i++)if(fFences[i] && fFences[i]->token==q.wait.seq)break;
            if(i==LMQ_FENCE_SLOTS){error=LM_EINVAL;break;}
            LancelotMaliQueueFence *f=fFences[i];f->retain();IOLockUnlock(fLock);
            error=f->wait(selector==LMQ_ACK?0:q.wait.timeout_ns>0?(uint64_t)q.wait.timeout_ns:0);
            IOLockLock(fLock);
            if(selector==LMQ_ACK && error!=LM_ETIMEDOUT && fFences[i]==f)OSSafeReleaseNULL(fFences[i]);
            f->release();break;}
        case LMQ_CLOSE:{
            b=buffer(q.close.handle);
            if(!b || q.close.pad){error=LM_EINVAL;break;}
            IOMemoryDescriptor *wanted=b->memory;wanted->retain();
            uint64_t end=lmq_clock()+UINT64_C(5000000000),absolute;nanoseconds_to_absolutetime(end,&absolute);
            while(b->memory==wanted && b->preparing && lmq_clock()<end)
                IOLockSleepDeadline(fLock,&fPreparing,absolute,THREAD_UNINT);
            error=b->memory!=wanted?LM_EINVAL:b->preparing?LM_ETIMEDOUT:waitBuffer(b,UINT64_C(5000000000));
            if(!error && !releaseBuffer(b)){b->quarantined=true;fUnsafe=true;error=LM_EIO;}
            wanted->release();break;}
        }
        if(!error){memcpy(args->structureOutput,&q,expected);args->structureOutputSize=expected;}
        IOLockUnlock(fLock);release();return lmq_return(error);
    }
};
OSDefineMetaClassAndStructors(LancelotMaliQueuedUserClient, IOUserClient);

static IOUserClient *lmal_create_queued_client() { return new LancelotMaliQueuedUserClient; }
