/*
 * LancelotMali: the versioned, permission-gated IOKit client for Mali-G52.
 *
 * The client owns no physical address supplied by user space.  BOs are wired
 * IOBufferMemoryDescriptors (or page-aligned MiniIOSurface subranges); every
 * physical page is obtained from getPhysicalSegment and passed to the
 * per-client VM one page at a time.  GPU virtual addresses are allocated here
 * and never accepted from a caller.
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
#include "../../bsd/dev/lancelot_mali_core.h"
#include "lancelot_mali_uapi.h"
#include "lancelot_mali_client_logic.h"
#include "LancelotMaliExport.h"

extern "C" {
/* A wrapper-owned bootarg/platform state machine prepares the core first. */
void lancelot_mali_init(void);
void LancelotMaliPublish(void);
}

#define LMAL_MAX_BOS             ((uint32_t)LMAL_MAX_SUBMIT_BOS)
#define LMAL_MAX_SYNCS           ((uint32_t)LMAL_MAX_SYNCOBJS)
#define LMAL_MAX_ARRAY_BYTES     (LMAL_MAX_SUBMIT_BOS * sizeof(uint32_t))
#define LMAL_MAX_SYNC_ARRAY_BYTES (LMAL_MAX_SYNCS * sizeof(uint32_t))
#define LMAL_MAX_UAPI_INPUT      ((size_t)LMAL_MAX_STRUCT_SIZE)
#define LMAL_PAGE_MASK           (LMAL_PAGE_SIZE - 1)
#define LMAL_VALID_BO_FLAGS LMAL_CLIENT_BO_FLAGS
#define LMAL_VALID_SUBMIT_FLAGS LMAL_SUBMIT_REQUIRE_COMPLETE
#define LMAL_VALID_SYNC_WAIT_FLAGS (LMAL_SYNC_WAIT_ALL | LMAL_SYNC_WAIT_FOR_SUBMIT)

static uint32_t gLancelotMode;
static bool gLancelotModeRead;
static char gLancelotClientName[64];
static uint32_t gLancelotClientPid;
static bool gLancelotClientIdentity;

static void
lmal_read_bootargs(void)
{
	if (gLancelotModeRead) {
		return;
	}
	gLancelotModeRead = true;
	gLancelotMode = 0;
	(void)PE_parse_boot_argn("lancelot_mali", &gLancelotMode, sizeof(gLancelotMode));
	bzero(gLancelotClientName, sizeof(gLancelotClientName));
	gLancelotClientPid = 0;
	bool has_name = PE_parse_boot_argn("lancelot_mali_client", gLancelotClientName,
	    sizeof(gLancelotClientName));
	bool has_pid = PE_parse_boot_argn("lancelot_mali_client_pid", &gLancelotClientPid,
	    sizeof(gLancelotClientPid));
	/* A non-root client is admitted only when both exact identity components are configured. */
	gLancelotClientIdentity = has_name && has_pid && gLancelotClientName[0] != '\0' && gLancelotClientPid != 0;
}

static bool
lmal_render_enabled(void)
{
	lmal_read_bootargs();
	/* Modes 1..3 are platform bring-up/self-test only. */
	return gLancelotMode == 4;
}

static bool
lmal_authorized_task(task_t task)
{
	lmal_read_bootargs();
	if (!lmal_render_enabled() || task == TASK_NULL) {
		return false;
	}
	proc_t p = (proc_t)get_bsdtask_info(task);
	if (p == NULL) {
		return false;
	}
	kauth_cred_t cred = proc_ucred(p);
	if (cred != NULL && kauth_cred_getuid(cred) == 0) {
		return true;
	}
	/* The non-root path is an exact configured (pid, process-name) pair. */
	if (!gLancelotClientIdentity || (uint32_t)proc_pid(p) != gLancelotClientPid) {
		return false;
	}
	char name[sizeof(gLancelotClientName)];
	bzero(name, sizeof(name));
	proc_name(proc_pid(p), name, sizeof(name));
	return strncmp(name, gLancelotClientName, sizeof(name)) == 0;
}

static uint64_t
lmal_now_ns(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static int
lmal_result_from_ioreturn(IOReturn ret)
{
	if (ret == kIOReturnSuccess) {
		return LMAL_OK;
	}
	if (ret == kIOReturnNoMemory) {
		return LMAL_ENOMEM;
	}
	if (ret == kIOReturnTimeout) {
		return LMAL_ETIMEDOUT;
	}
	if (ret == kIOReturnNotReady || ret == kIOReturnOffline) {
		return LMAL_ENODEV;
	}
	if (ret == kIOReturnUnsupported) {
		return LMAL_ENOTSUP;
	}
	if (ret == kIOReturnBusy) {
		return LMAL_EBUSY;
	}
	if (ret == kIOReturnVMError) {
		return LMAL_EFAULT;
	}
	if (ret == kIOReturnNotPermitted) {
		return LMAL_EPERM;
	}
	return LMAL_EINVAL;
}

static IOReturn
lmal_ioreturn(int result)
{
	switch (result) {
	case LMAL_OK:       return kIOReturnSuccess;
	case LMAL_ENOMEM:   return kIOReturnNoMemory;
	case LMAL_EBUSY:    return kIOReturnBusy;
	case LMAL_ENODEV:   return kIOReturnNotReady;
	case LMAL_ENOTSUP:  return kIOReturnUnsupported;
	case LMAL_ETIMEDOUT:return kIOReturnTimeout;
	case LMAL_EFAULT:   return kIOReturnVMError;
	case LMAL_EPERM:    return kIOReturnNotPermitted;
	case LMAL_EIO:      return kIOReturnError;
	default:            return kIOReturnBadArgument;
	}
}

static bool
lmal_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	if (b > UINT64_MAX - a) {
		return true;
	}
	*out = a + b;
	return false;
}

static uint64_t
lmal_round_backing(uint64_t value)
{
	/* PAGE_SIZE is selected at boot: 4K or 16K, independently of Mali's 4K. */
	return lmal_client_round_backing(value, (uint64_t)PAGE_SIZE, LMAL_MAX_BO_BYTES);
}

static int
lmal_copy_u32_array(uint64_t user_address, uint32_t count, uint32_t maximum,
    uint32_t **array)
{
	*array = NULL;
	if (count == 0) {
		return LMAL_OK;
	}
	if (count > maximum || user_address == 0 || count > UINT32_MAX / sizeof(uint32_t)) {
		return LMAL_EINVAL;
	}
	uint64_t bytes64 = (uint64_t)count * sizeof(uint32_t);
	if (bytes64 > SIZE_MAX) {
		return LMAL_EINVAL;
	}
	uint32_t *copy = IONew(uint32_t, count);
	if (copy == NULL) {
		return LMAL_ENOMEM;
	}
	if (copyin((user_addr_t)user_address, copy, (size_t)bytes64) != 0) {
		IODelete(copy, uint32_t, count);
		return LMAL_EFAULT;
	}
	*array = copy;
	return LMAL_OK;
}

static void
lmal_free_u32_array(uint32_t *array, uint32_t count)
{
	if (array != NULL) {
		IODelete(array, uint32_t, count);
	}
}

static bool
lmal_surface_metadata(MiniIOSurface *surface, uint64_t *size)
{
	if (surface == NULL || surface->memory == NULL || surface->client != NULL ||
	    surface->info.allocSize == 0 || surface->kbase == NULL ||
	    surface->kpixels != surface->kbase + PAGE_SIZE) {
		return false;
	}
	const struct minisurf_result &info = surface->info;
	const struct minisurf_plane &plane = surface->planes[0];
	uint32_t extended = plane.extendedPixels[0] | plane.extendedPixels[1] |
	    plane.extendedPixels[2] | plane.extendedPixels[3];
	if (lmal_client_surface_layout(info.width, info.height, info.bytesPerRow,
	    info.offset, info.pixelFormat, info.planeCount, info.bytesPerElement,
	    info.elementWidth, info.elementHeight, info.allocSize, plane.compression,
	    extended, info.protectionOptions != 0, info.parentID) != LMAL_OK ||
	    surface->shared()->tiled || plane.width != info.width || plane.height != info.height ||
	    plane.bytesPerRow != info.bytesPerRow || plane.offset || plane.bytesPerElement != 4 ||
	    plane.elementWidth != 1 || plane.elementHeight != 1 ||
	    plane.size < (uint64_t)info.bytesPerRow * info.height || plane.size > info.allocSize) return false;
	uint64_t rounded = lmal_client_surface_backing(surface->info.allocSize,
	    (uint64_t)PAGE_SIZE, surface->memory->getLength(), LMAL_MAX_BO_BYTES);
	if (rounded == 0) {
		return false;
	}
	*size = rounded;
	return true;
}

class LancelotMaliUserClient;

class LancelotMali : public IOService
{
	OSDeclareDefaultStructors(LancelotMali);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LancelotMali, IOService);

struct lmal_bo {
	IOMemoryDescriptor *memory;
	LancelotMaliExport *export_memory;
	MiniIOSurface *surface;
	uint64_t gpu_va;
	uint64_t size;
	uint64_t last_sequence;
	uint32_t flags;
	uint32_t surface_id;
	uint32_t last_status;
	uint32_t in_flight;
	bool used;
	bool prepared;
	bool mapped;
	bool quarantined;
};

class LancelotMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(LancelotMaliUserClient);

	task_t fTask;
	IOLock *fLock;
	struct lm_vm *fVM;
	struct lmal_bo *fBOs;
	struct lmal_sync *fSyncs;
	uint64_t fNextVA;
	uint64_t fJobsSubmitted;
	uint64_t fJobsCompleted;
	uint64_t fFaults;
	uint64_t fResets;
	uint64_t fQuarantined;
	bool fAuthorized;
	bool fClosing;
	IOReturn fCloseResult;
	bool fVMQuarantined;

	struct lmal_bo *bo(uint32_t handle);
	struct lmal_sync *sync(uint32_t handle);
	bool bo_overlaps(uint64_t va, uint64_t size) const;
	int alloc_va(uint64_t size, uint64_t *va);
	int collect_pages(IOMemoryDescriptor *memory, uint64_t size, struct lm_page **pages);
	int create_memory(uint64_t size, IOBufferMemoryDescriptor **memory);
	void quarantine_bo(struct lmal_bo *b);
	void discard_unmapped_bo(struct lmal_bo *b, bool map_attempted);
	bool release_bo(struct lmal_bo *b);
	int bo_create(const struct lmal_bo_create *request, struct lmal_bo_create *response);
	int bo_query(const struct lmal_bo_query *request, struct lmal_bo_query *response);
	int bo_close(const struct lmal_bo_close *request);
	int bo_unmap(const struct lmal_bo_close *request);
	int bo_wait(const struct lmal_bo_wait *request);
	int surface_query(const struct lmal_surface_query *request, struct lmal_surface_query *response);
	int surface_import(const struct lmal_surface_import *request, struct lmal_surface_import *response);
	int submit(const struct lmal_submit *request, struct lmal_submit *response);
	int sync_create(const struct lmal_syncobj_create *request, struct lmal_syncobj_create *response);
	int sync_destroy(const struct lmal_syncobj_handle *request);
	int sync_set_array(const struct lmal_syncobj_array *request, bool signaled);
	int sync_wait(const struct lmal_syncobj_wait *request, struct lmal_syncobj_wait *response);

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LancelotMaliUserClient, IOUserClient);

bool
LancelotMali::start(IOService *provider)
{
	if (!lmal_render_enabled() || !lm_gpu_ready() || provider == NULL ||
	    !IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "LancelotMaliUserClient");
	setName(LMAL_SERVICE_NAME);
	registerService();
	return true;
}

extern "C" void
LancelotMaliPublish(void)
{
	if (!lmal_render_enabled() || !lm_gpu_ready()) {
		return;
	}
	IOService *provider = IOService::getPlatform();
	if (provider == NULL) {
		return;
	}
	LancelotMali *service = OSTypeAlloc(LancelotMali);
	if (service == NULL || !service->init() || !service->attach(provider) ||
	    !service->start(provider)) {
		if (service != NULL) {
			if (service->isInactive() == false) {
				service->terminate();
			}
			service->release();
		}
		IOLog("LancelotMali: cannot publish service in render mode\n");
		return;
	}
	service->release();
}

bool
LancelotMaliUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = TASK_NULL;
	fLock = NULL;
	fVM = NULL;
	fBOs = NULL;
	fSyncs = NULL;
	fNextVA = LM_VA_MIN;
	fJobsSubmitted = 0;
	fJobsCompleted = 0;
	fFaults = 0;
	fResets = 0;
	fQuarantined = 0;
	fAuthorized = false;
	fClosing = false;
	fCloseResult = kIOReturnSuccess;
	fVMQuarantined = false;
	if (type != LMAL_OPEN_TYPE || !lmal_authorized_task(owningTask)) {
		return false;
	}
	fTask = owningTask;
	fAuthorized = true;
	return true;
}

bool
LancelotMaliUserClient::start(IOService *provider)
{
	if (!fAuthorized || provider == NULL || !lm_gpu_ready() ||
	    !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fBOs = IONewZero(struct lmal_bo, LMAL_MAX_BOS);
	fSyncs = IONewZero(struct lmal_sync, LMAL_MAX_SYNCS);
	fNextVA = LM_VA_MIN;
	if (fLock == NULL || fBOs == NULL || fSyncs == NULL) {
		if (fSyncs != NULL) {
			IODelete(fSyncs, struct lmal_sync, LMAL_MAX_SYNCS);
			fSyncs = NULL;
		}
		if (fBOs != NULL) {
			IODelete(fBOs, struct lmal_bo, LMAL_MAX_BOS);
			fBOs = NULL;
		}
		if (fLock != NULL) {
			IOLockFree(fLock);
			fLock = NULL;
		}
		return false;
	}
	fVM = lm_gpu_vm_new();
	if (fVM == NULL) {
		IODelete(fSyncs, struct lmal_sync, LMAL_MAX_SYNCS);
		fSyncs = NULL;
		IODelete(fBOs, struct lmal_bo, LMAL_MAX_BOS);
		fBOs = NULL;
		IOLockFree(fLock);
		fLock = NULL;
		return false;
	}
	return true;
}

struct lmal_bo *
LancelotMaliUserClient::bo(uint32_t handle)
{
	if (handle == 0 || handle > LMAL_MAX_BOS || fBOs == NULL) {
		return NULL;
	}
	struct lmal_bo *b = &fBOs[handle - 1];
	return b->used && !b->quarantined ? b : NULL;
}

struct lmal_sync *
LancelotMaliUserClient::sync(uint32_t handle)
{
	if (handle == 0 || handle > LMAL_MAX_SYNCS || fSyncs == NULL) {
		return NULL;
	}
	struct lmal_sync *s = &fSyncs[handle - 1];
	return s->used ? s : NULL;
}

bool
LancelotMaliUserClient::bo_overlaps(uint64_t va, uint64_t size) const
{
	uint64_t end = 0;
	if (size == 0 || lmal_add_overflow_u64(va, size, &end)) {
		return true;
	}
	for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
		const struct lmal_bo *b = &fBOs[i];
		if (!b->used || !b->mapped || b->gpu_va == 0) {
			continue;
		}
		uint64_t b_end = b->gpu_va + b->size;
		if (va < b_end && b->gpu_va < end) {
			return true;
		}
	}
	return false;
}

int
LancelotMaliUserClient::alloc_va(uint64_t size, uint64_t *va)
{
	if (size == 0 || (size & LMAL_PAGE_MASK) != 0 || size > LMAL_MAX_BO_BYTES ||
	    size > LM_VA_END - LM_VA_MIN) {
		return LMAL_EINVAL;
	}
	uint64_t starts[2] = { fNextVA, LM_VA_MIN };
	for (unsigned pass = 0; pass < 2; pass++) {
		uint64_t candidate = starts[pass];
		if (candidate < LM_VA_MIN || candidate >= LM_VA_END) {
			candidate = LM_VA_MIN;
		}
		candidate = (candidate + LMAL_PAGE_MASK) & ~LMAL_PAGE_MASK;
		uint32_t scans = 0;
		while (candidate < LM_VA_END && scans++ < 262144) {
			uint64_t end = 0;
			if (lmal_add_overflow_u64(candidate, size, &end) || end > LM_VA_END) {
				break;
			}
			bool conflict = false;
			uint64_t bump = candidate + LMAL_PAGE_SIZE;
			for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
				const struct lmal_bo *b = &fBOs[i];
				if (!b->used || !b->mapped || b->gpu_va == 0) {
					continue;
				}
				uint64_t b_end = b->gpu_va + b->size;
				if (candidate < b_end && b->gpu_va < end) {
					conflict = true;
					if (b_end > bump) {
						bump = b_end;
					}
				}
			}
			if (!conflict) {
				*va = candidate;
				fNextVA = end < LM_VA_END ? end : LM_VA_MIN;
				return LMAL_OK;
			}
			candidate = (bump + LMAL_PAGE_MASK) & ~LMAL_PAGE_MASK;
		}
	}
	return LMAL_ENOMEM;
}

int
LancelotMaliUserClient::create_memory(uint64_t size, IOBufferMemoryDescriptor **memory)
{
	if (size > UINT32_MAX || lmal_round_backing(size) != size || size == 0) {
		return LMAL_EINVAL;
	}
	IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithOptions(
	    kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
	    (vm_size_t)size, PAGE_SIZE);
	if (m == NULL) {
		return LMAL_ENOMEM;
	}
	if (m->getLength() != size || m->getCapacity() != size) {
		/* IOBuffer rounds its allocation locally but preserves requested length. */
		m->release();
		return LMAL_EFAULT;
	}
	IOReturn ret = m->prepare();
	if (ret != kIOReturnSuccess) {
		m->release();
		return lmal_result_from_ioreturn(ret);
	}
	*memory = m;
	return LMAL_OK;
}

int
LancelotMaliUserClient::collect_pages(IOMemoryDescriptor *memory, uint64_t size,
    struct lm_page **pages)
{
	uint32_t count = lmal_client_gpu_page_count(size, LM_MAX_MAP_PAGES);
	if (memory == NULL || count == 0 || memory->getLength() < size) {
		return LMAL_EINVAL;
	}
	struct lm_page *p = IONew(struct lm_page, count);
	if (p == NULL) {
		return LMAL_ENOMEM;
	}
	bzero(p, sizeof(*p) * count);
	uint8_t *cpu = NULL;
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		cpu = (uint8_t *)buffer->getBytesNoCopy();
	} else {
		/* IOSubMemoryDescriptor has no getBytesNoCopy; derive its known pixel base below. */
		for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
			if (fBOs[i].used && fBOs[i].memory == memory && fBOs[i].surface != NULL) {
				cpu = fBOs[i].surface->kbase + PAGE_SIZE;
				break;
			}
		}
	}
	if (cpu == NULL) {
		IODelete(p, struct lm_page, count);
		return LMAL_EFAULT;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint64_t offset = lmal_client_gpu_page_offset(i);
		IOByteCount segment = 0;
		addr64_t pa = memory->getPhysicalSegment((IOByteCount)offset,
		    &segment, kIOMemoryMapperNone);
		if (!lmal_client_gpu_segment_valid((uint64_t)pa, (uint64_t)segment)) {
			IODelete(p, struct lm_page, count);
			return LMAL_EFAULT;
		}
		p[i].cpu = cpu + (size_t)offset;
		p[i].pa = (uint64_t)pa;
	}
	*pages = p;
	return LMAL_OK;
}

void
LancelotMaliUserClient::quarantine_bo(struct lmal_bo *b)
{
	if (b == NULL || !b->used || b->quarantined) {
		return;
	}
	b->quarantined = true;
	fQuarantined++;
}

static int
lmal_complete_new_memory(void *memory)
{
	return ((IOMemoryDescriptor *)memory)->complete() == kIOReturnSuccess ? LMAL_OK : LMAL_EIO;
}

static void
lmal_drop_new_memory(void *memory)
{
	((IOMemoryDescriptor *)memory)->release();
}

static void
lmal_drop_new_surface(void *surface)
{
	MiniIOSurfaceDrop((MiniIOSurface *)surface);
}

void
LancelotMaliUserClient::discard_unmapped_bo(struct lmal_bo *b, bool map_attempted)
{
	/* Core mapping modifies software tables only; failed maps roll back before
	 * returning. An unattempted page collection cannot give DMA this backing. */
	bool safe = !map_attempted || (fVM != NULL && !fVMQuarantined && lm_gpu_vm_safe_to_free(fVM));
	int cleanup = lmal_client_cleanup_new_backing(b->memory, b->surface, safe,
	    lmal_complete_new_memory, lmal_drop_new_memory, lmal_drop_new_surface);
	if (cleanup == LMAL_OK) {
		bzero(b, sizeof(*b));
	} else {
		/* Prevent a failed client from repeatedly allocating quarantined backing. */
		fVMQuarantined = true;
		quarantine_bo(b);
	}
}

bool
LancelotMaliUserClient::release_bo(struct lmal_bo *b)
{
	if (b == NULL || !b->used) {
		return true;
	}
	/* Even quarantined GPU backing must stop new CPU exports at close. */
	if (b->export_memory != NULL && b->export_memory->revoke(true) != LMAL_OK) {
		quarantine_bo(b);
		return false;
	}
	if (b->quarantined) {
		/* A timeout/reset ambiguity owns this backing until a later proof of safety. */
		return false;
	}
	if (b->in_flight != 0 || fVM == NULL || fVMQuarantined) {
		quarantine_bo(b);
		return false;
	}
	if (b->mapped) {
		int ret = lm_gpu_vm_unmap(fVM, b->gpu_va, (uint32_t)(b->size / LMAL_PAGE_SIZE));
		if (ret != LMAL_OK || !lm_gpu_vm_safe_to_free(fVM)) {
			quarantine_bo(b);
			return false;
		}
		b->mapped = false;
	}
	if (b->prepared && b->memory != NULL) {
		if (b->memory->complete() != kIOReturnSuccess) {
			quarantine_bo(b);
			return false;
		}
		b->prepared = false;
	}
	if (b->export_memory != NULL) {
		b->export_memory->release();
		b->export_memory = NULL;
	}
	if (b->memory != NULL) {
		b->memory->release();
	}
	if (b->surface != NULL) {
		MiniIOSurfaceDrop(b->surface);
	}
	bzero(b, sizeof(*b));
	return true;
}

int
LancelotMaliUserClient::bo_create(const struct lmal_bo_create *request,
    struct lmal_bo_create *response)
{
	int admission = lmal_client_allocation_admit(lm_gpu_ready(), fVMQuarantined, fVM != NULL);
	if (admission != LMAL_OK) return admission;
	if (request->h.flags != 0 || request->size == 0 || request->size > LMAL_MAX_BO_BYTES ||
	    (request->flags & ~LMAL_VALID_BO_FLAGS) != 0 ||
	    ((request->flags & LMAL_BO_EXEC) && (request->flags & LMAL_BO_NOEXEC)) ||
	    ((request->flags & LMAL_BO_READONLY) && (request->flags & LMAL_BO_WRITE))) {
		return LMAL_EINVAL;
	}
	uint64_t size = lmal_round_backing(request->size);
	if (size == 0 || size > LMAL_MAX_BO_BYTES) {
		return LMAL_EINVAL;
	}
	if (request->flags & LMAL_BO_HEAP) {
		/* v1 uses fixed backing; callers must size the heap explicitly. */
		if (size > LMAL_MAX_BO_BYTES) {
			return LMAL_EINVAL;
		}
	}
	struct lmal_bo *b = NULL;
	uint32_t index = 0;
	for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
		if (!fBOs[i].used) {
			b = &fBOs[i];
			index = i;
			break;
		}
	}
	if (b == NULL) {
		return LMAL_ENOMEM;
	}
	uint64_t va = 0;
	int ret = alloc_va(size, &va);
	if (ret != LMAL_OK) {
		return ret;
	}
	IOBufferMemoryDescriptor *memory = NULL;
	ret = create_memory(size, &memory);
	if (ret != LMAL_OK) {
		return ret;
	}
	bzero(b, sizeof(*b));
	b->used = true;
	b->memory = memory;
	b->size = size;
	b->gpu_va = va;
	b->flags = request->flags | LMAL_BO_FIXED_BACKING;
	if ((b->flags & (LMAL_BO_EXEC | LMAL_BO_NOEXEC)) == 0) {
		b->flags |= LMAL_BO_NOEXEC;
	}
	b->prepared = true;
	struct lm_page *pages = NULL;
	ret = collect_pages(memory, size, &pages);
	if (ret != LMAL_OK) {
		discard_unmapped_bo(b, false);
		return ret;
	}
	uint32_t map_flags = lmal_client_gpu_permissions(request->flags, LM_MAP_WRITE, LM_MAP_EXEC);
	ret = lm_gpu_vm_map(fVM, va, pages, (uint32_t)(size / LMAL_PAGE_SIZE), map_flags);
	IODelete(pages, struct lm_page, (uint32_t)(size / LMAL_PAGE_SIZE));
	if (ret != LMAL_OK) {
		discard_unmapped_bo(b, true);
		return ret;
	}
	b->mapped = true;
	response->handle = index + 1;
	response->gpu_va = va;
	response->actual_size = size;
	response->map_type = index + 1;
	response->flags = b->flags;
	return LMAL_OK;
}

int
LancelotMaliUserClient::bo_query(const struct lmal_bo_query *request,
    struct lmal_bo_query *response)
{
	if (request->h.flags != 0) {
		return LMAL_EINVAL;
	}
	struct lmal_bo *b = bo(request->handle);
	if (b == NULL) {
		return LMAL_EINVAL;
	}
	response->size = b->size;
	response->gpu_va = b->gpu_va;
	response->flags = b->flags;
	response->map_type = request->handle;
	response->map_refs = b->export_memory != NULL ? b->export_memory->mapCount() : 0;
	return LMAL_OK;
}

int
LancelotMaliUserClient::bo_close(const struct lmal_bo_close *request)
{
	if (request->h.flags != 0) {
		return LMAL_EINVAL;
	}
	struct lmal_bo *b = bo(request->handle);
	if (b == NULL) {
		return LMAL_EINVAL;
	}
	return release_bo(b) ? LMAL_OK : LMAL_EBUSY;
}

int
LancelotMaliUserClient::bo_unmap(const struct lmal_bo_close *request)
{
	if (request->h.flags != 0) {
		return LMAL_EINVAL;
	}
	struct lmal_bo *b = bo(request->handle);
	if (b == NULL) {
		return LMAL_EINVAL;
	}
	/* Transport order: IOConnectUnmapMemory64 first, then this revoke-all ACK.
	 * Repeated ACK is idempotent, but never claims success after unmap failure. */
	if (b->export_memory != NULL && b->export_memory->revoke(false) != LMAL_OK) {
		quarantine_bo(b);
		return LMAL_EIO;
	}
	return LMAL_OK;
}

int
LancelotMaliUserClient::bo_wait(const struct lmal_bo_wait *request)
{
	if (request->h.flags != 0 || (request->flags & ~(LMAL_BO_WAIT_READ | LMAL_BO_WAIT_WRITE)) != 0) {
		return LMAL_EINVAL;
	}
	struct lmal_bo *b = bo(request->handle);
	if (b == NULL) {
		return LMAL_EINVAL;
	}
	if (b->last_status != 0) {
		return (int)b->last_status;
	}
	/* Submit is synchronous in v1; a completed sequence is immediately waitable. */
	return LMAL_OK;
}

int
LancelotMaliUserClient::surface_query(const struct lmal_surface_query *request,
    struct lmal_surface_query *response)
{
	if (request->h.flags != 0 || request->surface_id == 0) {
		return LMAL_EINVAL;
	}
	MiniIOSurface *surface = MiniIOSurfaceCopy(request->surface_id);
	if (surface == NULL) {
		return LMAL_EINVAL;
	}
	uint64_t size = 0;
	if (!lmal_surface_metadata(surface, &size)) {
		MiniIOSurfaceDrop(surface);
		return LMAL_ENOTSUP;
	}
	response->size = size;
	response->offset = surface->info.offset;
	response->row_bytes = surface->info.bytesPerRow;
	response->width = surface->info.width;
	response->height = surface->info.height;
	response->pixel_format = surface->info.pixelFormat;
	response->planes = surface->info.planeCount;
	MiniIOSurfaceDrop(surface);
	return LMAL_OK;
}

int
LancelotMaliUserClient::surface_import(const struct lmal_surface_import *request,
    struct lmal_surface_import *response)
{
	int admission = lmal_client_allocation_admit(lm_gpu_ready(), fVMQuarantined, fVM != NULL);
	if (admission != LMAL_OK) return admission;
	if (request->h.flags != 0 || request->surface_id == 0 ||
	    (request->flags & ~LMAL_VALID_BO_FLAGS) != 0 || (request->flags & LMAL_BO_EXEC)) {
		return LMAL_EINVAL;
	}
	MiniIOSurface *surface = MiniIOSurfaceCopy(request->surface_id);
	if (surface == NULL) {
		return LMAL_EINVAL;
	}
	uint64_t size = 0;
	if (!lmal_surface_metadata(surface, &size)) {
		MiniIOSurfaceDrop(surface);
		return LMAL_ENOTSUP;
	}
	for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
		struct lmal_bo *old = &fBOs[i];
		if (old->used && !old->quarantined && old->surface_id == request->surface_id &&
		    old->surface != NULL) {
			if (old->flags != (request->flags | LMAL_BO_NOEXEC | LMAL_BO_FIXED_BACKING)) {
				MiniIOSurfaceDrop(surface);
				return LMAL_EINVAL;
			}
			response->handle = i + 1;
			response->gpu_va = old->gpu_va;
			response->size = old->size;
			response->map_type = i + 1;
			response->offset = surface->info.offset;
			response->row_bytes = surface->info.bytesPerRow;
			response->width = surface->info.width;
			response->height = surface->info.height;
			response->pixel_format = surface->info.pixelFormat;
			response->planes = surface->info.planeCount;
			MiniIOSurfaceDrop(surface);
			return LMAL_OK;
		}
	}
	struct lmal_bo *b = NULL;
	uint32_t index = 0;
	for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
		if (!fBOs[i].used) {
			b = &fBOs[i];
			index = i;
			break;
		}
	}
	if (b == NULL) {
		MiniIOSurfaceDrop(surface);
		return LMAL_ENOMEM;
	}
	uint64_t va = 0;
	int ret = alloc_va(size, &va);
	if (ret != LMAL_OK) {
		MiniIOSurfaceDrop(surface);
		return ret;
	}
	IOSubMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(
	    surface->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (sub == NULL) {
		MiniIOSurfaceDrop(surface);
		return LMAL_ENOMEM;
	}
	ret = sub->prepare();
	if (ret != kIOReturnSuccess) {
		sub->release();
		MiniIOSurfaceDrop(surface);
		return lmal_result_from_ioreturn(ret);
	}
	/* collect_pages derives the subrange CPU base from this retained surface. */
	bzero(b, sizeof(*b));
	b->used = true;
	b->memory = sub;
	b->surface = surface;
	b->surface_id = request->surface_id;
	b->gpu_va = va;
	b->size = size;
	b->flags = request->flags | LMAL_BO_NOEXEC | LMAL_BO_FIXED_BACKING;
	b->prepared = true;
	struct lm_page *pages = NULL;
	ret = collect_pages(sub, size, &pages);
	bool map_attempted = false;
	if (ret == LMAL_OK) {
		map_attempted = true;
		ret = lm_gpu_vm_map(fVM, va, pages, (uint32_t)(size / LMAL_PAGE_SIZE),
		    lmal_client_gpu_permissions(request->flags, LM_MAP_WRITE, LM_MAP_EXEC));
	}
	if (pages != NULL) {
		IODelete(pages, struct lm_page, (uint32_t)(size / LMAL_PAGE_SIZE));
	}
	if (ret != LMAL_OK) {
		discard_unmapped_bo(b, map_attempted);
		return ret;
	}
	b->mapped = true;
	response->handle = index + 1;
	response->gpu_va = va;
	response->size = size;
	response->map_type = index + 1;
	response->offset = surface->info.offset;
	response->row_bytes = surface->info.bytesPerRow;
	response->width = surface->info.width;
	response->height = surface->info.height;
	response->pixel_format = surface->info.pixelFormat;
	response->planes = surface->info.planeCount;
	return LMAL_OK;
}

int
LancelotMaliUserClient::sync_create(const struct lmal_syncobj_create *request,
    struct lmal_syncobj_create *response)
{
	if (request->h.flags != 0 || (request->flags & ~LMAL_SYNCOBJ_CREATE_SIGNALED) != 0) {
		return LMAL_EINVAL;
	}
	for (uint32_t i = 0; i < LMAL_MAX_SYNCS; i++) {
		if (!fSyncs[i].used && lmal_client_sync_create(&fSyncs[i],
		    (request->flags & LMAL_SYNCOBJ_CREATE_SIGNALED) != 0) == LMAL_OK) {
			response->handle = i + 1;
			return LMAL_OK;
		}
	}
	return LMAL_ENOMEM;
}

int
LancelotMaliUserClient::sync_destroy(const struct lmal_syncobj_handle *request)
{
	if (request->h.flags != 0) {
		return LMAL_EINVAL;
	}
	struct lmal_sync *s = sync(request->handle);
	return s == NULL ? LMAL_EINVAL : lmal_client_sync_destroy(s);
}

int
LancelotMaliUserClient::sync_set_array(const struct lmal_syncobj_array *request,
    bool signaled)
{
	if (request->h.flags != 0 || request->count == 0 || request->count > LMAL_MAX_SYNCS) {
		return LMAL_EINVAL;
	}
	uint32_t *handles = NULL;
	int ret = lmal_copy_u32_array(request->handles, request->count, LMAL_MAX_SYNCS, &handles);
	if (ret != LMAL_OK) {
		return ret;
	}
	for (uint32_t i = 0; i < request->count; i++) {
		struct lmal_sync *s = sync(handles[i]);
		if (s == NULL || s->in_flight != 0) {
			lmal_free_u32_array(handles, request->count);
			return LMAL_EINVAL;
		}
	}
	for (uint32_t i = 0; i < request->count; i++) {
		fSyncs[handles[i] - 1].signaled = signaled;
	}
	lmal_free_u32_array(handles, request->count);
	return LMAL_OK;
}

int
LancelotMaliUserClient::sync_wait(const struct lmal_syncobj_wait *request,
    struct lmal_syncobj_wait *response)
{
	if (request->h.flags != 0 || (request->flags & ~LMAL_VALID_SYNC_WAIT_FLAGS) != 0 ||
	    request->count == 0 || request->count > LMAL_MAX_SYNCS) {
		return LMAL_EINVAL;
	}
	uint32_t *handles = NULL;
	int ret = lmal_copy_u32_array(request->handles, request->count, LMAL_MAX_SYNCS, &handles);
	if (ret != LMAL_OK) {
		return ret;
	}
	uint64_t *generations = IONew(uint64_t, request->count);
	if (generations == NULL) {
		lmal_free_u32_array(handles, request->count);
		return LMAL_ENOMEM;
	}
	ret = lmal_client_wait_pin(fSyncs, LMAL_MAX_SYNCS, handles, generations, request->count);
	if (ret != LMAL_OK) {
		IODelete(generations, uint64_t, request->count);
		lmal_free_u32_array(handles, request->count);
		return ret;
	}
	uint64_t now = lmal_now_ns();
	uint64_t deadline = request->absolute_monotonic_ns;
	if (deadline <= now) {
		deadline = now;
	} else if (deadline - now > LMAL_MAX_WAIT_NS) {
		deadline = now + LMAL_MAX_WAIT_NS;
	}
	bool wait_all = (request->flags & LMAL_SYNC_WAIT_ALL) != 0;
	response->first_signaled = UINT32_MAX;
	for (;;) {
		uint32_t first = UINT32_MAX;
		ret = lmal_client_wait_poll(fSyncs, handles, generations, request->count,
		    wait_all, fClosing, &first);
		if (ret != LMAL_EBUSY) {
			if (ret == LMAL_OK) response->first_signaled = first;
			break;
		}
		if (lmal_now_ns() >= deadline) {
			ret = LMAL_ETIMEDOUT;
			break;
		}
		/* SIGNAL/RESET remain usable; DESTROY returns EBUSY until pins drop.
		 * externalMethod retains this client, including its lock and slot array. */
		IOLockUnlock(fLock);
		IOSleep(1);
		IOLockLock(fLock);
	}
	lmal_client_wait_unpin(fSyncs, handles, generations, request->count);
	IODelete(generations, uint64_t, request->count);
	lmal_free_u32_array(handles, request->count);
	return ret;
}

int
LancelotMaliUserClient::submit(const struct lmal_submit *request,
    struct lmal_submit *response)
{
	uint32_t *handles = NULL;
	uint32_t *sync_handles = NULL;
	bool jc_valid = false;
	uint64_t jc_end = 0;
	int ret = LMAL_OK;
	struct lmal_bo **refs = NULL;
	uint32_t ref_count = 0;
	if (request->h.flags != 0 || request->bo_count == 0 ||
	    request->bo_count > LMAL_MAX_SUBMIT_BOS || request->in_sync_count > LMAL_MAX_SUBMIT_SYNCS ||
	    (request->requirements & ~UINT32_C(1)) != 0 || request->timeout_ns > LMAL_MAX_WAIT_NS ||
	    request->jc == 0 || (request->jc & 7) != 0) {
		return LMAL_EINVAL;
	}
	ret = lmal_copy_u32_array(request->bo_handles, request->bo_count,
	    LMAL_MAX_SUBMIT_BOS, &handles);
	if (ret != LMAL_OK) {
		return ret;
	}
	ret = lmal_copy_u32_array(request->in_syncs, request->in_sync_count,
	    LMAL_MAX_SUBMIT_SYNCS, &sync_handles);
	if (ret != LMAL_OK) {
		lmal_free_u32_array(handles, request->bo_count);
		return ret;
	}
	refs = IONew(struct lmal_bo *, request->bo_count);
	if (refs == NULL) {
		lmal_free_u32_array(handles, request->bo_count);
		lmal_free_u32_array(sync_handles, request->in_sync_count);
		return LMAL_ENOMEM;
	}
	for (uint32_t i = 0; i < request->bo_count; i++) {
		struct lmal_bo *b = bo(handles[i]);
		if (b == NULL) {
			ret = LMAL_EINVAL;
			goto submit_out;
		}
		bool duplicate = false;
		for (uint32_t j = 0; j < ref_count; j++) {
			if (refs[j] == b) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			refs[ref_count++] = b;
		}
	}
	for (uint32_t i = 0; i < request->in_sync_count; i++) {
		if (sync(sync_handles[i]) == NULL) {
			ret = LMAL_EINVAL;
			goto submit_out;
		}
	}
	if (request->out_sync != 0 && sync(request->out_sync) == NULL) {
		ret = LMAL_EINVAL;
		goto submit_out;
	}
	if (request->out_sync != 0) {
		for (uint32_t i = 0; i < request->in_sync_count; i++) {
			if (sync_handles[i] == request->out_sync) {
				ret = LMAL_EINVAL;
				goto submit_out;
			}
		}
	}
	/* Panfrost JM writes progress/exception words back into the job descriptor.
	 * Validate the complete 64-byte header/payload in a client-owned writable BO;
	 * EXEC applies only to shader code and is not required on descriptor storage. */
	if ((request->jc & 63u) == 0 &&
	    request->jc <= UINT64_MAX - sizeof(uint32_t) * 16u) {
		jc_end = request->jc + sizeof(uint32_t) * 16u;
		for (uint32_t i = 0; i < ref_count; i++) {
			struct lmal_bo *b = refs[i];
			uint64_t end = 0;
			if (!lmal_add_overflow_u64(b->gpu_va, b->size, &end) &&
			    (b->flags & LMAL_BO_WRITE) && request->jc >= b->gpu_va &&
			    jc_end <= end) {
				jc_valid = true;
				break;
			}
		}
	}
	if (!jc_valid) {
		ret = LMAL_EINVAL;
		goto submit_out;
	}
	for (uint32_t i = 0; i < request->in_sync_count; i++) {
		if (!fSyncs[sync_handles[i] - 1].signaled) {
			/* v1 submit is synchronous; an unsignaled external dependency cannot be consumed. */
			ret = LMAL_EBUSY;
			goto submit_out;
		}
	}
	for (uint32_t i = 0; i < ref_count; i++) {
		refs[i]->in_flight++;
	}
	for (uint32_t i = 0; i < request->in_sync_count; i++) {
		fSyncs[sync_handles[i] - 1].in_flight++;
	}
	if (request->out_sync != 0) {
		fSyncs[request->out_sync - 1].in_flight++;
		fSyncs[request->out_sync - 1].signaled = false;
	}
	fJobsSubmitted++;
	{
		struct lm_job_result result;
		bzero(&result, sizeof(result));
		/* Keep the client lock while the core performs its serialized wait.
		 * ExternalMethod is not assumed to serialize calls; refs and slot generations
		 * therefore cannot be changed by close/map/reset while DMA owns them. */
		ret = lm_gpu_submit_wait(fVM, request->jc, request->requirements,
		    request->timeout_ns, &result);
		response->sequence = result.sequence;
		response->job_status = result.job_status;
		if (result.gpu_fault || result.mmu_fault) {
			fFaults++;
		}
		if (result.reset_completed) {
			fResets++;
		}
		if (result.quarantined || !lm_gpu_vm_safe_to_free(fVM)) {
			fVMQuarantined = true;
			fQuarantined++;
		}
		if (ret == LMAL_OK) {
			fJobsCompleted++;
			if (request->out_sync != 0) {
				fSyncs[request->out_sync - 1].signaled = true;
			}
		}
		for (uint32_t i = 0; i < ref_count; i++) {
			refs[i]->in_flight--;
			refs[i]->last_sequence = result.sequence;
			refs[i]->last_status = ret;
		}
		for (uint32_t i = 0; i < request->in_sync_count; i++) {
			fSyncs[sync_handles[i] - 1].in_flight--;
		}
		if (request->out_sync != 0) {
			fSyncs[request->out_sync - 1].in_flight--;
		}
	}
submit_out:
	IODelete(refs, struct lmal_bo *, request->bo_count);
	lmal_free_u32_array(handles, request->bo_count);
	lmal_free_u32_array(sync_handles, request->in_sync_count);
	return ret;
}

IOReturn
LancelotMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
	if (options == NULL || memory == NULL || fLock == NULL) {
		return kIOReturnBadArgument;
	}
	*memory = NULL;
	IOLockLock(fLock);
	if (!fAuthorized || fClosing || current_task() != fTask) {
		IOLockUnlock(fLock);
		return kIOReturnNotPermitted;
	}
	struct lmal_bo *b = bo(type); /* client-local handle, never a global PA/IOVA */
	if (b == NULL || b->memory == NULL) {
		IOLockUnlock(fLock);
		return kIOReturnBadArgument;
	}
	if (b->export_memory == NULL) {
		b->export_memory = LancelotMaliExport::withBacking(b->memory, fTask);
		if (b->export_memory == NULL) {
			IOLockUnlock(fLock);
			return kIOReturnNoMemory;
		}
	}
	/* Map AND Unmap invoke this callback. Only makeMapping tracks actual maps.
	 * Internal read-only cannot be overridden by kIOMapUserOptionsMask. */
	b->export_memory->retain();
	*memory = b->export_memory;
	*options = (b->flags & LMAL_BO_READONLY) ? kIOMapReadOnly : 0;
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
LancelotMaliUserClient::clientClose(void)
{
	if (fLock == NULL) {
		return kIOReturnSuccess;
	}
	retain();
	IOLockLock(fLock);
	if (fClosing) {
		IOReturn closed = fCloseResult;
		IOLockUnlock(fLock);
		release();
		return closed;
	}
	fClosing = true;
	fCloseResult = kIOReturnBusy;
	bool all_safe = !fVMQuarantined;
	for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
		if (fBOs[i].used && !release_bo(&fBOs[i])) {
			all_safe = false;
		}
	}
	if (all_safe && fVM != NULL) {
		int ret = lm_gpu_vm_destroy(fVM);
		if (ret == LMAL_OK) {
			fVM = NULL;
		} else {
			all_safe = false;
			fVMQuarantined = true;
			for (uint32_t i = 0; i < LMAL_MAX_BOS; i++) {
				if (fBOs[i].used) {
					quarantine_bo(&fBOs[i]);
				}
			}
		}
	}
	IOReturn close_result = all_safe ? kIOReturnSuccess : kIOReturnBusy;
	fCloseResult = close_result;
	IOLockUnlock(fLock);
	/* Termination can wait for other client activity: never hold fLock here. */
	if (!isInactive()) {
		terminate();
	}
	release();
	return close_result;
}

void
LancelotMaliUserClient::free(void)
{
	/* Quarantined BO references are deliberately leaked until a future reset policy can prove safety. */
	if (fSyncs != NULL) {
		IODelete(fSyncs, struct lmal_sync, LMAL_MAX_SYNCS);
		fSyncs = NULL;
	}
	if (fBOs != NULL) {
		IODelete(fBOs, struct lmal_bo, LMAL_MAX_BOS);
		fBOs = NULL;
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
LancelotMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	(void)dispatch;
	(void)target;
	(void)reference;
	if (args == NULL || args->structureInput == NULL || fLock == NULL ||
	    args->structureInputSize < sizeof(struct lmal_header) ||
	    args->structureInputSize > sizeof(union lmal_request) ||
	    args->structureInputDescriptor != NULL || args->structureOutputDescriptor != NULL ||
	    args->scalarInputCount != 0 || args->scalarOutputCount != 0 ||
	    args->asyncReferenceCount != 0 || args->asyncWakePort != MACH_PORT_NULL) {
		return kIOReturnBadArgument;
	}
	union lmal_request r;
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, args->structureInputSize);
	int result = lmal_client_validate(selector, &r, args->structureInputSize,
	    args->structureOutput != NULL ? args->structureOutputSize : 0);
	if (result != LMAL_OK) {
		args->structureOutputSize = 0;
		return lmal_ioreturn(result);
	}
	/* Own the client across sync_wait's unlocked sleep and concurrent Close. */
	retain();
	IOLockLock(fLock);
	if (!fAuthorized || current_task() != fTask) result = LMAL_EPERM;
	else if (fClosing || fVM == NULL) result = LMAL_ENODEV;
	else switch (selector) {
	case LMAL_SELECTOR_GET_PARAM:
		result = lm_gpu_param(r.get_param.param, &r.get_param.value);
		break;
	case LMAL_SELECTOR_BO_CREATE:
		result = bo_create(&r.bo_create, &r.bo_create);
		break;
	case LMAL_SELECTOR_BO_QUERY:
		result = bo_query(&r.bo_query, &r.bo_query);
		break;
	case LMAL_SELECTOR_BO_CLOSE:
		result = bo_close(&r.bo_close);
		break;
	case LMAL_SELECTOR_BO_UNMAP:
		result = bo_unmap(&r.bo_close);
		break;
	case LMAL_SELECTOR_BO_WAIT:
		result = bo_wait(&r.bo_wait);
		break;
	case LMAL_SELECTOR_SUBMIT:
		result = submit(&r.submit, &r.submit);
		break;
	case LMAL_SELECTOR_SYNCOBJ_CREATE:
		result = sync_create(&r.sync_create, &r.sync_create);
		break;
	case LMAL_SELECTOR_SYNCOBJ_DESTROY:
		result = sync_destroy(&r.sync_handle);
		break;
	case LMAL_SELECTOR_SYNCOBJ_RESET:
	case LMAL_SELECTOR_SYNCOBJ_SIGNAL:
		result = sync_set_array(&r.sync_array, selector == LMAL_SELECTOR_SYNCOBJ_SIGNAL);
		break;
	case LMAL_SELECTOR_SYNCOBJ_WAIT:
		result = sync_wait(&r.sync_wait, &r.sync_wait);
		break;
	case LMAL_SELECTOR_GET_CAPS:
		r.caps.jobs_submitted = fJobsSubmitted;
		r.caps.jobs_completed = fJobsCompleted;
		r.caps.faults = fFaults;
		r.caps.resets = fResets;
		r.caps.quarantined = fQuarantined;
		r.caps.max_bo_bytes = (uint32_t)LMAL_MAX_BO_BYTES;
		r.caps.max_vm_bytes = (uint32_t)LMAL_MAX_VM_BYTES;
		r.caps.flags = 0;
		break;
	case LMAL_SELECTOR_SURFACE_QUERY:
		r.surface_query.handle = 0;
		result = surface_query(&r.surface_query, &r.surface_query);
		break;
	case LMAL_SELECTOR_SURFACE_IMPORT:
		result = surface_import(&r.surface_import, &r.surface_import);
		break;
	case LMAL_SELECTOR_SYNC_IMPORT:
	case LMAL_SELECTOR_SYNC_EXPORT:
		result = LMAL_ENOTSUP;
		break;
	default:
		result = LMAL_ENOTSUP;
		break;
	}
	size_t written = 0;
	int copied = lmal_client_copy_reply(args->structureOutput, args->structureOutputSize,
	    &r, selector, result, &written);
	args->structureOutputSize = (uint32_t)written;
	IOLockUnlock(fLock);
	release();
	return lmal_ioreturn(copied);
}
