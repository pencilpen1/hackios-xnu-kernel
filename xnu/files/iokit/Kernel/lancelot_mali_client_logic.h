/* Production LMAL v1 validation/lifetime policy; also compiled by host tests. */
#ifndef LANCELOT_MALI_CLIENT_LOGIC_H
#define LANCELOT_MALI_CLIENT_LOGIC_H
#include <stddef.h>
#include "lancelot_mali_uapi.h"
#define LMAL_CLIENT_BO_FLAGS (LMAL_BO_READ | LMAL_BO_WRITE | LMAL_BO_EXEC | LMAL_BO_NOEXEC | LMAL_BO_HEAP | LMAL_BO_FIXED_BACKING | LMAL_BO_READONLY)
#define LMAL_CLIENT_EXPORT_MAPS 16u
union lmal_request {
    struct lmal_header h;
    struct lmal_get_param get_param;
    struct lmal_bo_create bo_create;
    struct lmal_bo_query bo_query;
    struct lmal_bo_close bo_close;
    struct lmal_bo_wait bo_wait;
    struct lmal_submit submit;
    struct lmal_submit_access submit_access;
    struct lmal_submit_pair_sync submit_pair_sync;
    struct lmal_check_pair_sync check_pair_sync;
    struct lmal_syncobj_create sync_create;
    struct lmal_syncobj_handle sync_handle;
    struct lmal_syncobj_array sync_array;
    struct lmal_syncobj_wait sync_wait;
    struct lmal_sync_file sync_file;
    struct lmal_caps caps;
    struct lmal_surface_query surface_query;
    struct lmal_surface_import surface_import;
};
static inline size_t lmal_client_message_size(uint32_t selector)
{
    switch (selector) {
    case LMAL_SELECTOR_GET_PARAM: return sizeof(struct lmal_get_param);
    case LMAL_SELECTOR_BO_CREATE: return sizeof(struct lmal_bo_create);
    case LMAL_SELECTOR_BO_QUERY: return sizeof(struct lmal_bo_query);
    case LMAL_SELECTOR_BO_CLOSE:
    case LMAL_SELECTOR_BO_UNMAP: return sizeof(struct lmal_bo_close);
    case LMAL_SELECTOR_BO_WAIT: return sizeof(struct lmal_bo_wait);
    case LMAL_SELECTOR_SUBMIT: return sizeof(struct lmal_submit);
    case LMAL_SELECTOR_SUBMIT_ACCESS: return sizeof(struct lmal_submit_access);
    case LMAL_SELECTOR_SUBMIT_PAIR_SYNC: return sizeof(struct lmal_submit_pair_sync);
    case LMAL_SELECTOR_CHECK_PAIR_SYNC: return sizeof(struct lmal_check_pair_sync);
    case LMAL_SELECTOR_SYNCOBJ_CREATE: return sizeof(struct lmal_syncobj_create);
    case LMAL_SELECTOR_SYNCOBJ_DESTROY: return sizeof(struct lmal_syncobj_handle);
    case LMAL_SELECTOR_SYNCOBJ_RESET:
    case LMAL_SELECTOR_SYNCOBJ_SIGNAL: return sizeof(struct lmal_syncobj_array);
    case LMAL_SELECTOR_SYNCOBJ_WAIT: return sizeof(struct lmal_syncobj_wait);
    case LMAL_SELECTOR_GET_CAPS: return sizeof(struct lmal_caps);
    case LMAL_SELECTOR_SYNC_IMPORT:
    case LMAL_SELECTOR_SYNC_EXPORT: return sizeof(struct lmal_sync_file);
    case LMAL_SELECTOR_SURFACE_QUERY: return sizeof(struct lmal_surface_query);
    case LMAL_SELECTOR_SURFACE_IMPORT: return sizeof(struct lmal_surface_import);
    default: return 0;
    }
}
static inline int lmal_client_bo_flags_valid(uint32_t flags, int surface)
{
    return !(flags & ~LMAL_CLIENT_BO_FLAGS) &&
        !((flags & LMAL_BO_EXEC) && (flags & LMAL_BO_NOEXEC)) &&
        !((flags & LMAL_BO_READONLY) && (flags & LMAL_BO_WRITE)) &&
        !(surface && (flags & (LMAL_BO_EXEC | LMAL_BO_HEAP)));
}
/* Backing/export length uses the advertised runtime kernel granule, not Mali's
 * translation granule. Only the audited XNU 4K/16K cases are admitted. */
static inline uint64_t lmal_client_round_backing(uint64_t bytes,
    uint64_t kernel_page_size, uint64_t cap)
{
    uint64_t mask, rounded;
    if (kernel_page_size != LMAL_PAGE_SIZE && kernel_page_size != UINT64_C(16384)) return 0;
    mask = kernel_page_size - 1;
    if (!bytes || bytes > cap || bytes > UINT64_MAX - mask) return 0;
    rounded = (bytes + mask) & ~mask;
    return rounded <= cap ? rounded : 0;
}
static inline uint64_t lmal_client_surface_backing(uint64_t pixels,
    uint64_t kernel_page_size, uint64_t parent_length, uint64_t cap)
{
    uint64_t rounded = lmal_client_round_backing(pixels, kernel_page_size, cap);
    if (!rounded || parent_length < kernel_page_size ||
        rounded > parent_length - kernel_page_size) return 0;
    return rounded; /* the preceding state page is NOT GPU pixel backing */
}
/* v1 imports only one linear, uncompressed RGBA/BGRA8 pixel plane, no crop.
 * Keep metadata validation at the actual kernel admission boundary, not only
 * in Mesa. Reject unsupported layout rather than mapping it as ordinary RGBA. */
static inline int lmal_client_surface_layout(uint32_t width, uint32_t height,
    uint32_t row_bytes, uint32_t offset, uint32_t format, uint32_t planes,
    uint32_t bytes_per_element, uint32_t element_width, uint32_t element_height,
    uint64_t allocated, uint32_t compression, uint32_t extended_pixels,
    uint32_t protection, uint32_t parent_id)
{
    /* MiniIOSurface planeCount=0 denotes its ordinary implicit single plane. */
    if (!width || !height || width > 4096 || height > 4096 || planes > 1 ||
        offset || bytes_per_element != 4 || element_width != 1 || element_height != 1 ||
        (format != UINT32_C(0x42475241) && format != UINT32_C(0x52474241)) ||
        row_bytes < (uint64_t)width * 4 || (row_bytes & 3) ||
        (uint64_t)row_bytes * height > allocated || allocated > LMAL_MAX_BO_BYTES ||
        compression || extended_pixels || protection || parent_id) return LMAL_ENOTSUP;
    return LMAL_OK;
}
static inline uint32_t lmal_client_gpu_page_count(uint64_t bytes, uint32_t maximum)
{
    uint64_t count;
    if (!bytes || (bytes & (LMAL_PAGE_SIZE - 1))) return 0;
    count = bytes / LMAL_PAGE_SIZE;
    return count <= maximum ? (uint32_t)count : 0;
}
static inline uint64_t lmal_client_gpu_page_offset(uint32_t index)
{
    return (uint64_t)index * LMAL_PAGE_SIZE;
}
static inline int lmal_client_gpu_segment_valid(uint64_t pa, uint64_t segment_length)
{
    return pa && !(pa & (LMAL_PAGE_SIZE - 1)) && segment_length >= LMAL_PAGE_SIZE;
}
/* Core flag values are supplied by the caller; no private core ABI is copied. */
static inline uint32_t lmal_client_gpu_permissions(uint32_t bo_flags,
    uint32_t write_flag, uint32_t exec_flag)
{
    uint32_t permissions = 0;
    if ((bo_flags & LMAL_BO_WRITE) && !(bo_flags & LMAL_BO_READONLY)) permissions |= write_flag;
    if (bo_flags & LMAL_BO_EXEC) permissions |= exec_flag;
    return permissions;
}
static inline int lmal_client_allocation_admit(int ready, int quarantined, int has_vm)
{
    if (!ready || !has_vm) return LMAL_ENODEV;
    return quarantined ? LMAL_EBUSY : LMAL_OK;
}
/* Never infer GPU-only from a name, size, mapCount()==0 or a user flag.
 * A created WC BO with no export descriptor has never been handed to a CPU
 * mapping path; imported surfaces and all prior export attempts stay excluded. */
static inline int lmal_client_gpu_only_eligible(int cpu_wc, int imported,
    int export_ever_requested, int contains_job)
{
    return cpu_wc && !imported && !export_ever_requested && !contains_job;
}
/* Failed table-only map/collection: caller proves no DMA can own new backing.
 * Descriptor completion must succeed before either descriptor or surface drops. */
static inline int lmal_client_cleanup_new_backing(void *memory, void *surface,
    int safe_to_free, int (*complete)(void *), void (*release_memory)(void *),
    void (*release_surface)(void *))
{
    if (!safe_to_free) return LMAL_EBUSY;
    if (complete(memory) != LMAL_OK) return LMAL_EIO;
    release_memory(memory);
    if (surface) release_surface(surface);
    return LMAL_OK;
}
/* Called on an aligned private snapshot, BEFORE allocations, copyin or DMA. */
static inline int lmal_client_validate(uint32_t selector, const union lmal_request *r,
    size_t input_size, size_t output_capacity)
{
    size_t exact = lmal_client_message_size(selector);
    if (!exact) return LMAL_ENOTSUP;
    if (!r || input_size != exact || r->h.size != exact ||
        r->h.magic != LMAL_UAPI_MAGIC || r->h.version != LMAL_UAPI_VERSION ||
        r->h.flags || r->h.reserved0 || r->h.reserved1 || r->h.result) return LMAL_EINVAL;
    if (output_capacity < exact) return LMAL_EFAULT;
    switch (selector) {
    case LMAL_SELECTOR_GET_PARAM:
        if (r->get_param.reserved) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_BO_CREATE:
        if (r->bo_create.reserved || !lmal_client_bo_flags_valid(r->bo_create.flags, 0) ||
            !r->bo_create.size || r->bo_create.size > LMAL_MAX_BO_BYTES) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_BO_QUERY:
        if (r->bo_query.reserved0 || r->bo_query.reserved1) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_BO_CLOSE:
    case LMAL_SELECTOR_BO_UNMAP:
        if (r->bo_close.reserved) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_BO_WAIT:
        if (r->bo_wait.flags & ~(LMAL_BO_WAIT_READ | LMAL_BO_WAIT_WRITE)) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_SUBMIT_ACCESS:
        if (!r->submit_access.bo_readonly) return LMAL_EINVAL;
        /* The prefix has identical validation. */
        /* fall through */
    case LMAL_SELECTOR_SUBMIT:
        if (r->submit.reserved || (r->submit.requirements & ~LMAL_SUBMIT_REQUIRE_COMPLETE) ||
            !r->submit.bo_count || r->submit.bo_count > LMAL_MAX_SUBMIT_BOS ||
            !r->submit.bo_handles || r->submit.in_sync_count > LMAL_MAX_SUBMIT_SYNCS ||
            (r->submit.in_sync_count && !r->submit.in_syncs) ||
            (!r->submit.in_sync_count && r->submit.in_syncs) ||
            !r->submit.timeout_ns || r->submit.timeout_ns > LMAL_MAX_WAIT_NS || !r->submit.jc ||
            (r->submit.jc & 63u)) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_CHECK_PAIR_SYNC:
        if (r->check_pair_sync.admission.eligible || r->check_pair_sync.admission.reason ||
            r->check_pair_sync.admission.stage_index || r->check_pair_sync.admission.bo_handle ||
            r->check_pair_sync.admission.surface_id || r->check_pair_sync.admission.bo_flags ||
            r->check_pair_sync.admission.bo_state || r->check_pair_sync.admission.reserved) return LMAL_EINVAL;
        /* Identical snapshot/JC/sync validation; only the exact wire size differs. */
        /* fall through */
    case LMAL_SELECTOR_SUBMIT_PAIR_SYNC: {
        const struct lmal_submit_pair_sync *p = &r->submit_pair_sync;
        if (!p->timeout_ns || p->timeout_ns > LMAL_MAX_WAIT_NS ||
            p->in_sync_count > LMAL_MAX_SUBMIT_SYNCS ||
            (!!p->in_sync_count != !!p->in_syncs) ||
            p->attempted_mask || p->completed_mask || p->reserved[0] || p->reserved[1])
            return LMAL_EINVAL;
        for (uint32_t i = 0; i < 2; i++) {
            const struct lmal_sync_pair_stage *s = &p->stage[i];
            if (!s->jc || (s->jc & 63u) || s->jc > UINT64_MAX - 64u ||
                !s->bo_count || s->bo_count > LMAL_SYNC_PAIR_STAGE_BOS ||
                !s->bo_handles || !s->bo_readonly || s->reserved ||
                p->sequence[i] || p->job_status[i] || p->stage_error[i]) return LMAL_EINVAL;
        }
        break;
    }
    case LMAL_SELECTOR_SYNCOBJ_CREATE:
        if (r->sync_create.flags & ~LMAL_SYNCOBJ_CREATE_SIGNALED) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_SYNCOBJ_DESTROY:
        if (r->sync_handle.reserved) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_SYNCOBJ_RESET:
    case LMAL_SELECTOR_SYNCOBJ_SIGNAL:
        if (r->sync_array.reserved || !r->sync_array.count ||
            r->sync_array.count > LMAL_MAX_SYNCOBJS || !r->sync_array.handles) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_SYNCOBJ_WAIT:
        if (r->sync_wait.reserved1 || !r->sync_wait.count ||
            r->sync_wait.count > LMAL_MAX_SYNCOBJS || !r->sync_wait.handles ||
            (r->sync_wait.flags & ~(LMAL_SYNC_WAIT_ALL | LMAL_SYNC_WAIT_FOR_SUBMIT))) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_GET_CAPS:
        if (r->caps.reserved || r->caps.flags) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_SURFACE_IMPORT:
        if (r->surface_import.reserved || !r->surface_import.surface_id ||
            !lmal_client_bo_flags_valid(r->surface_import.flags, 1)) return LMAL_EINVAL;
        break;
    case LMAL_SELECTOR_SURFACE_QUERY:
        if (!r->surface_query.surface_id) return LMAL_EINVAL;
        break;
    default: break;
    }
    return LMAL_OK;
}
/* Exact copy; oversized output does not change negotiated reply length. */
static inline int lmal_client_copy_reply(void *output, size_t capacity,
    const union lmal_request *r, uint32_t selector, int result, size_t *written)
{
    size_t exact = lmal_client_message_size(selector), i;
    *written = 0;
    if (!exact || !output || !r || capacity < exact) return LMAL_EFAULT;
    for (i = 0; i < exact; ++i) ((unsigned char *)output)[i] = ((const unsigned char *)r)[i];
    {
        int32_t status = (int32_t)result;
        for (i = 0; i < sizeof(status); ++i)
            ((unsigned char *)output)[offsetof(struct lmal_header, result) + i] =
                ((const unsigned char *)&status)[i];
    }
    *written = exact;
    return result;
}
struct lmal_sync {
    uint64_t generation;
    uint32_t in_flight;
    uint32_t wait_pins;
    int used;
    int signaled;
};
static inline int lmal_client_sync_create(struct lmal_sync *s, int signaled)
{
    if (s->used || s->generation == UINT64_MAX) return LMAL_EBUSY;
    ++s->generation;
    s->used = 1;
    s->signaled = signaled;
    s->in_flight = s->wait_pins = 0;
    return LMAL_OK;
}
static inline int lmal_client_sync_destroy(struct lmal_sync *s)
{
    if (!s->used) return LMAL_EINVAL;
    if (s->in_flight || s->wait_pins) return LMAL_EBUSY;
    s->used = s->signaled = 0; /* generation survives slot reuse */
    return LMAL_OK;
}
static inline void lmal_client_wait_unpin(struct lmal_sync *syncs, const uint32_t *handles,
    const uint64_t *generations, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        struct lmal_sync *s = &syncs[handles[i] - 1];
        if (s->generation == generations[i] && s->wait_pins) --s->wait_pins;
    }
}
static inline int lmal_client_wait_pin(struct lmal_sync *syncs, uint32_t maximum,
    const uint32_t *handles, uint64_t *generations, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        struct lmal_sync *s;
        if (!handles[i] || handles[i] > maximum) break;
        s = &syncs[handles[i] - 1];
        if (!s->used || s->wait_pins == UINT32_MAX) break;
        generations[i] = s->generation;
        ++s->wait_pins;
    }
    if (i == count) return LMAL_OK;
    lmal_client_wait_unpin(syncs, handles, generations, i);
    return LMAL_EINVAL;
}
/* Under client lock, including immediately after every unlocked sleep. */
static inline int lmal_client_wait_poll(const struct lmal_sync *syncs, const uint32_t *handles,
    const uint64_t *generations, uint32_t count, int wait_all, int closing, uint32_t *first)
{
    uint32_t i, signaled = 0;
    *first = UINT32_MAX;
    if (closing) return LMAL_ENODEV;
    for (i = 0; i < count; ++i) {
        const struct lmal_sync *s = &syncs[handles[i] - 1];
        if (!s->used || s->generation != generations[i]) return LMAL_EINVAL;
        if (s->signaled) {
            if (!signaled) *first = i;
            ++signaled;
        }
    }
    return (wait_all ? signaled == count : signaled != 0) ? LMAL_OK : LMAL_EBUSY;
}
/* Guarded by export lock; makeMapping never acquires user-client lock. */
struct lmal_client_export {
    void *maps[LMAL_CLIENT_EXPORT_MAPS];
    uint32_t count;
    int closed;
    int poisoned;
};
static inline int lmal_client_export_track(struct lmal_client_export *e, void *map)
{
    uint32_t i;
    for (i = 0; i < e->count; ++i) if (e->maps[i] == map) return 0;
    if (e->closed || e->poisoned || e->count == LMAL_CLIENT_EXPORT_MAPS) return -1;
    e->maps[e->count++] = map;
    return 1; /* caller takes real mapping retain only on insertion */
}
static inline int lmal_client_export_revoke(struct lmal_client_export *e, int permanent,
    int (*unmap)(void *), void (*release)(void *))
{
    uint32_t i;
    int failed = e->poisoned;
    e->closed = 1; /* close creation gate BEFORE looking at maps */
    for (i = 0; i < e->count; ++i) if (unmap(e->maps[i]) != LMAL_OK) failed = 1;
    if (failed) {
        e->poisoned = 1;
        return LMAL_EIO; /* no release or reopening, even on later retries */
    }
    for (i = 0; i < e->count; ++i) {
        release(e->maps[i]);
        e->maps[i] = NULL;
    }
    e->count = 0;
    if (!permanent) e->closed = 0;
    return LMAL_OK;
}
/* Kernel-only atomic-pair policy. No selector/capability is published by this
 * header. A real adapter must derive alias/CPU leases, materialize arrays in
 * the originating task, and retain task/client/VM/BO/fence objects BEFORE
 * enqueue. Copied handles alone do not satisfy this contract. All policy and
 * lifetime mutations use one GLOBAL gate; there is only one worker lease. */
#define LMAL_PAIR_QUEUE_DEPTH 8u
#define LMAL_PAIR_STAGE_BO_LIMIT 256u /* larger/unknown worksets stay legacy */
#define LMAL_PAIR_METADATA_BUDGET (UINT64_C(8) * 1024u * 1024u)
#define LMAL_PAIR_CANCELLED (-125)
#define LMAL_PAIR_NO_SLOT UINT32_MAX
#define LMAL_PAIR_LEGACY_SLOT LMAL_PAIR_QUEUE_DEPTH
#define LMAL_PAIR_CORE_RANGE_BYTES 16u /* checked against core by CPP dispatch */
struct lm_range;
struct lmal_pair_range_snapshot { uint64_t va; uint32_t pages, flags; };
static inline void lmal_pair_range_read(const struct lm_range *ranges, uint32_t index,
    struct lmal_pair_range_snapshot *out)
{
    size_t i;
    const unsigned char *bytes = (const unsigned char *)ranges +
        (size_t)index * LMAL_PAIR_CORE_RANGE_BYTES;
    /* Metadata memcpy, not GPU/CPU pixel readback. Avoid incompatible C type
     * aliasing; the actual core layout is statically checked in CPP dispatch. */
    for (i = 0; i < sizeof(*out); ++i) ((unsigned char *)out)[i] = bytes[i];
}
enum lmal_pair_state {
    LMAL_PAIR_EMPTY, LMAL_PAIR_QUEUED, LMAL_PAIR_RUNNING, LMAL_PAIR_RETIRING,
    LMAL_PAIR_COMPLETE, LMAL_PAIR_FAILED, LMAL_PAIR_CANCELLED_STATE,
    LMAL_PAIR_QUARANTINED
};
struct lmal_pair_lifetime {
    uint64_t generation;
    uint32_t pins, wait_pins;
    int used, closing, quarantined, error;
};
struct lmal_pair_fence {
    struct lmal_pair_lifetime life;
    uintptr_t client;
    uint64_t producer_token;
    int signaled, error;
};
struct lmal_pair_bo_snapshot {
    struct lmal_pair_lifetime *life;
    uint64_t generation, va, bytes;
    uintptr_t object, backing_identity, client, vm;
    uint32_t flags, readonly;
    /* Trusted backend observations, never user-supplied permission hints. */
    int cpu_aliases_excluded, external_aliases_excluded, lease_retained;
};
struct lmal_pair_fence_snapshot {
    struct lmal_pair_fence *fence;
    uint64_t generation;
};
struct lmal_pair_stage_snapshot {
    uint64_t jc;
    uint32_t requirements, bo_count;
    struct lmal_pair_bo_snapshot *bos; /* materialized kernel metadata */
    const struct lm_range *ranges; /* owned immutable facade arrays */
};
struct lmal_pair_identity {
    uintptr_t source_task, client, vm;
    int32_t source_pid;
    struct lmal_pair_lifetime *owner;
    uint64_t generation;
};
struct lmal_pair_payload {
    uintptr_t source_task, client, vm;
    int32_t source_pid;
    const struct lmal_pair_identity *identity; /* retained client authority */
    struct lmal_pair_lifetime *owner;
    uint64_t owner_generation;
    struct lmal_pair_stage_snapshot stage[2];
    struct lmal_pair_fence_snapshot *inputs;
    uint32_t input_count;
    struct lmal_pair_fence_snapshot output;
    uint64_t metadata_bytes, deadline_ns, pair_token;
    int authorized, kernel_arrays_owned, objects_retained, pins_applied;
};
struct lmal_pair_execution {
    uint64_t sequence[2];
    uint64_t started_ns[2], completed_ns[2]; /* actual core API dispatch, not enqueue */
    uint32_t attempted_mask, completed_mask, job_status[2];
    int error[2], dma_safe, offline;
};
struct lmal_pair_record {
    uint64_t token, accepted_ns, started_ns, done_ns;
    uintptr_t client, source_task;
    int32_t source_pid;
    enum lmal_pair_state state;
    int result, cancel_error;
    struct lmal_pair_execution execution;
    struct lmal_pair_payload *payload;
};
struct lmal_pair_queue {
    struct lmal_pair_record slots[LMAL_PAIR_QUEUE_DEPTH];
    uint64_t next_token, metadata_bytes, legacy_cookie;
    uint32_t running;
    int offline;
};
struct lmal_pair_worker_ops {
    void *cookie;
    void (*lock)(void *);
    void (*unlock)(void *);
    uint64_t (*now_ns)(void *);
    /* One callback, one global facade lease across VTC AND FS. It must never
     * copyin a user address or use worker current_task/PID as the submitter. */
    int (*execute)(void *, const struct lmal_pair_payload *, uint64_t remaining_ns,
        struct lmal_pair_execution *);
    /* Called outside queue gate. On failure/unsafe DMA the adapter must retain
     * all backing and leases (quarantine). It may not partially release then
     * return an error. A successful retire returns only after real ownership
     * transitions/unpins and final-fence error/success publication complete. */
    int (*retire)(void *, struct lmal_pair_payload *, int result, int dma_safe);
};
static inline void lmal_pair_queue_init(struct lmal_pair_queue *q)
{
    size_t i;
    for (i = 0; i < sizeof(*q); ++i) ((unsigned char *)q)[i] = 0;
    q->running = LMAL_PAIR_NO_SLOT;
}
static inline int lmal_pair_life_pin(struct lmal_pair_lifetime *s, uint64_t generation)
{
    if (!s || !s->used || !generation || s->generation != generation) return LMAL_EINVAL;
    if (s->closing || s->quarantined) return LMAL_ENODEV;
    if (s->error) return s->error;
    if (s->pins == UINT32_MAX) return LMAL_EBUSY;
    ++s->pins;
    return LMAL_OK;
}
static inline int lmal_pair_life_unpin(struct lmal_pair_lifetime *s, uint64_t generation)
{
    if (!s || s->generation != generation || !s->pins) return LMAL_EIO;
    --s->pins;
    return LMAL_OK;
}
static inline int lmal_pair_cpu_admit(const struct lmal_pair_lifetime *s, uint64_t generation)
{
    if (!s || !s->used || s->generation != generation) return LMAL_EINVAL;
    if (s->quarantined || s->closing) return LMAL_ENODEV;
    if (s->error) return s->error;
    return s->pins ? LMAL_EBUSY : LMAL_OK;
}
static inline int lmal_pair_close_admit(struct lmal_pair_lifetime *s)
{
    if (!s || !s->used) return LMAL_EINVAL;
    s->closing = 1; /* close the creation/enqueue gate, do not poison healthy DMA */
    if (s->quarantined) return LMAL_EIO;
    return s->pins || s->wait_pins ? LMAL_EBUSY : LMAL_OK;
}
static inline int lmal_pair_fence_poll(const struct lmal_pair_fence *s, uint64_t generation)
{
    if (!s || !s->life.used || s->life.generation != generation) return LMAL_EINVAL;
    if (s->error) return s->error; /* failed completion is never signaled success */
    return s->signaled ? LMAL_OK : LMAL_EBUSY;
}
static inline int lmal_pair_payload_validate(const struct lmal_pair_payload *p,
    uint64_t now_ns, int queued)
{
    uint32_t stage, i, j;
    uint64_t metadata = sizeof(*p);
    if (!p || !p->identity || !p->source_task || !p->client || !p->vm || p->source_pid <= 0 ||
        p->identity->source_task != p->source_task || p->identity->client != p->client ||
        p->identity->vm != p->vm || p->identity->source_pid != p->source_pid ||
        p->identity->owner != p->owner || p->identity->generation != p->owner_generation ||
        !p->authorized || !p->kernel_arrays_owned || !p->objects_retained ||
        p->pins_applied != queued || (!queued && p->pair_token) || (queued && !p->pair_token) ||
        !p->owner || p->owner->generation != p->owner_generation ||
        !p->owner->used || p->owner->closing || p->owner->quarantined ||
        p->owner->error || !now_ns || p->deadline_ns <= now_ns ||
        p->deadline_ns - now_ns > LMAL_MAX_WAIT_NS ||
        p->input_count > LMAL_MAX_SUBMIT_SYNCS ||
        (p->input_count && !p->inputs)) return LMAL_EINVAL;
    for (stage = 0; stage < 2; ++stage) {
        const struct lmal_pair_stage_snapshot *s = &p->stage[stage];
        int jc_found = 0;
        if (!s->jc || (s->jc & 63u) || s->requirements != stage ||
            !s->bo_count || s->bo_count > LMAL_MAX_SUBMIT_BOS || !s->bos || !s->ranges)
            return LMAL_EINVAL;
        if (s->bo_count > LMAL_PAIR_STAGE_BO_LIMIT) return LMAL_ENOTSUP;
        metadata += (uint64_t)s->bo_count * (sizeof(*s->bos) + LMAL_PAIR_CORE_RANGE_BYTES);
        for (i = 0; i < s->bo_count; ++i) {
            const struct lmal_pair_bo_snapshot *b = &s->bos[i];
            struct lmal_pair_range_snapshot range;
            if (!b->life || !b->life->used || !b->generation ||
                b->life->generation != b->generation || b->life->closing ||
                b->life->quarantined || b->life->error || !b->object ||
                !b->backing_identity || b->client != p->client || b->vm != p->vm ||
                !b->lease_retained ||
                !b->cpu_aliases_excluded || !b->external_aliases_excluded)
                return LMAL_ENOTSUP;
            if (!b->va || !b->bytes || (b->va & (LMAL_PAGE_SIZE - 1)) ||
                (b->bytes & (LMAL_PAGE_SIZE - 1)) || b->bytes > LMAL_MAX_BO_BYTES ||
                b->va > UINT64_MAX - b->bytes ||
                !lmal_client_bo_flags_valid(b->flags, 0) || b->readonly > 1)
                return LMAL_EINVAL;
            lmal_pair_range_read(s->ranges, i, &range);
            if (range.va != b->va || range.pages != b->bytes / LMAL_PAGE_SIZE ||
                range.flags != (b->readonly ? 2u : 0u)) return LMAL_EINVAL;
            if (s->jc >= b->va && s->jc - b->va <= b->bytes - 64 &&
                (b->flags & LMAL_BO_WRITE) && !b->readonly) jc_found = 1;
            for (j = 0; j < i; ++j) {
                const struct lmal_pair_bo_snapshot *a = &s->bos[j];
                if (a->life == b->life) {
                    if (a->generation != b->generation || a->va != b->va ||
                        a->bytes != b->bytes || a->object != b->object ||
                        a->flags != b->flags || a->backing_identity != b->backing_identity)
                        return LMAL_EINVAL;
                } else if (a->backing_identity == b->backing_identity ||
                    (a->va < b->va + b->bytes && b->va < a->va + a->bytes))
                    return LMAL_ENOTSUP;
            }
        }
        if (!jc_found) return LMAL_EINVAL;
    }
    /* Both worksets describe one retained VM. Cross-stage alias/mapping
     * contradictions are rejected too, not hidden by separate array copies. */
    for (i = 0; i < p->stage[0].bo_count; ++i)
        for (j = 0; j < p->stage[1].bo_count; ++j) {
            const struct lmal_pair_bo_snapshot *a = &p->stage[0].bos[i];
            const struct lmal_pair_bo_snapshot *b = &p->stage[1].bos[j];
            if (a->life == b->life) {
                if (a->generation != b->generation || a->va != b->va ||
                    a->bytes != b->bytes || a->object != b->object ||
                    a->flags != b->flags || a->backing_identity != b->backing_identity)
                    return LMAL_EINVAL;
            } else if (a->backing_identity == b->backing_identity ||
                (a->va < b->va + b->bytes && b->va < a->va + a->bytes))
                return LMAL_ENOTSUP;
        }
    metadata += (uint64_t)p->input_count * sizeof(*p->inputs);
    if (p->metadata_bytes != metadata || metadata > LMAL_PAIR_METADATA_BUDGET)
        return LMAL_EINVAL;
    for (i = 0; i < p->input_count; ++i) {
        const struct lmal_pair_fence_snapshot *f = &p->inputs[i];
        if (!f->fence || f->fence->client != p->client || !f->fence->life.used ||
            f->fence->life.generation != f->generation || f->fence->life.closing ||
            f->fence->life.quarantined || f->fence == p->output.fence)
            return LMAL_EINVAL;
        if (f->fence->error) return f->fence->error;
    }
    if (p->output.fence && (p->output.fence->client != p->client || !p->output.fence->life.used ||
        p->output.fence->life.generation != p->output.generation ||
        p->output.fence->life.closing || p->output.fence->life.quarantined ||
        (!queued && p->output.fence->life.pins))) return LMAL_EBUSY;
    return LMAL_OK;
}
/* Transactional logical pins, in addition to the adapter's actual OS retains.
 * Duplicates retain independent pins; GPU readonly duplicates are a separate
 * permission-resolution policy, never an excuse to drop lifetime pins. */
static inline int lmal_pair_payload_pin(struct lmal_pair_payload *p)
{
    uint32_t stage, i, counts[2] = {0,0}, inputs = 0;
    int result;
    if (!p || p->pins_applied) return LMAL_EINVAL;
    result = lmal_pair_life_pin(p->owner, p->owner_generation);
    if (result) return result;
    for (stage = 0; stage < 2; ++stage)
        for (i = 0; i < p->stage[stage].bo_count; ++i) {
            struct lmal_pair_bo_snapshot *b = &p->stage[stage].bos[i];
            result = lmal_pair_life_pin(b->life, b->generation);
            if (result) goto rollback;
            ++counts[stage];
        }
    for (i = 0; i < p->input_count; ++i) {
        result = lmal_pair_life_pin(&p->inputs[i].fence->life, p->inputs[i].generation);
        if (result) goto rollback;
        ++inputs;
    }
    if (p->output.fence) {
        result = lmal_pair_life_pin(&p->output.fence->life, p->output.generation);
        if (result) goto rollback;
    }
    p->pins_applied = 1;
    return LMAL_OK;
rollback:
    for (i = 0; i < inputs; ++i)
        (void)lmal_pair_life_unpin(&p->inputs[i].fence->life, p->inputs[i].generation);
    for (stage = 0; stage < 2; ++stage)
        for (i = 0; i < counts[stage]; ++i) {
            struct lmal_pair_bo_snapshot *b = &p->stage[stage].bos[i];
            (void)lmal_pair_life_unpin(b->life, b->generation);
        }
    (void)lmal_pair_life_unpin(p->owner, p->owner_generation);
    return result;
}
static inline int lmal_pair_payload_unpin(struct lmal_pair_payload *p,
    int result, int dma_safe)
{
    uint32_t stage, i, other_stage, j;
    if (!p || !p->pins_applied || !p->owner ||
        p->owner->generation != p->owner_generation || !p->owner->pins)
        return LMAL_EIO;
    /* Preflight multiplicity/generation before any pin drops. A corrupt
     * adapter cannot partially release backing on an underflow/stale slot. */
    for (stage = 0; stage < 2; ++stage)
        for (i = 0; i < p->stage[stage].bo_count; ++i) {
            struct lmal_pair_bo_snapshot *b = &p->stage[stage].bos[i];
            uint32_t needed = 0;
            if (!b->life || b->life->generation != b->generation) return LMAL_EIO;
            for (other_stage = 0; other_stage < 2; ++other_stage)
                for (j = 0; j < p->stage[other_stage].bo_count; ++j)
                    needed += p->stage[other_stage].bos[j].life == b->life;
            if (b->life->pins < needed) return LMAL_EIO;
        }
    for (i = 0; i < p->input_count; ++i) {
        uint32_t needed = 0;
        if (p->inputs[i].fence->life.generation != p->inputs[i].generation)
            return LMAL_EIO;
        for (j = 0; j < p->input_count; ++j)
            needed += p->inputs[i].fence == p->inputs[j].fence;
        if (p->inputs[i].fence->life.pins < needed) return LMAL_EIO;
    }
    if (p->output.fence && (p->output.fence->life.generation != p->output.generation ||
        !p->output.fence->life.pins)) return LMAL_EIO;
    if (!dma_safe) {
        p->owner->quarantined = 1;
        for (stage = 0; stage < 2; ++stage)
            for (i = 0; i < p->stage[stage].bo_count; ++i)
                p->stage[stage].bos[i].life->quarantined = 1;
        if (p->output.fence) {
            p->output.fence->life.quarantined = 1;
            p->output.fence->error = result ? result : LMAL_EIO;
            p->output.fence->signaled = 0;
        }
        return LMAL_EBUSY; /* all actual references/leases remain held */
    }
    for (stage = 0; stage < 2; ++stage)
        for (i = 0; i < p->stage[stage].bo_count; ++i) {
            struct lmal_pair_bo_snapshot *b = &p->stage[stage].bos[i];
            (void)lmal_pair_life_unpin(b->life, b->generation);
            if (result && !b->life->error) b->life->error = result;
        }
    for (i = 0; i < p->input_count; ++i)
        (void)lmal_pair_life_unpin(&p->inputs[i].fence->life, p->inputs[i].generation);
    if (p->output.fence) {
        (void)lmal_pair_life_unpin(&p->output.fence->life, p->output.generation);
        p->output.fence->error = result;
        p->output.fence->signaled = result == LMAL_OK;
    }
    (void)lmal_pair_life_unpin(p->owner, p->owner_generation);
    p->pins_applied = 0;
    return LMAL_OK;
}
static inline struct lmal_pair_record *lmal_pair_find(struct lmal_pair_queue *q,
    uint64_t token)
{
    uint32_t i;
    if (!token) return NULL;
    for (i = 0; i < LMAL_PAIR_QUEUE_DEPTH; ++i)
        if (q->slots[i].state != LMAL_PAIR_EMPTY && q->slots[i].token == token)
            return &q->slots[i];
    return NULL;
}
/* Queue takes a prepared payload only on success. On any pre-accept failure
 * caller still owns it and rolls back its real retains/CPU leases. */
static inline int lmal_pair_enqueue(struct lmal_pair_queue *q,
    struct lmal_pair_payload *p, uint64_t now_ns, uint64_t *token)
{
    uint32_t i, free_slot = LMAL_PAIR_NO_SLOT;
    int result;
    if (!q || !token) return LMAL_EINVAL;
    *token = 0;
    if (q->offline) return LMAL_ENODEV;
    result = lmal_pair_payload_validate(p, now_ns, 0);
    if (result) return result;
    if (q->metadata_bytes > LMAL_PAIR_METADATA_BUDGET) return LMAL_EIO;
    if (q->next_token == UINT64_MAX || p->metadata_bytes >
        LMAL_PAIR_METADATA_BUDGET - q->metadata_bytes) return LMAL_EBUSY;
    for (i = 0; i < LMAL_PAIR_QUEUE_DEPTH; ++i)
        if (q->slots[i].state == LMAL_PAIR_EMPTY) { free_slot = i; break; }
    if (free_slot == LMAL_PAIR_NO_SLOT) return LMAL_EBUSY;
    for (i = 0; i < p->input_count; ++i) {
        struct lmal_pair_fence *f = p->inputs[i].fence;
        struct lmal_pair_record *predecessor;
        if (f->signaled) continue;
        predecessor = lmal_pair_find(q, f->producer_token);
        if (!predecessor || predecessor->token > q->next_token ||
            predecessor->state == LMAL_PAIR_FAILED ||
            predecessor->state == LMAL_PAIR_CANCELLED_STATE ||
            predecessor->state == LMAL_PAIR_QUARANTINED) return LMAL_EBUSY;
        if (predecessor->state == LMAL_PAIR_COMPLETE) return LMAL_EINVAL;
    }
    result = lmal_pair_payload_pin(p);
    if (result) return result;
    {
        struct lmal_pair_record *r = &q->slots[free_slot];
        size_t n;
        for (n = 0; n < sizeof(*r); ++n) ((unsigned char *)r)[n] = 0;
        r->token = ++q->next_token;
        p->pair_token = r->token;
        r->accepted_ns = now_ns;
        r->client = p->client; r->source_task = p->source_task; r->source_pid = p->source_pid;
        r->payload = p; r->state = LMAL_PAIR_QUEUED;
        q->metadata_bytes += p->metadata_bytes;
        if (p->output.fence) {
            p->output.fence->producer_token = r->token;
            p->output.fence->signaled = p->output.fence->error = 0;
        }
        *token = r->token;
    }
    return LMAL_OK;
}
static inline int lmal_pair_query(struct lmal_pair_queue *q, uintptr_t client,
    uint64_t token, struct lmal_pair_record *out)
{
    struct lmal_pair_record *r = lmal_pair_find(q, token);
    if (!r || !out) return LMAL_EINVAL;
    if (!client || r->client != client) return LMAL_EPERM;
    *out = *r;
    out->payload = NULL;
    out->client = out->source_task = 0; /* no native kernel pointer in status */
    return LMAL_OK;
}
static inline int lmal_pair_ack(struct lmal_pair_queue *q, uintptr_t client, uint64_t token)
{
    struct lmal_pair_record *r = lmal_pair_find(q, token);
    size_t n;
    if (!r) return LMAL_EINVAL;
    if (!client || r->client != client) return LMAL_EPERM;
    if (r->payload || r->state == LMAL_PAIR_QUEUED || r->state == LMAL_PAIR_RUNNING ||
        r->state == LMAL_PAIR_RETIRING || r->state == LMAL_PAIR_QUARANTINED)
        return LMAL_EBUSY;
    for (n = 0; n < sizeof(*r); ++n) ((unsigned char *)r)[n] = 0;
    return LMAL_OK;
}
static inline void lmal_pair_cancel_owner(struct lmal_pair_queue *q, uintptr_t client)
{
    uint32_t i;
    for (i = 0; i < LMAL_PAIR_QUEUE_DEPTH; ++i)
        if (q->slots[i].state == LMAL_PAIR_QUEUED && q->slots[i].client == client)
            q->slots[i].cancel_error = LMAL_PAIR_CANCELLED;
    /* Running DMA is never claimed canceled or unpinned here. */
}
static inline void lmal_pair_offline(struct lmal_pair_queue *q)
{
    q->offline = 1; /* sticky; no queue-only reset/revival operation */
}
/* Legacy synchronous work also takes the GLOBAL queue execution lease. It
 * cannot overtake queued pairs or enter between a pair's VTC/FS callbacks. */
static inline int lmal_pair_legacy_enter(struct lmal_pair_queue *q, uint64_t *cookie)
{
    uint32_t i;
    if (!q || !cookie) return LMAL_EINVAL;
    *cookie = 0;
    if (q->offline) return LMAL_ENODEV;
    if (q->running != LMAL_PAIR_NO_SLOT) return LMAL_EBUSY;
    for (i = 0; i < LMAL_PAIR_QUEUE_DEPTH; ++i)
        if (q->slots[i].state == LMAL_PAIR_QUEUED) return LMAL_EBUSY;
    if (q->legacy_cookie == UINT64_MAX) return LMAL_EBUSY;
    *cookie = ++q->legacy_cookie;
    q->running = LMAL_PAIR_LEGACY_SLOT;
    return LMAL_OK;
}
static inline int lmal_pair_legacy_leave(struct lmal_pair_queue *q, uint64_t cookie)
{
    if (!q || q->running != LMAL_PAIR_LEGACY_SLOT || !cookie ||
        cookie != q->legacy_cookie) return LMAL_EINVAL;
    q->running = LMAL_PAIR_NO_SLOT;
    return LMAL_OK;
}
/* One worker step; no allocation, unbounded poll or user copy occurs here.
 * OS worker adapter sleeps on enqueue notifications between steps. The gate
 * is released across GPU wait so CPU preparation can overlap with execution. */
static inline int lmal_pair_worker_step(struct lmal_pair_queue *q,
    const struct lmal_pair_worker_ops *ops)
{
    uint32_t i, best = LMAL_PAIR_NO_SLOT;
    uint64_t now, started = 0, bytes;
    int result = LMAL_OK, retired, dma_safe = 1;
    struct lmal_pair_record *r;
    struct lmal_pair_payload *p;
    struct lmal_pair_execution execution;
    {
        size_t n;
        for (n = 0; n < sizeof(execution); ++n) ((unsigned char *)&execution)[n] = 0;
        execution.dma_safe = 1;
    }
    if (!q || !ops || !ops->lock || !ops->unlock || !ops->now_ns ||
        !ops->execute || !ops->retire) return LMAL_EINVAL;
    ops->lock(ops->cookie);
    if (q->running != LMAL_PAIR_NO_SLOT) {
        ops->unlock(ops->cookie); return LMAL_EBUSY;
    }
    for (i = 0; i < LMAL_PAIR_QUEUE_DEPTH; ++i)
        if (q->slots[i].state == LMAL_PAIR_QUEUED &&
            (best == LMAL_PAIR_NO_SLOT || q->slots[i].token < q->slots[best].token)) best = i;
    if (best == LMAL_PAIR_NO_SLOT) {
        ops->unlock(ops->cookie); return LMAL_EBUSY;
    }
    r = &q->slots[best]; p = r->payload;
    q->running = best; r->state = LMAL_PAIR_RUNNING;
    now = ops->now_ns(ops->cookie);
    if (r->cancel_error) result = r->cancel_error;
    else if (q->offline) result = LMAL_ENODEV;
    else if (now >= p->deadline_ns) result = LMAL_ETIMEDOUT;
    else {
        result = lmal_pair_payload_validate(p, now, 1);
        for (i = 0; !result && i < p->input_count; ++i)
            result = lmal_pair_fence_poll(p->inputs[i].fence, p->inputs[i].generation);
    }
    if (!result) {
        /* Metadata validation itself consumes budget too. Do not hand the
         * facade a timeout measured before a potentially expensive workset. */
        now = ops->now_ns(ops->cookie);
        if (now >= p->deadline_ns) result = LMAL_ETIMEDOUT;
        else { started = now; r->started_ns = started; }
    }
    ops->unlock(ops->cookie);
    if (!result) {
        result = ops->execute(ops->cookie, p, p->deadline_ns - started, &execution);
        /* Reject impossible success/sequence reports; fail closed, never turn
         * accepted token into a fabricated hardware sequence or completed fence. */
        if ((execution.attempted_mask & ~3u) || (execution.completed_mask &
            ~execution.attempted_mask) ||
            (execution.sequence[0] && !(execution.attempted_mask & 1u)) ||
            (execution.sequence[1] && !(execution.attempted_mask & 2u)) ||
            ((execution.attempted_mask & 2u) && (!(execution.completed_mask & 1u) ||
             execution.error[0])) ||
            (!result && (execution.completed_mask != 3u || execution.error[0] ||
             execution.error[1] || !execution.sequence[0] || !execution.sequence[1]))) {
            result = LMAL_EIO; execution.dma_safe = 0; execution.offline = 1;
        }
        dma_safe = execution.dma_safe;
    }
    ops->lock(ops->cookie);
    r->state = LMAL_PAIR_RETIRING;
    if (execution.offline || !dma_safe) q->offline = 1;
    bytes = p->metadata_bytes;
    ops->unlock(ops->cookie);
    retired = ops->retire(ops->cookie, p, result, dma_safe);
    ops->lock(ops->cookie);
    r->execution = execution;
    r->done_ns = ops->now_ns(ops->cookie);
    r->result = result;
    if (retired || !dma_safe) {
        r->result = result ? result : LMAL_EIO;
        r->state = LMAL_PAIR_QUARANTINED;
        q->offline = 1; /* payload/leases remain owned; ACK cannot free them */
    } else {
        r->payload = NULL;
        q->metadata_bytes -= bytes;
        r->state = result == LMAL_PAIR_CANCELLED ? LMAL_PAIR_CANCELLED_STATE :
            result ? LMAL_PAIR_FAILED : LMAL_PAIR_COMPLETE;
    }
    q->running = LMAL_PAIR_NO_SLOT;
    result = r->result; /* ACK may recycle the slot immediately after unlock. */
    ops->unlock(ops->cookie);
    return result;
}
#endif
