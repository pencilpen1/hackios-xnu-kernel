/*
 * Lancelot Mali user-client ABI, version 1.
 *
 * This is deliberately a private, versioned protocol.  Its selectors do not
 * overlap the Redwood/MSM protocol: the service name and selector namespace
 * are both Lancelot-specific.  Do not add native pointers to these structs;
 * user addresses are uint64_t and are copied with copyin/copyout by the
 * kernel client.
 */
#ifndef LANCELOT_MALI_UAPI_H
#define LANCELOT_MALI_UAPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LMAL_SERVICE_NAME             "LancelotMali"
#define LMAL_UAPI_MAGIC               UINT32_C(0x4c4d414c) /* "LMAL" */
#define LMAL_UAPI_VERSION             UINT16_C(1)
#define LMAL_OPEN_TYPE                UINT32_C(0x4c4d0001) /* independent IOServiceOpen type */
#define LMAL_SELECTOR_BASE            UINT32_C(0x4c4d0000)
#define LMAL_MAX_STRUCT_SIZE          UINT32_C(4096)
#define LMAL_PAGE_SIZE                UINT64_C(4096)
#define LMAL_MAX_BO_BYTES             UINT64_C(0x08000000) /* 128 MiB */
#define LMAL_MAX_VM_BYTES             UINT64_C(0x10000000) /* 256 MiB */
#define LMAL_MAX_SUBMIT_BOS           UINT32_C(8192)
#define LMAL_MAX_SUBMIT_SYNCS         UINT32_C(256)
#define LMAL_MAX_SYNCOBJS             UINT32_C(4096)
#define LMAL_MAX_WAIT_NS              UINT64_C(5000000000) /* 5 seconds */

/* Kernel return values are also exposed in header.result for shim callers. */
#define LMAL_OK                       INT32_C(0)
#define LMAL_EINVAL                   INT32_C(-22)
#define LMAL_ENOMEM                   INT32_C(-12)
#define LMAL_EBUSY                    INT32_C(-16)
#define LMAL_ENODEV                   INT32_C(-19)
#define LMAL_ENOTSUP                  INT32_C(-95)
#define LMAL_ETIMEDOUT                INT32_C(-110)
#define LMAL_EIO                      INT32_C(-5)
#define LMAL_EFAULT                   INT32_C(-14)
#define LMAL_EPERM                   INT32_C(-1)

/* Each request starts with this 24-byte, forward-compatible header. */
struct lmal_header {
    uint32_t magic;
    uint16_t version;
    uint16_t size;          /* bytes supplied by this ABI version */
    uint32_t flags;
    uint32_t reserved0;
    int32_t  result;        /* LMAL_* result, mirrored from IOReturn */
    uint32_t reserved1;
};

/* Selector values are an independent namespace, never MSM/Redwood values. */
enum lmal_selector {
    LMAL_SELECTOR_GET_PARAM       = LMAL_SELECTOR_BASE + 0x01,
    LMAL_SELECTOR_BO_CREATE       = LMAL_SELECTOR_BASE + 0x02,
    LMAL_SELECTOR_BO_QUERY        = LMAL_SELECTOR_BASE + 0x03,
    LMAL_SELECTOR_BO_CLOSE        = LMAL_SELECTOR_BASE + 0x04,
    LMAL_SELECTOR_BO_WAIT         = LMAL_SELECTOR_BASE + 0x05,
    LMAL_SELECTOR_SUBMIT          = LMAL_SELECTOR_BASE + 0x06,
    LMAL_SELECTOR_SYNCOBJ_CREATE  = LMAL_SELECTOR_BASE + 0x07,
    LMAL_SELECTOR_SYNCOBJ_DESTROY = LMAL_SELECTOR_BASE + 0x08,
    LMAL_SELECTOR_SYNCOBJ_RESET   = LMAL_SELECTOR_BASE + 0x09,
    LMAL_SELECTOR_SYNCOBJ_SIGNAL  = LMAL_SELECTOR_BASE + 0x0a,
    LMAL_SELECTOR_SYNCOBJ_WAIT    = LMAL_SELECTOR_BASE + 0x0b,
    LMAL_SELECTOR_GET_CAPS        = LMAL_SELECTOR_BASE + 0x0c,
    LMAL_SELECTOR_SYNC_IMPORT     = LMAL_SELECTOR_BASE + 0x0d,
    LMAL_SELECTOR_SYNC_EXPORT     = LMAL_SELECTOR_BASE + 0x0e,
    LMAL_SELECTOR_SURFACE_QUERY   = LMAL_SELECTOR_BASE + 0x0f,
    LMAL_SELECTOR_SURFACE_IMPORT  = LMAL_SELECTOR_BASE + 0x10,
    LMAL_SELECTOR_BO_UNMAP        = LMAL_SELECTOR_BASE + 0x11,
};

/* BO flags.  HEAP is fixed-size backing in v1; it never grows implicitly. */
#define LMAL_BO_READ                 UINT32_C(0x00000001)
#define LMAL_BO_WRITE                UINT32_C(0x00000002)
#define LMAL_BO_EXEC                 UINT32_C(0x00000004)
#define LMAL_BO_NOEXEC               UINT32_C(0x00000008)
#define LMAL_BO_HEAP                 UINT32_C(0x00000010)
#define LMAL_BO_FIXED_BACKING        UINT32_C(0x00000020) /* accepted input/output capability */
#define LMAL_BO_READONLY             UINT32_C(0x00000040)

/* Values intentionally follow the Linux Panfrost enum used by Mesa 25. */
enum lmal_param {
    LMAL_PARAM_GPU_PROD_ID = 0,
    LMAL_PARAM_GPU_REVISION = 1,
    LMAL_PARAM_SHADER_PRESENT = 2,
    LMAL_PARAM_TILER_PRESENT = 3,
    LMAL_PARAM_L2_PRESENT = 4,
    LMAL_PARAM_STACK_PRESENT = 5,
    LMAL_PARAM_AS_PRESENT = 6,
    LMAL_PARAM_JS_PRESENT = 7,
    LMAL_PARAM_L2_FEATURES = 8,
    LMAL_PARAM_CORE_FEATURES = 9,
    LMAL_PARAM_TILER_FEATURES = 10,
    LMAL_PARAM_MEM_FEATURES = 11,
    LMAL_PARAM_MMU_FEATURES = 12,
    LMAL_PARAM_THREAD_FEATURES = 13,
    LMAL_PARAM_THREAD_MAX_THREADS = 14,
    LMAL_PARAM_THREAD_MAX_WORKGROUP_SZ = 15,
    LMAL_PARAM_THREAD_MAX_BARRIER_SZ = 16,
    LMAL_PARAM_COHERENCY_FEATURES = 17,
    LMAL_PARAM_TEXTURE_FEATURES0 = 18,
    LMAL_PARAM_TEXTURE_FEATURES1 = 19,
    LMAL_PARAM_TEXTURE_FEATURES2 = 20,
    LMAL_PARAM_TEXTURE_FEATURES3 = 21,
    LMAL_PARAM_JS_FEATURES0 = 22,
    LMAL_PARAM_JS_FEATURES1 = 23,
    LMAL_PARAM_JS_FEATURES2 = 24,
    LMAL_PARAM_JS_FEATURES3 = 25,
    LMAL_PARAM_JS_FEATURES4 = 26,
    LMAL_PARAM_JS_FEATURES5 = 27,
    LMAL_PARAM_JS_FEATURES6 = 28,
    LMAL_PARAM_JS_FEATURES7 = 29,
    LMAL_PARAM_JS_FEATURES8 = 30,
    LMAL_PARAM_JS_FEATURES9 = 31,
    LMAL_PARAM_JS_FEATURES10 = 32,
    LMAL_PARAM_JS_FEATURES11 = 33,
    LMAL_PARAM_JS_FEATURES12 = 34,
    LMAL_PARAM_JS_FEATURES13 = 35,
    LMAL_PARAM_JS_FEATURES14 = 36,
    LMAL_PARAM_JS_FEATURES15 = 37,
    LMAL_PARAM_NR_CORE_GROUPS = 38,
    LMAL_PARAM_THREAD_TLS_ALLOC = 39,
    LMAL_PARAM_AFBC_FEATURES = 40,
    LMAL_PARAM_SYSTEM_TIMESTAMP = 41,
    LMAL_PARAM_SYSTEM_TIMESTAMP_FREQUENCY = 42,
    LMAL_PARAM_MAX = 43,
};

/* Query map_type is the client-local BO handle passed to IOConnectMapMemory64. */
enum lmal_bo_map_type {
    LMAL_MAP_TYPE_HANDLE = 0, /* symbolic: the returned map_type value is handle */
};

struct lmal_get_param {
    struct lmal_header h;
    uint32_t param;
    uint32_t reserved;
    uint64_t value;
};

struct lmal_bo_create {
    struct lmal_header h;
    uint64_t size;
    uint32_t flags;
    uint32_t handle;       /* output, client-local, never a global object id */
    uint64_t gpu_va;       /* output, kernel-assigned client-local VA */
    uint64_t actual_size;  /* output, page-rounded fixed backing size */
    uint32_t map_type;     /* output, pass to IOConnectMapMemory64 */
    uint32_t reserved;
};

struct lmal_bo_query {
    struct lmal_header h;
    uint32_t handle;
    uint32_t reserved0;
    uint64_t size;
    uint64_t gpu_va;
    uint32_t flags;
    uint32_t map_type;
    uint32_t map_refs;
    uint32_t reserved1;
};

struct lmal_bo_close {
    struct lmal_header h;
    uint32_t handle;
    uint32_t reserved;
};

#define LMAL_BO_WAIT_READ           UINT32_C(0x00000001)
#define LMAL_BO_WAIT_WRITE          UINT32_C(0x00000002)
struct lmal_bo_wait {
    struct lmal_header h;
    uint32_t handle;
    uint32_t flags;
    uint64_t absolute_monotonic_ns; /* XNU mach_absolute_time converted to ns; Mesa sends converted remaining deadline */
};

/* Submit is synchronous in v1: no raw fence is returned to user space. */
#define LMAL_SUBMIT_REQUIRE_COMPLETE UINT32_C(0x00000001)
struct lmal_submit {
    struct lmal_header h;
    uint64_t jc;                 /* GPU VA of a validated executable job chain */
    uint32_t requirements;
    uint32_t bo_count;
    uint64_t bo_handles;         /* uint32_t[bo_count], user address */
    uint32_t in_sync_count;
    uint32_t out_sync;           /* 0 means no output syncobj */
    uint64_t in_syncs;            /* uint32_t[in_sync_count], user address */
    uint64_t timeout_ns;          /* relative timeout, bounded by LMAL_MAX_WAIT_NS */
    uint64_t sequence;            /* output, monotonically increasing */
    uint32_t job_status;          /* output, core status */
    uint32_t reserved;
};

#define LMAL_SYNCOBJ_CREATE_SIGNALED UINT32_C(0x00000001)
struct lmal_syncobj_create {
    struct lmal_header h;
    uint32_t flags;
    uint32_t handle;
};

struct lmal_syncobj_handle {
    struct lmal_header h;
    uint32_t handle;
    uint32_t reserved;
};

/* RESET and SIGNAL operate on a bounded client-local handle array. */
struct lmal_syncobj_array {
    struct lmal_header h;
    uint64_t handles;            /* uint32_t[count], user address */
    uint32_t count;
    uint32_t reserved;
};

#define LMAL_SYNC_WAIT_ALL          UINT32_C(0x00000001)
#define LMAL_SYNC_WAIT_FOR_SUBMIT   UINT32_C(0x00000002)
struct lmal_syncobj_wait {
    struct lmal_header h;
    uint64_t handles;             /* uint32_t[count], user address */
    uint32_t count;
    uint32_t flags;
    uint64_t absolute_monotonic_ns; /* XNU mach_absolute_time converted to ns; Mesa sends converted remaining deadline */
    uint32_t first_signaled;      /* output: index, UINT32_MAX if none */
    uint32_t reserved1;
};

/* v1 has binary syncobjs only; shared sync-file import/export is explicit ENOTSUP. */
struct lmal_sync_file {
    struct lmal_header h;
    uint32_t handle;
    int32_t  fd;
};

struct lmal_caps {
    struct lmal_header h;
    uint64_t jobs_submitted;
    uint64_t jobs_completed;
    uint64_t faults;
    uint64_t resets;
    uint64_t quarantined;
    uint32_t max_bo_bytes;
    uint32_t max_vm_bytes;
    uint32_t flags;
    uint32_t reserved;
};

/* IOSurface metadata is copied from the kernel-owned MiniIOSurface descriptor. */
struct lmal_surface_query {
    struct lmal_header h;
    uint32_t surface_id;
    uint32_t handle;              /* output: 0 for query-only */
    uint64_t size;                /* pixel backing size, page rounded */
    uint32_t offset;
    uint32_t row_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t planes;
};

struct lmal_surface_import {
    struct lmal_header h;
    uint32_t surface_id;
    uint32_t flags;               /* BO flags; EXEC is rejected for surfaces */
    uint32_t handle;              /* output */
    uint32_t reserved;
    uint64_t gpu_va;              /* output */
    uint64_t size;                /* output */
    uint32_t map_type;            /* output: handle for IOConnectMapMemory64 */
    uint32_t offset;
    uint32_t row_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t planes;
};

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(struct lmal_header) == 24, "LMAL header ABI");
static_assert(sizeof(struct lmal_bo_create) == 64, "LMAL BO create ABI");
static_assert(sizeof(struct lmal_submit) == 88, "LMAL submit ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct lmal_header) == 24, "LMAL header ABI");
_Static_assert(sizeof(struct lmal_bo_create) == 64, "LMAL BO create ABI");
_Static_assert(sizeof(struct lmal_submit) == 88, "LMAL submit ABI");
#endif

#endif /* LANCELOT_MALI_UAPI_H */
