/* SPDX-License-Identifier: APSL-2.0
 * Panfrost DRM layouts and RedwoodMali extensions, matching the MT6768 donor.
 * Shared with the source-built Darwin bridge; all addresses below are VAs.
 */
#ifndef LANCELOT_MALI_QUEUE_UAPI_H
#define LANCELOT_MALI_QUEUE_UAPI_H
#include <stdint.h>
enum {
 LMQ_CLOSE=0x09, LMQ_SUBMIT=0x40, LMQ_WAIT_BO=0x41, LMQ_CREATE=0x42,
 LMQ_MMAP=0x43, LMQ_PARAM=0x44, LMQ_OFFSET=0x45, LMQ_MADVISE=0x48,
 LMQ_SUBMIT_ACCESS=0x7a, LMQ_ACK=0x7b, LMQ_SURFACE_SEQ=0x7c, LMQ_WAIT=0x7d, LMQ_SURFACE=0x7e
};
struct lmq_close { uint32_t handle,pad; };
struct lmq_submit {
 uint64_t jc,in_syncs; uint32_t in_sync_count,out_sync;
 uint64_t bo_handles; uint32_t bo_handle_count,requirements;
};
/* Lancelot extension: copy the parallel 0/1 access array at admission. Raw
 * Panfrost SUBMIT stays conservative; no CPU/GPU ownership hint is implied. */
struct lmq_submit_access { struct lmq_submit submit; uint64_t bo_readonly; };
struct lmq_wait_bo { uint32_t handle,pad; int64_t timeout_ns; };
struct lmq_create { uint32_t size,flags,handle,pad; uint64_t offset; };
struct lmq_mmap { uint32_t handle,flags; uint64_t offset; };
struct lmq_param { uint32_t param,pad; uint64_t value; };
struct lmq_offset { uint32_t handle,pad; uint64_t offset; };
struct lmq_madvise { uint32_t handle,madv,retained; };
struct lmq_wait { uint64_t seq; int64_t timeout_ns; };
struct lmq_surface {
 uint32_t surface,handle; uint64_t size;
 uint32_t offset,rowBytes,width,height,pixelFormat,planes;
};
struct lmq_surface_seq { uint32_t surface,pad; uint64_t seq; };
#define LMQ_DESCRIBE UINT32_MAX
#define LMQ_BO_NOEXEC 1u
#define LMQ_BO_HEAP 2u
#define LMQ_MAX_BOS 4096u
#define LMQ_MAX_BO_BYTES (UINT64_C(256) << 20) /* donor's per-buffer bound */
#define LMQ_MAX_BYTES (128ull << 20)
#define LMQ_FENCE_SLOTS 4096u
#ifdef __cplusplus
static_assert(sizeof(lmq_submit)==40,"Panfrost submit layout");
static_assert(sizeof(lmq_submit_access)==48,"Lancelot queued access layout");
static_assert(sizeof(lmq_surface)==40,"RedwoodMali surface layout");
#endif
#endif
