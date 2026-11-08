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
#include "LancelotMaliCpuLease.h"
#include "../../bsd/dev/lancelot_mali_queue.h"
static bool lmal_prepare_queued_client();
static IOUserClient *lmal_create_queued_client();
static bool lmal_prepare_hakios_client();
static IOUserClient *lmal_create_hakios_client();

/* Bounded metadata-only lease diagnostics: no pixels, no pointers, no behavior.
 * Each missing lease is counted; the first 48 and then every 256th are printed. */
static uint64_t gLmalLeaseCreated, gLmalLeaseMissing, gLmalImported, gLmalImportMissing;
static uint64_t gLmalLeaseSingle, gLmalLeaseSingleMissing;
static bool
lmal_lease_diag_sample(uint64_t n)
{
	return n <= 48 || (n % 256) == 0;
}
static void
lmal_lease_diag_create(uint32_t handle, uint64_t size, uint32_t flags, bool lease,
    const struct lmal_cpu_lease_why *why)
{
	uint64_t created = __atomic_add_fetch(&gLmalLeaseCreated, 1, __ATOMIC_RELAXED);
	bool single = size <= PAGE_SIZE;
	if (single) __atomic_add_fetch(&gLmalLeaseSingle, 1, __ATOMIC_RELAXED);
	uint64_t missing = lease ? __atomic_load_n(&gLmalLeaseMissing, __ATOMIC_RELAXED) :
	    __atomic_add_fetch(&gLmalLeaseMissing, 1, __ATOMIC_RELAXED);
	if (!lease && single) __atomic_add_fetch(&gLmalLeaseSingleMissing, 1, __ATOMIC_RELAXED);
	if ((created % 32) == 0) {
		printf("lancelot-lease-count: created=%llu missing=%llu single=%llu single_missing=%llu imported=%llu import_missing=%llu\n",
		    (unsigned long long)created, (unsigned long long)missing,
		    (unsigned long long)__atomic_load_n(&gLmalLeaseSingle, __ATOMIC_RELAXED),
		    (unsigned long long)__atomic_load_n(&gLmalLeaseSingleMissing, __ATOMIC_RELAXED),
		    (unsigned long long)__atomic_load_n(&gLmalImported, __ATOMIC_RELAXED),
		    (unsigned long long)__atomic_load_n(&gLmalImportMissing, __ATOMIC_RELAXED));
	}
	if (lease || !lmal_lease_diag_sample(missing)) return;
	printf("lancelot-lease-missing: site=bo_create pid=%d handle=%u size=%llu flags=%#x step=%u detail=%#x value=%llu pages=%u stage=%u first=%u bits=%u mismatch=%u final_first=%u final_bits=%u final_mismatch=%u created=%llu missing=%llu\n",
	    proc_selfpid(), handle, (unsigned long long)size, flags, why->step, why->detail,
	    (unsigned long long)why->value, why->pages, why->stage, why->first, why->bits, why->mismatch,
	    why->final_first, why->final_bits, why->final_mismatch,
	    (unsigned long long)created, (unsigned long long)missing);
}
static void
lmal_lease_diag_import(uint32_t handle, uint32_t surface_id, uint64_t size, bool lease)
{
	uint64_t imported = __atomic_add_fetch(&gLmalImported, 1, __ATOMIC_RELAXED);
	if (lease) return;
	uint64_t missing = __atomic_add_fetch(&gLmalImportMissing, 1, __ATOMIC_RELAXED);
	if (!lmal_lease_diag_sample(missing)) return;
	printf("lancelot-lease-missing: site=surface_import pid=%d handle=%u surface=%u size=%llu imported=%llu import_missing=%llu\n",
	    proc_selfpid(), handle, surface_id, (unsigned long long)size,
	    (unsigned long long)imported, (unsigned long long)missing);
}

extern "C" {
/* A wrapper-owned bootarg/platform state machine prepares the core first. */
void lancelot_mali_init(void);
void LancelotMaliPublish(void);
/* Parent-owned facade: one globalmali_lock lease across the two exact calls.
 * This declaration does not publish an async selector/capability. */
int lm_gpu_submit_pair(struct lm_vm *, uint64_t, uint64_t, uint64_t,
    struct lm_job_result results[2], const struct lm_range *, uint32_t,
    const struct lm_range *, uint32_t, int32_t, uint64_t);
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
/* Panfrost's non-coherent default is CPU write-combine. WB is a comparison
 * mode with the existing submit-time CPU maintenance, not hardware coherency. */
static uint32_t gLancelotBOCached;

static void
lmal_read_bootargs(void)
{
	if (gLancelotModeRead) {
		return;
	}
	gLancelotModeRead = true;
	gLancelotMode = 0;
	(void)PE_parse_boot_argn("lancelot_mali", &gLancelotMode, sizeof(gLancelotMode));
	(void)PE_parse_boot_argn("lancelot_bocache", &gLancelotBOCached, sizeof(gLancelotBOCached));
	if (gLancelotBOCached > 1) {
		printf("LancelotMali: invalid bocache=%u (0=writecombine 1=writeback)\n", gLancelotBOCached);
	}
	bzero(gLancelotClientName, sizeof(gLancelotClientName));
	gLancelotClientPid = 0;
	bool has_name = PE_parse_boot_argn("lancelot_mali_client", gLancelotClientName,
	    sizeof(gLancelotClientName));
	bool has_pid = PE_parse_boot_argn("lancelot_mali_client_pid", &gLancelotClientPid,
	    sizeof(gLancelotClientPid));
	/* A non-root client is admitted by the process name configured in the boot argument, and
	 * by its pid as well when one is configured. A pid is optional for a client whose pid
	 * launchd assigns at boot -- the window server is the one this service is for -- and
	 * requiring one there would make the identity a different process every boot. */
	if (!has_pid) {
		gLancelotClientPid = 0;
	}
	gLancelotClientIdentity = has_name && gLancelotClientName[0] != '\0' &&
	    (!has_pid || gLancelotClientPid != 0);
}

static bool
lmal_render_enabled(void)
{
	lmal_read_bootargs();
	/* Modes 1..3 are platform bring-up/self-test only. */
	return gLancelotMode == 4 && gLancelotBOCached <= 1;
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
	/* The non-root path is the configured process name, and the pid too when one was given. */
	if (!gLancelotClientIdentity ||
	    (gLancelotClientPid != 0 && (uint32_t)proc_pid(p) != gLancelotClientPid)) {
		return false;
	}
	char name[sizeof(gLancelotClientName)];
	bzero(name, sizeof(name));
	proc_name(proc_pid(p), name, sizeof(name));
	if (strncmp(name, gLancelotClientName, sizeof(name)) != 0) {
		return false;
	}
	printf("LancelotMali: non-root client %s pid=%d admitted by the boot argument\n", name, proc_pid(p));
	return true;
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

/* Prepared worker/facade boundary. NOT wired to a user selector: current CPU
 * exports/IOSurface aliases do not yet supply the required queue lease. All
 * ranges are materialized before enqueue; no allocation/copyin/task lookup
 * occurs in this dispatch. Actual owner identity is retained in the payload. */
static inline int
lmal_pair_execute_facade(const struct lmal_pair_payload *p,
    const struct lm_range *vt_ranges, const struct lm_range *fs_ranges,
    uint64_t remaining_ns, struct lmal_pair_execution *execution)
{
    static_assert(sizeof(struct lm_range) == LMAL_PAIR_CORE_RANGE_BYTES,
        "pair metadata range budget/layout");
    static_assert(sizeof(struct lmal_pair_range_snapshot) == sizeof(struct lm_range) &&
        offsetof(struct lmal_pair_range_snapshot, va) == offsetof(struct lm_range, va) &&
        offsetof(struct lmal_pair_range_snapshot, pages) == offsetof(struct lm_range, pages) &&
        offsetof(struct lmal_pair_range_snapshot, flags) == offsetof(struct lm_range, flags),
        "pair/core exact range fields");
    if (!p || !execution || !vt_ranges || !fs_ranges || !p->pins_applied ||
        !p->authorized || !p->kernel_arrays_owned || !p->objects_retained ||
        !p->pair_token || !p->identity || p->identity->source_task != p->source_task ||
        p->identity->source_pid != p->source_pid || p->source_pid <= 0 ||
        p->identity->client != p->client || p->identity->vm != p->vm ||
        !remaining_ns || remaining_ns > LMAL_MAX_WAIT_NS) return LMAL_EINVAL;
    if (vt_ranges != p->stage[0].ranges || fs_ranges != p->stage[1].ranges)
        return LMAL_EINVAL;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        const struct lm_range *ranges = stage ? fs_ranges : vt_ranges;
        const struct lmal_pair_stage_snapshot *s = &p->stage[stage];
        if (!s->bo_count || s->bo_count > LMAL_MAX_SUBMIT_BOS || !s->bos ||
            s->requirements != stage || !s->jc ||
            (s->jc & 63u)) return LMAL_EINVAL;
        if (s->bo_count > LMAL_PAIR_STAGE_BO_LIMIT) return LMAL_ENOTSUP;
        for (uint32_t i = 0; i < s->bo_count; ++i) {
            const struct lmal_pair_bo_snapshot *b = &s->bos[i];
            /* Keep GPU-only optimization out of this initial queue adapter.
             * RO is still explicit; all CPU/alias leases must be real. */
            if (!b->lease_retained || !b->cpu_aliases_excluded ||
                !b->external_aliases_excluded || ranges[i].va != b->va ||
                ranges[i].pages != b->bytes / LMAL_PAGE_SIZE ||
                ranges[i].flags != (b->readonly ? LM_RANGE_READONLY : 0))
                return LMAL_ENOTSUP;
        }
    }
    struct lm_job_result results[2];
    bzero(results, sizeof(results));
    int ret = lm_gpu_submit_pair((struct lm_vm *)p->vm, p->stage[0].jc,
        p->stage[1].jc, remaining_ns, results, vt_ranges, p->stage[0].bo_count,
        fs_ranges, p->stage[1].bo_count, p->source_pid, p->pair_token);
    bzero(execution, sizeof(*execution));
    for (uint32_t i = 0; i < 2; ++i) {
        execution->sequence[i] = results[i].sequence;
        execution->started_ns[i] = results[i].started_ns;
        execution->completed_ns[i] = results[i].completed_ns;
        execution->job_status[i] = results[i].job_status;
        execution->error[i] = results[i].pair_stage_error;
        if (results[i].pair_stage_attempted) {
            execution->attempted_mask |= 1u << i;
            if (!results[i].pair_stage_error) execution->completed_mask |= 1u << i;
        }
    }
    execution->dma_safe = !results[0].quarantined && !results[1].quarantined &&
        lm_gpu_vm_safe_to_free((struct lm_vm *)p->vm);
    execution->offline = !lm_gpu_ready();
    bool malformed = false;
    for (uint32_t i = 0; i < 2; ++i)
        if (!results[i].pair_stage_attempted && (results[i].sequence ||
            results[i].pair_stage_error || results[i].started_ns || results[i].completed_ns))
            malformed = true;
    if (malformed || (!ret && (execution->attempted_mask != 3 ||
        execution->completed_mask != 3 || !results[0].sequence || !results[1].sequence ||
        results[0].gpu_fault || results[0].mmu_fault ||
        results[1].gpu_fault || results[1].mmu_fault))) {
        execution->dma_safe = 0;
        execution->offline = 1;
        execution->completed_mask = 0;
        return LMAL_EIO;
    }
    return ret;
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
	virtual IOReturn newUserClient(task_t owningTask, void *security_id, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LancelotMali, IOService);
OSDefineMetaClassAndStructors(LancelotMaliCpuLease, OSObject);
extern "C" void lmal_cpu_lease_retain(void *p) {
    if (p) ((LancelotMaliCpuLease *)p)->retain();
}
extern "C" void lmal_cpu_lease_release(void *p) {
    if (p) ((LancelotMaliCpuLease *)p)->release();
}
extern "C" int lmal_cpu_lease_map_lock(void *p, int reference) {
    return p ? ((LancelotMaliCpuLease *)p)->lockMapping(reference != 0) : LMAL_OK;
}
extern "C" void lmal_cpu_lease_map_unlock(void *p) {
    if (p) ((LancelotMaliCpuLease *)p)->unlockMapping();
}

struct lmal_bo {
	IOMemoryDescriptor *memory;
	LancelotMaliCpuLease *cpu_lease;
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
	uint64_t fSyncPairToken, fSyncPairReportAt;
	uint64_t fSyncPairRejectCount, fSyncPairRejectAt;
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
	int submit(const struct lmal_submit *request, struct lmal_submit *response, uint64_t readonly = 0);
	int submit_pair_sync(const struct lmal_submit_pair_sync *, struct lmal_submit_pair_sync *, uint64_t,
	    struct lmal_pair_admission * = NULL);
	void pair_admission_diagnostic(uint32_t, int, const struct lmal_pair_admission *);
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
	if (lancelot_hakios_backend() && !lmal_prepare_hakios_client()) return false;
	if (lancelot_mali_queued_backend() && !lmal_prepare_queued_client()) return false;
    setProperty(kIOUserClientClassKey, lancelot_hakios_backend()?"LancelotHakiosMaliUserClient":lancelot_mali_queued_backend() ?
        "LancelotMaliQueuedUserClient" : "LancelotMaliUserClient");
	setName(LMAL_SERVICE_NAME);
	printf("LancelotMali: bocache=%u BO CPU=%s; IOSurface keeps owner cache policy\n",
	    gLancelotBOCached, gLancelotBOCached ? "writeback" : "writecombine");
	registerService();
	printf("LancelotMali: service published name=%s\n", LMAL_SERVICE_NAME);
	return true;
}

IOReturn
LancelotMali::newUserClient(task_t owningTask, void *security_id, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	IOUserClient *client;

	if (handler == NULL) {
		return kIOReturnBadArgument;
	}
	*handler = NULL;
	client = lancelot_hakios_backend()?lmal_create_hakios_client():lancelot_mali_queued_backend() ? lmal_create_queued_client() : new LancelotMaliUserClient;
	if (client == NULL) {
		printf("LancelotMali: user client allocation failed\n");
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, security_id, type, properties)) {
		printf("LancelotMali: user client refused type=%#x\n", type);
		client->release();
		return kIOReturnNotPermitted;
	}
	if (!client->attach(this)) {
		printf("LancelotMali: user client attach failed\n");
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		printf("LancelotMali: user client start failed\n");
		client->detach(this);
		client->release();
		return kIOReturnNotPermitted;
	}
	printf("LancelotMali: user client opened type=%#x\n", type);
	*handler = client;
	return kIOReturnSuccess;
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
	fSyncPairToken = 0;
	fSyncPairReportAt = 0;
	fSyncPairRejectCount = 0;
	fSyncPairRejectAt = 0;
	fAuthorized = false;
	fClosing = false;
	fCloseResult = kIOReturnSuccess;
	fVMQuarantined = false;
	if (type != LMAL_OPEN_TYPE) {
		printf("LancelotMali: open type %#x refused (want %#x)\n", type, LMAL_OPEN_TYPE);
		return false;
	}
	if (!lmal_authorized_task(owningTask)) {
		printf("LancelotMali: task not authorized for the GPU service\n");
		return false;
	}
	fTask = owningTask;
	fAuthorized = true;
	proc_t owner = (proc_t)get_bsdtask_info(owningTask);
	printf("LancelotMali: client identity pid=%d task-authorized=1\n", owner != NULL ? proc_pid(owner) : -1);
	return true;
}

bool
LancelotMaliUserClient::start(IOService *provider)
{
	if (!fAuthorized || provider == NULL || !lm_gpu_ready() ||
	    !IOUserClient::start(provider)) {
		printf("LancelotMali: client start refused auth=%d provider=%d ready=%d\n",
		    fAuthorized ? 1 : 0, provider != NULL ? 1 : 0, lm_gpu_ready() ? 1 : 0);
		if (fAuthorized && provider != NULL && !lm_gpu_ready()) {
			lm_gpu_report_first_failure(); /* Cached original reason, no hardware access/revival. */
		}
		return false;
	}
	fLock = IOLockAlloc();
	fBOs = IONewZero(struct lmal_bo, LMAL_MAX_BOS);
	fSyncs = IONewZero(struct lmal_sync, LMAL_MAX_SYNCS);
	fNextVA = LM_VA_MIN;
	if (fLock == NULL || fBOs == NULL || fSyncs == NULL) {
		printf("LancelotMali: client metadata allocation failed lock=%u bos=%u syncs=%u\n",
		    fLock != NULL, fBOs != NULL, fSyncs != NULL);
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
		printf("LancelotMali: client VM creation failed ready=%u\n", lm_gpu_ready());
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
	/* Select the memory object's cache mode at allocation, before any export.
	 * XNU applies it to the backing and default-cache mappings inherit it.
	 * Changing only IOConnectMapMemory's user alias would leave mismatched
	 * aliases. Imported IOSurfaces use their owner's descriptor instead. */
	IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithOptions(
	    kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gLancelotBOCached ? 0 : kIOMapWriteCombineCache),
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
		for (uint32_t i = 0; i < LMAL_MAX_BOS; i++)
			if (fBOs[i].used && fBOs[i].memory == memory && fBOs[i].cpu_lease != NULL) {
				cpu = (uint8_t *)fBOs[i].cpu_lease->cacheAddress();
				break;
			}
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
	if (safe && b->cpu_lease != NULL) {
		b->cpu_lease->release();
		b->cpu_lease = NULL;
	}
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
	if (b->cpu_lease != NULL) {
		b->cpu_lease->release();
		b->cpu_lease = NULL;
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
	struct lmal_cpu_lease_why lease_why = {};
	b->cpu_lease = LancelotMaliCpuLease::withRoot(memory, true, 2, &lease_why);
	b->size = size;
	b->gpu_va = va;
	b->flags = request->flags | LMAL_BO_FIXED_BACKING;
	if ((b->flags & (LMAL_BO_EXEC | LMAL_BO_NOEXEC)) == 0) {
		b->flags |= LMAL_BO_NOEXEC;
	}
	lmal_lease_diag_create(index + 1, size, b->flags, b->cpu_lease != NULL, &lease_why);
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
	if (b->cpu_lease != NULL) {
		uint64_t now = lmal_now_ns();
		uint64_t remaining = request->absolute_monotonic_ns > now ?
		    request->absolute_monotonic_ns - now : 0;
		if (remaining > LMAL_MAX_WAIT_NS) remaining = LMAL_MAX_WAIT_NS;
		int waited = b->cpu_lease->waitCpu(remaining);
		if (waited) return waited;
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
	b->cpu_lease = surface->maliCpuLease;
	if (b->cpu_lease != NULL) b->cpu_lease->retain();
	b->surface_id = request->surface_id;
	lmal_lease_diag_import(index + 1, request->surface_id, size, b->cpu_lease != NULL);
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
    struct lmal_submit *response, uint64_t readonly)
{
	uint32_t *handles = NULL;
	uint32_t *access = NULL;
	uint32_t *sync_handles = NULL;
	bool jc_valid = false;
	uint64_t jc_end = 0;
	int ret = LMAL_OK;
	struct lmal_bo **refs = NULL;
	struct lm_range *ranges = NULL;
	uint32_t ref_count = 0;
	LancelotMaliCpuLease **cpu_leases = NULL;
	uint64_t *cpu_cookies = NULL;
	bool *cpu_writers = NULL;
	uint32_t cpu_count = 0;
	uint64_t cpu_deadline = 0;
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
	if (readonly != 0) {
		ret = lmal_copy_u32_array(readonly, request->bo_count, LMAL_MAX_SUBMIT_BOS, &access);
		if (ret != LMAL_OK) {
			goto submit_out;
		}
		for (uint32_t i = 0; i < request->bo_count; i++) {
			if (access[i] & ~LMAL_ACCESS_READONLY) {
				ret = LMAL_EINVAL;
				goto submit_out;
			}
		}
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
				if (access != NULL) {
					access[j] &= access[i]; /* Any writable duplicate wins. */
				}
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			if (access != NULL) {
				access[ref_count] = access[i];
			}
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
	/* Exact retained mappings, deduplicated above. The optional core workset
	 * masks all other PTEs before narrowing CPU maintenance. */
	ranges = IONew(struct lm_range, ref_count);
	if (ranges == NULL) {
		ret = LMAL_ENOMEM;
		goto submit_out;
	}
	for (uint32_t i = 0; i < ref_count; i++) {
		ranges[i].va = refs[i]->gpu_va;
		ranges[i].pages = (uint32_t)(refs[i]->size / LMAL_PAGE_SIZE);
		bool contains_job = request->jc >= refs[i]->gpu_va &&
		    request->jc - refs[i]->gpu_va < refs[i]->size;
		ranges[i].flags = lmal_client_gpu_only_eligible(gLancelotBOCached == 0,
		    refs[i]->surface != NULL, refs[i]->export_memory != NULL, contains_job) ? LM_RANGE_GPU_ONLY : 0;
		if (access != NULL && access[i] == LMAL_ACCESS_READONLY) {
			if (contains_job) {
				ret = LMAL_EINVAL;
				goto submit_out;
			}
			ranges[i].flags |= LM_RANGE_READONLY;
		}
	}
	/* Legacy remains synchronous, but cannot bypass active CPU/presenter
	 * ownership. Sort roots identically to the presenter's reader acquisition. */
	cpu_leases = IONew(LancelotMaliCpuLease *, ref_count);
	cpu_cookies = IONew(uint64_t, ref_count);
	cpu_writers = IONew(bool, ref_count);
	if (!cpu_leases || !cpu_cookies || !cpu_writers) { ret = LMAL_ENOMEM; goto submit_out; }
	bzero(cpu_cookies, sizeof(*cpu_cookies) * ref_count);
	for (uint32_t i = 0; i < ref_count; i++) {
		LancelotMaliCpuLease *lease = refs[i]->cpu_lease;
		if (!lease) continue;
		uint32_t j = 0;
		while (j < cpu_count && (uintptr_t)cpu_leases[j] < (uintptr_t)lease) j++;
		bool writer = (refs[i]->flags & LMAL_BO_WRITE) &&
		    !(ranges[i].flags & LM_RANGE_READONLY);
		if (j < cpu_count && cpu_leases[j] == lease) { cpu_writers[j] |= writer; continue; }
		for (uint32_t k = cpu_count; k > j; k--) {
			cpu_leases[k] = cpu_leases[k - 1]; cpu_writers[k] = cpu_writers[k - 1];
		}
		cpu_leases[j] = lease;
		cpu_writers[j] = writer;
		cpu_count++;
	}
	{
		uint64_t now = lmal_now_ns();
		if (!now || lmal_add_overflow_u64(now, request->timeout_ns, &cpu_deadline)) {
			ret = LMAL_EINVAL; goto submit_out;
		}
	}
	for (uint32_t i = 0; i < cpu_count; i++) {
		uint64_t now = lmal_now_ns();
		if (now >= cpu_deadline) { ret = LMAL_ETIMEDOUT; goto submit_out; }
		ret = cpu_leases[i]->beginGpu(cpu_deadline - now, &cpu_cookies[i], false, cpu_writers[i]);
		if (ret) goto submit_out;
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
		for (uint32_t i = 0; i < cpu_count; i++)
			(void)cpu_leases[i]->markLegacy(cpu_cookies[i]);
		/* Keep the client lock while the core performs its serialized wait.
		 * ExternalMethod is not assumed to serialize calls; refs and slot generations
		 * therefore cannot be changed by close/map/reset while DMA owns them. */
		uint64_t now = lmal_now_ns();
		ret = now >= cpu_deadline ? LMAL_ETIMEDOUT : lm_gpu_submit_ranges(fVM,
		    request->jc, request->requirements, cpu_deadline - now, &result, ranges, ref_count);
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
		for (uint32_t i = 0; i < cpu_count; i++) {
			int ended = cpu_leases[i]->endGpu(cpu_cookies[i], ret,
			    !fVMQuarantined && !result.quarantined);
			cpu_cookies[i] = 0; /* unknown DMA retains its cookie inside the root */
			if (ended) {
				if (!ret) ret = ended;
				fVMQuarantined = true;
			}
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
	if (cpu_cookies != NULL) {
		for (uint32_t i = 0; i < cpu_count; i++)
			if (cpu_cookies[i]) (void)cpu_leases[i]->endGpu(cpu_cookies[i], LMAL_OK, true);
		IODelete(cpu_cookies, uint64_t, ref_count);
	}
	if (cpu_leases != NULL) IODelete(cpu_leases, LancelotMaliCpuLease *, ref_count);
	if (cpu_writers != NULL) IODelete(cpu_writers, bool, ref_count);
	lmal_free_u32_array(access, request->bo_count);
	if (ranges != NULL) {
		IODelete(ranges, struct lm_range, ref_count);
	}
	IODelete(refs, struct lmal_bo *, request->bo_count);
	lmal_free_u32_array(handles, request->bo_count);
	lmal_free_u32_array(sync_handles, request->in_sync_count);
	return ret;
}

#include "lancelot_mali_sync_pair.inc"

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
		if (b->cpu_lease != NULL) {
			int waited = b->cpu_lease->waitCpu(LMAL_MAX_WAIT_NS);
			if (waited) { IOLockUnlock(fLock); return lmal_ioreturn(waited); }
		}
		/* Serialize the first CPU ownership handoff before any mapping object
		 * escapes. Once export_memory exists, future submits conservatively
		 * maintain this BO even if every map has since been unmapped. */
		if (gLancelotBOCached == 0 && b->surface == NULL) {
			int result = lm_gpu_vm_cpu_access(fVM, b->gpu_va, (uint32_t)(b->size / LMAL_PAGE_SIZE));
			if (result != LMAL_OK) {
				IOLockUnlock(fLock);
				return lmal_ioreturn(result);
			}
		}
		b->export_memory = LancelotMaliExport::withBacking(b->memory, fTask, b->cpu_lease);
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
		if ((selector == LMAL_SELECTOR_SUBMIT_PAIR_SYNC || selector == LMAL_SELECTOR_CHECK_PAIR_SYNC) &&
		    IOLockTryLock(fLock)) {
			if (fAuthorized && current_task() == fTask && !fClosing) {
				struct lmal_pair_admission detail = {};
				detail.reason = LMAL_PAIR_REASON_WIRE;
				detail.stage_index = LMAL_PAIR_STAGE_NONE;
				pair_admission_diagnostic(selector, result, &detail);
			}
			IOLockUnlock(fLock);
		}
		args->structureOutputSize = 0;
		return lmal_ioreturn(result);
	}
	/* Own the client across sync_wait's unlocked sleep and concurrent Close. */
	retain();
	uint64_t pair_started = 0;
	if (selector == LMAL_SELECTOR_SUBMIT_PAIR_SYNC || selector == LMAL_SELECTOR_CHECK_PAIR_SYNC) {
		pair_started = lmal_now_ns();
		bool locked = false;
		int lock_error = pair_started ? LMAL_ETIMEDOUT : LMAL_EIO;
		for (uint32_t i = 0; pair_started && i < 10000; i++) {
			uint64_t now = lmal_now_ns();
			if (now < pair_started) { lock_error = LMAL_EIO; break; }
			if (now-pair_started >= r.submit_pair_sync.timeout_ns) break;
			if (IOLockTryLock(fLock)) { locked = true; break; }
			IOSleep(1); /* bounded, preemptible client-lock wait */
		}
		if (!locked) {
			size_t written = 0;
			int copied = lmal_client_copy_reply(args->structureOutput, args->structureOutputSize,
			    &r, selector, lock_error, &written);
			args->structureOutputSize = (uint32_t)written;
			release(); return lmal_ioreturn(copied);
		}
	} else IOLockLock(fLock);
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
	case LMAL_SELECTOR_SUBMIT_ACCESS:
		result = submit(&r.submit_access.submit, &r.submit_access.submit, r.submit_access.bo_readonly);
		break;
	case LMAL_SELECTOR_SUBMIT_PAIR_SYNC:
		result = submit_pair_sync(&r.submit_pair_sync, &r.submit_pair_sync, pair_started);
		break;
	case LMAL_SELECTOR_CHECK_PAIR_SYNC:
		result = submit_pair_sync(&r.check_pair_sync.submit, &r.check_pair_sync.submit,
		    pair_started, &r.check_pair_sync.admission);
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
		r.caps.flags = LMAL_CAP_SUBMIT_ACCESS | LMAL_CAP_SUBMIT_PAIR_SYNC | LMAL_CAP_CHECK_PAIR_SYNC;
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

#include "lancelot_mali_queue_client.inc"

#include "lancelot_hakios_client.inc"
