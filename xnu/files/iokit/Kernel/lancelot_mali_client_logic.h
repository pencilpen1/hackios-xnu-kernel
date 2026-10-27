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
    case LMAL_SELECTOR_SUBMIT:
        if (r->submit.reserved || (r->submit.requirements & ~LMAL_SUBMIT_REQUIRE_COMPLETE) ||
            !r->submit.bo_count || r->submit.bo_count > LMAL_MAX_SUBMIT_BOS ||
            !r->submit.bo_handles || r->submit.in_sync_count > LMAL_MAX_SUBMIT_SYNCS ||
            (r->submit.in_sync_count && !r->submit.in_syncs) ||
            (!r->submit.in_sync_count && r->submit.in_syncs) ||
            !r->submit.timeout_ns || r->submit.timeout_ns > LMAL_MAX_WAIT_NS || !r->submit.jc ||
            (r->submit.jc & 63u)) return LMAL_EINVAL;
        break;
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
#endif
