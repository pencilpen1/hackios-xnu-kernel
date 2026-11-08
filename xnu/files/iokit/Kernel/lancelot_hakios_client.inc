/* SPDX-License-Identifier: APSL-2.0
 * Imported from oosexgang/hakios-mt6768@4f27037146bd147ad15d36329e307a937603cc13.
 * The service name and lancelot task authorization are adapted in LancelotMali.cpp.
 */
/*
 * LancelotHakiosMali: the Infinix's Mali-G52 for programs to draw with.
 *
 * bsd/dev/lancelot_hakios_mali.c brings the GPU up (boot-arg mali=1) and, once a job has run through its
 * page tables, starts this service. A program that opens it speaks the interface of Linux's
 * panfrost DRM driver, the one Mesa's panfrost knows: every request is a call of the user client
 * with the request's number as the selector and its structure passed in and back -- as RedwoodGPU
 * does it for the other phone's Adreno and freedreno. What Linux does with mmap() on the device is
 * the client memory of the type that is the buffer's handle.
 *
 * What there is: parameters, buffers (made, entered into the GPU's address space, told their
 * address, mapped, given back), and job chains. A buffer is wired kernel memory, zero when made and
 * write-combined, so what the program writes goes to memory where the GPU reads it. A job chain is
 * run to its end before the call returns, so waiting for a buffer has nothing left to wait for, and
 * the synchronisation objects Mesa asks about can all be answered "signalled" in the program
 * (tools/mesa/redwood-drm). Not here yet: jobs that overlap, an address space for each program,
 * interrupts, buffers shared between programs.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void lancelot_hakios_keep(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/lancelot_hakios_mali.c */
bool lancelot_hakios_mali_ready(void);
bool lancelot_hakios_mali_parameter(uint32_t which, uint64_t * value);
uint64_t lancelot_hakios_mali_address(uint64_t size);
bool lancelot_hakios_mali_map(uint64_t gpu, uint64_t phys);
void lancelot_hakios_mali_mapped(void);
void lancelot_hakios_mali_unmap(uint64_t gpu, uint64_t size);
void lancelot_hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset);
uint32_t lancelot_hakios_mali_submit(uint64_t chain, bool fragment);
uint64_t lancelot_hakios_mali_enqueue(uint64_t chain, bool fragment);
uint64_t lancelot_hakios_mali_enqueue_complete(uint64_t chain,bool fragment,
    int (*completion)(void *,uint64_t,int,bool *),void *context);
bool lancelot_hakios_mali_wait(uint64_t seq, uint64_t timeout_ns);
int lancelot_hakios_mali_wait_result(uint64_t seq,uint64_t timeout_ns,bool retirement);
int lancelot_hakios_mali_profile(uint64_t sequence,uint64_t times[4],uint32_t *hz,int32_t *error);
bool lancelot_hakios_mali_retired(uint64_t seq,uint64_t timeout_ns);
uint64_t lancelot_hakios_mali_resets(void);


void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);

/* what programs have done with the GPU so far, for bsd/dev/lancelot_hakios_mali.c to repeat in the log */
uint32_t gHakiosMaliClients, gHakiosMaliBuffers, gHakiosMaliSubmits, gHakiosMaliFailed, gHakiosMaliShown;
/* the first buffers ever made, for matching a GPU fault's address to one of them */
struct mali_made { uint64_t address, size; uint32_t asked, flags; };
struct mali_made gHakiosMaliMade[32];
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
	kRedwoodSurfaceSeq      = 0x7c,         /* ours: a surface's last job, or "drawing, not yet sent" (struct redwood_surface_seq) */
	kRedwoodWait            = 0x7d,         /* ours: wait for a job by its number (struct redwood_wait) */
	kRedwoodSurface         = 0x7e,         /* ours, as RedwoodGPU's: an IOSurface's memory as a buffer */
	kRedwoodShow            = 0x7f,         /* ours, as RedwoodGPU's: a test's pixels onto the panel */
};

struct redwood_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kRedwoodDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
#define kRedwoodDescribe        0xffffffffu

/* kRedwoodWait: job `seq` (what kPanfrostSubmit gave back in `jc`) done within timeout_ns, or kIOReturnTimeout */
struct redwood_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

/* kRedwoodSurfaceSeq: the job after which the surface is drawn (MiniIOSurfaceDrawn), or
 * REDWOOD_SEQ_PENDING while the Metal driver's thread has its frame still to send */
struct redwood_surface_seq {
	uint32_t surface, pad;
	uint64_t seq;
};
#define REDWOOD_SEQ_PENDING     (~0ull)

struct redwood_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
#define PANFROST_JD_REQ_FS      (1u << 0)

struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};

#define MALI_BUFFERS            4096
#define MALI_BUFFER_MAX         (256ull << 20)

struct mali_buffer {
	IOMemoryDescriptor * memory;            /* made here, or the pixels of an IOSurface (`surface`) */
	MiniIOSurface * surface;
	uint64_t address, size;
	uint64_t last_seq;                      /* the last job given it (kPanfrostSubmit's bo_handles) */
	uint32_t preparing;                    /* CPU handoff may release the global client lock */
};

static IOLock * gHakiosMaliLock;

class HakiosPreparePins {
    mali_buffer **fBuffers=NULL;
    uint32_t *fTotal=NULL,fCount=0,fCapacity=0;
public:
    bool init(uint32_t *total,uint32_t capacity) {
        if(*total==UINT32_MAX)return false;
        fBuffers=IONew(mali_buffer *,capacity);if(!fBuffers)return false;
        fTotal=total;fCapacity=capacity;++*fTotal;return true;
    }
    bool add(mali_buffer *b) {
        for(uint32_t i=0;i<fCount;i++)if(fBuffers[i]==b)return true;
        if(fCount==fCapacity || b->preparing==UINT32_MAX)return false;
        ++b->preparing;fBuffers[fCount++]=b;return true;
    }
    ~HakiosPreparePins() {
        for(uint32_t i=0;i<fCount;i++)--fBuffers[i]->preparing;
        if(fTotal){--*fTotal;IOLockWakeup(gHakiosMaliLock,fTotal,false);}
        if(fBuffers)IODelete(fBuffers,mali_buffer *,fCapacity);
    }
};

#include "lancelot_hakios_surface_handoff.inc"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* The existing LancelotMali IOService publishes this transplanted user client. */

class LancelotHakiosMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(LancelotHakiosMaliUserClient);

	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fSubmits, fFailed, fRequests;
	uint32_t fCompleted,fCompletionErrors;
	task_t fTask;bool fClosing,fPinned;uint32_t fPreparing;

	struct mali_buffer * buffer(uint32_t handle);
	bool freeBuffer(struct mali_buffer * b);
	IOReturn submitQueued(struct drm_panfrost_submit *request);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle);
	IOReturn surfaceBuffer(struct redwood_surface * request);

public:
    /* FIFO callback: no client lock, including while GEM_CLOSE waits under it. */
    void recordCompletion(int result) {
        if(result)__atomic_add_fetch(&fCompletionErrors,1u,__ATOMIC_RELAXED);
        else __atomic_add_fetch(&fCompleted,1u,__ATOMIC_RELAXED);
    }
    virtual bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) APPLE_KEXT_OVERRIDE {
        if(type!=LMAL_OPEN_TYPE || !lmal_authorized_task(task) || !IOUserClient::initWithTask(task,security,type,properties))return false;
        fTask=task;return true;
    }
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LancelotHakiosMaliUserClient, IOUserClient);
#include "lancelot_hakios_submission.inc"

bool
LancelotHakiosMaliUserClient::start(IOService * provider)
{
	if (!lancelot_hakios_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("LancelotHakiosMali: a client could not be started\n");
		return false;
	}
	IOLog("LancelotHakiosMali: a client opened the GPU\n");
	OSIncrementAtomic((SInt32 *) &gHakiosMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

struct mali_buffer *
LancelotHakiosMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With the lock held. Wait for the last referenced FIFO job before releasing. */
bool
LancelotHakiosMaliUserClient::freeBuffer(struct mali_buffer *b)
{
    if(b->preparing)return false;
    if(!lm_gpu_ready() || (b->last_seq && !lancelot_hakios_mali_retired(b->last_seq,2ull*NSEC_PER_SEC))) {
        if(!fPinned){retain();fPinned=true;}return false;
    }
    lancelot_hakios_mali_unmap(b->address,b->size);
    if(!lm_gpu_ready()){if(!fPinned){retain();fPinned=true;}return false;}
    b->memory->release();if(b->surface)MiniIOSurfaceDrop(b->surface);
    bzero(b,sizeof(*b));return true;
}

/*
 * A handle and GPU addresses for `size` bytes of `memory` from byte `from` on, every page entered
 * into the GPU's tables. With the lock held; NULL: no handle, no addresses, or a page that would
 * not go in. The caller sets `memory` (and `surface`) of what comes back.
 */
struct mali_buffer *
LancelotHakiosMaliUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle)
{
	struct mali_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		IOLog("LancelotHakiosMali: no handles left\n");
		return NULL;
	}
	b->address = lancelot_hakios_mali_address(size);
	/* every 4 KB of it: the GPU's pages, a quarter of the kernel's 16 KB ones */
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && length>=4096 && !(phys&4095) && phys<(1ull<<40) && lancelot_hakios_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			lancelot_hakios_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		IOLog("LancelotHakiosMali: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	lancelot_hakios_mali_mapped();
	if (gHakiosMaliBuffers < 32) {
		gHakiosMaliMade[gHakiosMaliBuffers] = (struct mali_made) { b->address, size, (uint32_t) size, 0 };
	}
	OSIncrementAtomic((SInt32 *) &gHakiosMaliBuffers);
	b->size = size;
	return b;
}

/* With the lock held. */
IOReturn
LancelotHakiosMaliUserClient::createBO(struct drm_panfrost_create_bo * request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor * memory;
	struct mali_buffer * b;

	if (size == 0 || size > MALI_BUFFER_MAX || (request->flags & ~3u)) {
		return kIOReturnBadArgument;
	}
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("LancelotHakiosMali: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	b = newBuffer(memory, 0, size, &request->handle);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	request->size = (uint32_t)size; /* existing bridge reports actual mapped allocation size */
	request->offset = b->address;
	lancelot_hakios_mali_tag(b->address, 1, 0, 0, 0, 0, 0);
	return kIOReturnSuccess;
}

/*
 * The pixels of an IOSurface as a buffer, from the page after the surface's shared state on
 * (MiniIOSurface.h): what the window server draws into, and what is shown. The surface is held
 * for as long as the buffer is there; a surface that is a buffer of this client's already is that
 * buffer again. Asked only to describe (kRedwoodDescribe), no buffer is made. As RedwoodGPU's.
 */
IOReturn
LancelotHakiosMaliUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {                /* a program's own memory with a surface around it: not whole pages */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	if(s->maliCpuLease==NULL) {
        IOLog("LancelotHakiosMali: surface %u has no owned WB cache handoff\n",s->sid);
        MiniIOSurfaceDrop(s);return kIOReturnUnsupported;
    }
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	b = newBuffer(s->memory, PAGE_SIZE, size, &request->handle);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->surface = s;
	lancelot_hakios_mali_tag(b->address, 2, s->info.width, s->info.height, s->info.bytesPerRow, s->info.pixelFormat,
	    (uint32_t) s->info.offset);
	return kIOReturnSuccess;
}

IOReturn
LancelotHakiosMaliUserClient::clientClose(void)
{
	IOLockLock(gHakiosMaliLock);
	fClosing=true;
    uint64_t end=lmq_clock()+LMAL_MAX_WAIT_NS,absolute;nanoseconds_to_absolutetime(end,&absolute);
    while(fPreparing && lmq_clock()<end)IOLockSleepDeadline(gHakiosMaliLock,&fPreparing,absolute,THREAD_UNINT);
    if(fPreparing) {
        if(!fPinned){retain();fPinned=true;}
        IOLockUnlock(gHakiosMaliLock);return kIOReturnBusy;
    }
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gHakiosMaliLock);
	if (fSubmits != 0) {
		IOLog("LancelotHakiosMali: a client goes after %u job chains, %u of them unfinished\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
LancelotHakiosMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
LancelotHakiosMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gHakiosMaliLock);
	b = buffer(type);
	if (b != NULL && !b->preparing && !fClosing && current_task()==fTask) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gHakiosMaliLock);
	return ret;
}

IOReturn
LancelotHakiosMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct redwood_show show;
		struct redwood_surface surface;
		struct redwood_wait waitseq;
		struct redwood_surface_seq sseq;
	} r;
    if(!args || current_task()!=fTask || fClosing || args->scalarInputCount || args->scalarOutputCount || args->asyncReferenceCount || args->asyncWakePort || args->structureInputDescriptor || args->structureOutputDescriptor)return kIOReturnBadArgument;
    if(selector==LMAL_SELECTOR_JOB_PROFILE) {
        if(!args->structureInput || !args->structureOutput || args->structureInputSize!=sizeof(lmal_job_profile) || args->structureOutputSize!=sizeof(lmal_job_profile))return kIOReturnBadArgument;
        auto q=*(const lmal_job_profile*)args->structureInput;
        if(q.h.magic!=LMAL_UAPI_MAGIC || q.h.version!=LMAL_UAPI_VERSION || q.h.size!=sizeof(q) || q.h.reserved0 || q.h.reserved1)return kIOReturnBadArgument;
        uint64_t times[4]={0};uint32_t hz=0;int32_t error=0;
        q.h.result=lancelot_hakios_mali_profile(q.sequence,times,&hz,&error);
        q.start_ns=times[0];q.end_ns=times[1];q.jm_ns=times[2];q.worker_ns=times[3];q.hz=hz;q.error=error;
        *(lmal_job_profile*)args->structureOutput=q;return kIOReturnSuccess;
    }
    if(selector==LMAL_SELECTOR_GET_CAPS) {
        if(!args->structureInput || !args->structureOutput || args->structureInputSize!=sizeof(lmal_caps) || args->structureOutputSize!=sizeof(lmal_caps))return kIOReturnBadArgument;
        auto q=*(const lmal_caps*)args->structureInput;
        if(q.h.magic!=LMAL_UAPI_MAGIC || q.h.version!=LMAL_UAPI_VERSION || q.h.size!=sizeof(q))return kIOReturnBadArgument;
        IOLockLock(gHakiosMaliLock);
        q.h.result=0;q.jobs_submitted=fSubmits;q.jobs_completed=__atomic_load_n(&fCompleted,__ATOMIC_RELAXED);
        q.faults=fFailed+__atomic_load_n(&fCompletionErrors,__ATOMIC_RELAXED);q.resets=lancelot_hakios_mali_resets();q.quarantined=fPinned?1:0;
        q.max_bo_bytes=(uint32_t)MALI_BUFFER_MAX;q.max_vm_bytes=(uint32_t)LMAL_MAX_VM_BYTES;
        q.flags=LMAL_CAP_QUEUED_DRM | LMAL_CAP_HAKIOS_DONOR | LMAL_CAP_ASYNC_SUBMIT | LMAL_CAP_FENCE_SNAPSHOT | LMAL_CAP_JOB_PROFILE;
        IOLockUnlock(gHakiosMaliLock);
        *(lmal_caps*)args->structureOutput=q;return kIOReturnSuccess;
    }
    size_t expected=0;
    switch(selector){
    case kDrmGemClose:expected=sizeof(drm_gem_close);break;
    case kPanfrostSubmit:expected=sizeof(drm_panfrost_submit);break;
    case kPanfrostWaitBO:expected=sizeof(drm_panfrost_wait_bo);break;
    case kPanfrostCreateBO:expected=sizeof(drm_panfrost_create_bo);break;
    case kPanfrostMmapBO:expected=sizeof(drm_panfrost_mmap_bo);break;
    case kPanfrostGetParam:expected=sizeof(drm_panfrost_get_param);break;
    case kPanfrostGetBOOffset:expected=sizeof(drm_panfrost_get_bo_offset);break;
    case kPanfrostMadvise:expected=sizeof(drm_panfrost_madvise);break;
    case kRedwoodSurface:expected=sizeof(redwood_surface);break;
    case kRedwoodSurfaceSeq:expected=sizeof(redwood_surface_seq);break;
    case kRedwoodShow:expected=sizeof(redwood_show);break;
    case kRedwoodWait:case LMQ_ACK:expected=sizeof(redwood_wait);break;
    default:return kIOReturnUnsupported;
    }
    if(!args->structureInput || !args->structureOutput || args->structureInputSize!=expected || args->structureOutputSize!=expected)return kIOReturnBadArgument;
    if(selector==LMQ_ACK){memcpy(args->structureOutput,args->structureInput,expected);return kIOReturnSuccess;}
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!lancelot_hakios_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	/* the two waits, without holding everybody else up */
	if (selector == kRedwoodWait || selector == kPanfrostWaitBO) {
		uint64_t seq = r.waitseq.seq;
		int64_t timeout = r.waitseq.timeout_ns;

		if (selector == kPanfrostWaitBO) {
			IOLockLock(gHakiosMaliLock);
			b = buffer(r.wait.handle);
			seq = b != NULL ? b->last_seq : 0;
			IOLockUnlock(gHakiosMaliLock);
			timeout = r.wait.timeout_ns;
			if (b == NULL) {
				return kIOReturnNotFound;
			}
		}
		ret = lmq_return(seq==0?LM_OK:lancelot_hakios_mali_wait_result(seq,timeout>0?(uint64_t)timeout:0,false));
		if (args->structureOutput != NULL && args->structureOutputSize >= size) {
			memcpy(args->structureOutput, &r, size);
			args->structureOutputSize = (uint32_t) size;
		}
		return ret;
	}
	IOLockLock(gHakiosMaliLock);
	if (fRequests++ < 32) {                 /* the first ones a client makes, for following it from the log */
		IOLog("LancelotHakiosMali: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		if (!lancelot_hakios_mali_parameter(r.param.param, &r.param.value)) {
			ret = kIOReturnBadArgument;
		}
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.create);
		break;
	case kPanfrostMmapBO:                   /* the offset is the handle: the program maps client memory of that type */
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = (uint64_t) r.mmap.handle << 12;
		break;
	case kPanfrostGetBOOffset:
		b = buffer(r.offset.handle);
		ret = b != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.offset.offset = b != NULL ? b->address : 0;
		break;
	case kPanfrostSubmit:
        ret=submitQueued(&r.submit);
        break;
	case kRedwoodSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kRedwoodSurfaceSeq: {
		MiniIOSurface * surface = MiniIOSurfaceCopy(r.sseq.surface);

		if (surface != NULL) {
			/* drawing begins into a surface the display still holds (queued or being copied): what it
			 * shows would be part this frame, part the next -- counted, the first few by name */
			static uint32_t overDisplayed;

			if (r.sseq.seq == ~0ull && surface->useCount[0] != 0 && ++overDisplayed <= 8) {
				lancelot_hakios_keep("LancelotHakiosMali: surface %u drawn into while the display holds it (%u so far)",
				    r.sseq.surface, overDisplayed);
			} else if (r.sseq.seq == ~0ull && surface->useCount[0] != 0 && (overDisplayed & 255) == 0) {
				lancelot_hakios_keep("LancelotHakiosMali: surfaces drawn into while the display holds them: %u", overDisplayed);
			}
			surface->maliSeq = r.sseq.seq;
			wakeup((caddr_t) &surface->maliSeq);
			MiniIOSurfaceDrop(surface);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	}
	case kRedwoodShow: {
		struct redwood_show * show = (struct redwood_show *) &r;
		uint64_t bytes = (uint64_t) show->rowBytes * show->height;
		uint8_t * pixels;

		if (show->width == 0 || show->width > 4096 || show->height == 0 || show->height > 4096 ||
		    show->rowBytes < 4 * show->width || (pixels = (uint8_t *) IOMallocData(bytes)) == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (copyin((user_addr_t) show->pixels, pixels, bytes) != 0) {
			ret = kIOReturnVMError;
		} else {
			MiniIOMobileFramebufferShow(pixels, show->width, show->height, show->rowBytes);
			gHakiosMaliShown++;
		}
		IOFreeData(pixels, bytes);
		break;
	}
	case kPanfrostWaitBO:                   /* handled above, outside the global client lock */
		ret = buffer(r.wait.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		break;
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		b = buffer(r.close.handle);
		if (b != NULL) {
			if(!freeBuffer(b))ret=kIOReturnNotReady;
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	default:
		IOLog("LancelotHakiosMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gHakiosMaliLock);

	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}

/* Match the donor's enqueue-and-return contract. The FIFO completion owns the
 * handoff/fence; BO last_seq and explicit waits protect descriptors and scratch. */
IOReturn LancelotHakiosMaliUserClient::submitQueued(drm_panfrost_submit *q)
{
    if(!q->jc || (q->jc&63u) || (q->requirements&~1u) || q->in_sync_count || q->in_syncs || q->out_sync ||
       !q->bo_handles || !q->bo_handle_count || q->bo_handle_count>MALI_BUFFERS)return kIOReturnBadArgument;
    uint32_t *handles=IONew(uint32_t,q->bo_handle_count);
    if(!handles)return kIOReturnNoMemory;
    if(copyin((user_addr_t)q->bo_handles,handles,q->bo_handle_count*sizeof(*handles))) {
        IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnVMError;
    }
    bool jc=false;
    for(uint32_t i=0;i<q->bo_handle_count;i++) {
        auto*b=buffer(handles[i]);
        if(!b){IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnBadArgument;}
        if(q->jc>=b->address && q->jc-b->address<=b->size-64)jc=true;
    }
    if(!jc){IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnBadArgument;}
    HakiosPreparePins pins;
    if(!pins.init(&fPreparing,q->bo_handle_count)){IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnNoMemory;}
    for(uint32_t i=0;i<q->bo_handle_count;i++)if(!pins.add(buffer(handles[i]))) {
        IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnBusy;
    }
    auto *work=HakiosMaliSubmission::create(q->bo_handle_count,this);
    if(!work){IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnNoMemory;}
    HakiosSubmissionOwner owner(work);
    auto &handoff=work->handoff;
    for(uint32_t i=0;i<q->bo_handle_count;i++) {
        int e=handoff.add(buffer(handles[i])->surface);
        if(e){IODelete(handles,uint32_t,q->bo_handle_count);return lmq_return(e);}
    }
    auto*fence=work->fence;
    uint64_t end=lmq_clock()+LMAL_MAX_WAIT_NS,absolute;nanoseconds_to_absolutetime(end,&absolute);
    uint64_t preparedAt=lmq_clock();
    int prepared=LM_OK;uint32_t backoffs=0;
    for(;;) {
        if(fClosing || lmq_clock()>=end){prepared=fClosing?LM_ENODEV:LM_ETIMEDOUT;break;}
        prepared=handoff.acquire(end,false);
        if(prepared!=LM_EBUSY)break;
        prepared=handoff.resetPrepared();if(prepared)break;
        ++backoffs;
        IOLockUnlock(gHakiosMaliLock);IOSleep(1);IOLockLock(gHakiosMaliLock);
    }
    if(prepared){IODelete(handles,uint32_t,q->bo_handle_count);return lmq_return(prepared);}
    uint64_t preparationUs=(lmq_clock()-preparedAt)/1000;
    work->preparationUs=preparationUs;
    if(backoffs)IOLog("lancelot-hakios-contention: backoffs=%u wait_us=%llu pins=%u\n",backoffs,preparationUs,q->bo_handle_count);
    IOLockLock(lmq_surface_lock);
    for(;;) {
        bool busy=false;
        for(uint32_t i=0;i<q->bo_handle_count;i++){auto*s=buffer(handles[i])->surface;if(s&&s->maliQueueReaders)busy=true;}
        if(!busy)break;
        if(lmq_clock()>=end){IOLockUnlock(lmq_surface_lock);IODelete(handles,uint32_t,q->bo_handle_count);return kIOReturnTimeout;}
        IOLockSleepDeadline(lmq_surface_lock,&lmq_surface_changed,absolute,THREAD_UNINT);
    }
    for(uint32_t i=0;i<q->bo_handle_count;i++) {
        auto*s=buffer(handles[i])->surface;if(!s)continue;
        auto*old=s->maliQueueFence;fence->retain();s->maliQueueFence=fence;s->maliSeq=~0ull;OSSafeReleaseNULL(old);
    }
    IOLockUnlock(lmq_surface_lock);
    /* Publish ownership before a fast GPU completion can retire the cookie. */
    int published=handoff.publish();
    if(published){IODelete(handles,uint32_t,q->bo_handle_count);return lmq_return(published);}
    work->reserveQueueReference();
    uint64_t seq=lancelot_hakios_mali_enqueue_complete(q->jc,(q->requirements&1u)!=0,
        hakios_submission_complete,work);
    if(!seq) {
        work->rejectQueueReference();
        bool safe=true;(void)work->complete(0,LM_ENODEV,&safe);
    }
    for(uint32_t i=0;i<q->bo_handle_count;i++) {
        auto*b=buffer(handles[i]);if(seq)b->last_seq=seq;
        if(b->surface){__atomic_store_n(&b->surface->maliSeq,seq,__ATOMIC_RELEASE);wakeup((caddr_t)&b->surface->maliSeq);}
    }
    if(seq){fSubmits++;gHakiosMaliSubmits++;}else fFailed++;
    IODelete(handles,uint32_t,q->bo_handle_count);q->jc=seq;
    return seq?kIOReturnSuccess:kIOReturnNotReady;
}
static bool lmal_prepare_hakios_client()
{
    if(!gHakiosMaliLock)gHakiosMaliLock=IOLockAlloc();
    if(!lmq_surface_lock)lmq_surface_lock=IOLockAlloc();
    return gHakiosMaliLock&&lmq_surface_lock;
}
static IOUserClient *lmal_create_hakios_client(){return OSTypeAlloc(LancelotHakiosMaliUserClient);}
