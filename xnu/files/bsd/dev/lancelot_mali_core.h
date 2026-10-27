/* Lancelot Mali-G52 engine. All callers MUST serialize access to a device/VM.
 * Host tests exercise this same file via callbacks; no Linux or IOKit types.
 * References: kbase r25p0 gpu_regmap, mmu_mode_aarch64 and mmu_hw_direct.
 */
#ifndef LANCELOT_MALI_CORE_H
#define LANCELOT_MALI_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LM_PAGE_SIZE 4096u
#define LM_VA_MIN UINT64_C(0x02000000)
#define LM_VA_END UINT64_C(0x100000000)
#define LM_MAP_WRITE 1u
#define LM_MAP_EXEC 2u
#define LM_MAX_MAP_PAGES 32768u /* 128 MiB BO cap, noncontiguous PA supported */
#define LM_MAX_VM_PAGES 65536u /* 256 MiB total/client */
#define LM_OK 0
#define LM_EINVAL (-22)
#define LM_ENOMEM (-12)
#define LM_EBUSY (-16)
#define LM_ENODEV (-19)
#define LM_ENOTSUP (-95)
#define LM_ETIMEDOUT (-110)
#define LM_EIO (-5)
#define LM_EFAULT (-14)
/* Serialized private job: no priority/preemption request. G52 same-boot
 * receipt proved legacy THREAD_PRI(8) writes read back with that bit zero.
 * Use explicit zero instead of weakening the full register readback proof. */
#define LM_JOB_NEXT_CONFIG UINT32_C(0x00003300) /* AS0, start/end clean+invalidate */
struct lm_page { void *cpu; uint64_t pa; };
struct lm_vm;
struct lm_core;
enum lm_job_stage {
    LM_JOB_NONE, LM_JOB_BIND, LM_JOB_IRQ_CLEAR, LM_JOB_NEXT,
    LM_JOB_WAIT, LM_JOB_CACHE, LM_JOB_DETACH, LM_JOB_ACK
};
enum lm_core_phase {
    LM_PHASE_NONE,LM_PHASE_ID,LM_PHASE_RESET,LM_PHASE_CONFIG,LM_PHASE_L2_ON,
    LM_PHASE_TILER_ON,LM_PHASE_SHADER_ON,LM_PHASE_FEATURES,LM_PHASE_BIND,
    LM_PHASE_NEXT,LM_PHASE_WAIT,LM_PHASE_CACHE,LM_PHASE_DETACH,LM_PHASE_OFF,
    LM_PHASE_FINAL_RESET
};
struct lm_job_result {
    uint64_t sequence, elapsed_ns, fault_address;
    uint32_t gpu_fault, mmu_fault, job_status, job_irq, job_state;
    /* Failure diagnostics: the faulting slot's current head/tail, the AS
     * secondary fault address, and the GPU exception status/address. */
    uint32_t js_head_lo,js_head_hi,js_tail_lo,js_tail_hi;
    uint32_t fault_extra_lo,fault_extra_hi;
    uint32_t gpu_fault_status,gpu_fault_lo,gpu_fault_hi;
    /* First failed readback, before recovery overwrites the registers. */
    uint32_t failed_stage, mismatch_reg;
    uint64_t expected, observed;
    bool reset_completed, quarantined;
};
struct lm_ops {
    void *cookie;
    /* Called only once platform prepare has proved all domains accessible. */
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    uint64_t (*now_ns)(void *);
    void (*delay_us)(void *, uint32_t);
    void *(*alloc)(void *, size_t); /* zeroed metadata */
    void (*free)(void *, void *, size_t);
    int (*page_alloc)(void *, struct lm_page *); /* zeroed 4K normal WB page */
    void (*page_free)(void *, struct lm_page *);
    /* cache ops are synchronous: clean+invalidate before DMA, invalidate after.
     * In particular, register barriers are NOT substitutes for these callbacks. */
    void (*for_device)(void *, void *, size_t);
    void (*for_cpu)(void *, void *, size_t);
    void (*barrier)(void *);
    /* Sticky transport status, 0 until read/write fails. Reset cannot prove
     * DMA stopped through a failed MMIO path; backing is then quarantined. */
    int (*io_error)(void *);
    /* Optional bounded stop-only recovery. A provider may renew a diagnostic
     * deadline, never guess away a transport fault or permit another launch. */
    int (*recover_io)(void *);
    void (*phase)(void *,uint32_t);
};
struct lm_core *lm_core_create(const struct lm_ops *);
/* identity + reset + internal GPU core power. Platform supply already acquired. */
int lm_core_start(struct lm_core *);
bool lm_core_ready(const struct lm_core *);
/* Prove internal core power-off before outer platform domains are disabled. */
int lm_core_shutdown(struct lm_core *);
/* Fails without freeing while GPU/DMA is not proved stopped. */
int lm_core_destroy(struct lm_core *);
int lm_core_get_param(struct lm_core *, uint32_t panfrost_param, uint64_t *);
struct lm_vm *lm_core_vm_new(struct lm_core *);
int lm_core_vm_destroy(struct lm_vm *);
int lm_core_vm_map(struct lm_vm *, uint64_t gpu_va, const struct lm_page *, uint32_t pages, uint32_t flags);
int lm_core_vm_unmap(struct lm_vm *, uint64_t gpu_va, uint32_t pages);
bool lm_core_vm_safe_to_free(const struct lm_vm *);
int lm_core_submit_wait(struct lm_vm *, uint64_t jc, uint32_t requirements,
                        uint64_t timeout_ns, struct lm_job_result *);
/* Host-visible walk used by tests and diagnostics, not a user physical mapping API. */
uint64_t lm_core_vm_lookup(struct lm_vm *, uint64_t gpu_va, uint32_t *flags);

/* Serialized XNU-facing facade, implemented in lancelot_mali.c.
 * The client owns wired lm_page backing until unmap+safe_to_free have succeeded.
 * A nonzero destroy/unmap return means backing must be kept, never freed. */
/* Board integration must install an actual shared WACS2 arbiter before init.
 * The provider must exclude all CPUs/firmware users, not just Mali calls. The
 * default is unavailable: full modes fail before any PWRAP access, never guess. */
struct lm_gpu_pwrap_owner {
    void *cookie;
    int (*acquire)(void *, uint64_t timeout_ns);
    void (*release)(void *);
    void (*poison)(void *); /* shared channel, not a Mali-local flag */
};
int lm_gpu_register_pwrap_owner(const struct lm_gpu_pwrap_owner *);
void lancelot_mali_init(void);
bool lm_gpu_ready(void);
int lm_gpu_param(uint32_t panfrost_param, uint64_t *value);
struct lm_vm *lm_gpu_vm_new(void);
int lm_gpu_vm_destroy(struct lm_vm *);
int lm_gpu_vm_map(struct lm_vm *, uint64_t va, const struct lm_page *, uint32_t pages, uint32_t flags);
int lm_gpu_vm_unmap(struct lm_vm *, uint64_t va, uint32_t pages);
bool lm_gpu_vm_safe_to_free(const struct lm_vm *);
int lm_gpu_submit_wait(struct lm_vm *, uint64_t jc, uint32_t requirements,
                       uint64_t timeout_ns, struct lm_job_result *);
#ifdef __cplusplus
}
#endif
#endif
