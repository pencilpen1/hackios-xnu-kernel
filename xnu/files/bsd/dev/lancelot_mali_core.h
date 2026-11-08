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
#define LM_MAX_SUBMIT_RANGES 8192u /* Matches the existing LMAL BO-list bound. */
#define LM_RANGE_GPU_ONLY 1u /* Trusted internal BO: no CPU exposure, not JC. */
#define LM_RANGE_READONLY 2u /* Per-job GPU RO, enforced only in alias-free scope. */
struct lm_range { uint64_t va; uint32_t pages, flags; };
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
    LM_JOB_WAIT, LM_JOB_CACHE, LM_JOB_DETACH, LM_JOB_ACK, LM_JOB_AS_FLUSH,
    LM_JOB_DVFS_GUARD, LM_JOB_DEADLINE, LM_JOB_SCOPE_RESTORE,
    LM_JOB_SCOPE_SETUP
};
enum lm_core_phase {
    LM_PHASE_NONE,LM_PHASE_ID,LM_PHASE_RESET,LM_PHASE_CONFIG,LM_PHASE_L2_ON,
    LM_PHASE_TILER_ON,LM_PHASE_SHADER_ON,LM_PHASE_FEATURES,LM_PHASE_BIND,
    LM_PHASE_NEXT,LM_PHASE_WAIT,LM_PHASE_CACHE,LM_PHASE_DETACH,LM_PHASE_OFF,
    LM_PHASE_FINAL_RESET
};
struct lm_poll_counts {
    uint64_t spins, sleeps, spin_ns, sleep_ns;
};
#define LM_CHAIN_NODES 64u
struct lm_chain_node {
    uint64_t va,next,bo_va;
    uint32_t bo_pages,type,captured_words;
    uint32_t words[96];
};
struct lm_chain_snapshot {
    uint32_t count,stop_reason; /* 0=end,1=limit,2=cycle,3=unmapped,4=header */
    uint64_t stop_va;
    struct lm_chain_node nodes[LM_CHAIN_NODES];
};
struct lm_job_result {
    uint64_t sequence, elapsed_ns, fault_address;
    uint64_t queue_token; /* Adapter-owned identity, never a hardware sequence. */
    /* Borrowed engine-owned capture, stable once this engine goes offline.
     * Never use after lm_core_destroy. No fault-time GPU buffer reads. */
    const struct lm_chain_snapshot *chain;
    struct {
        uint64_t sampled_ns;
        uint32_t hz,measured_hz,vgpu_uv,vsram_uv;
        int32_t target_error;
        bool valid;
    } dvfs_sample,dvfs_after_sample; /* Admission and post-return cached AP evidence. */
    uint64_t started_ns, completed_ns; /* Actual dispatch wall, not enqueue/fence time. */
    /* Set by pair dispatch after each real API call. A skipped stage keeps
     * attempted=false, error=0, sequence=0; zero error alone is not completion. */
    int32_t pair_stage_error;
    bool pair_stage_attempted;
    /* Independent of profile mode. Real JM launch to fresh ALL_JS idle proof;
     * includes AP launch/poll overhead, never advertised as HW utilization.
     * Launched with no verified end is unknown, not an idle/zero-work sample. */
    struct { uint64_t start_ns,end_ns; bool launched,idle_verified; } jm;
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
    /* Software VM state at the original MMU fault, before reset/restore.
     * Never dereference GPU data or publish additional mappings to diagnose it. */
    struct {
        bool valid, scoped, mapped, selected, visible;
        uint32_t pages, flags;
        uint64_t va, page_pa, leaf_pte, expected_pte;
    } fault_vm;
    /* Opt-in CPU descriptor snapshot AFTER for_device and BEFORE launch.
     * Retained through reset without any fault-time GPU-buffer dereference.
     * Four 64-byte records: job, G52 FBD params, tiler context, first RT.
     * The mask counts actual captures; an absent record is never zero data. */
    struct {
        uint32_t mask, ranges, readonly_ranges;
        uint64_t va[4], mapping[4];
        uint32_t pages[4], words[4][16];
    } descriptors;
    /* Optional kernel/host diagnostics only; not part of the LMAL user ABI.
     * run_ns includes launch/poll/status checks, not GPU-only execution time. */
    struct {
        uint64_t for_device_ns, bind_ns, preflush_ns, run_ns;
        uint64_t postflush_ns, detach_ns, for_cpu_ns;
        uint64_t run_thread_ns; /* Optional caller thread CPU time, not HW time. */
        struct lm_poll_counts job_wait, control_wait;
        uint64_t last_busy_ns, done_seen_ns, max_poll_gap_ns;
        uint32_t polls;
        bool hybrid_poll;
        uint32_t mapped_pages, table_pages, cpu_pages, device_ranges, cpu_ranges, clean_pages;
        bool enabled;
        /* scope_mode: 0 full VM, 1 isolated workset, 2 aliases, 3 OOM fallback. */
        uint32_t vm_pages, requested_pages, scope_mode;
        uint64_t scope_setup_ns, scope_restore_ns;
        uint32_t masked_leaf_tables, masked_leaf_entries;
        uint32_t readonly_pages;
        uint32_t cleaned_table_pages; /* Actual table callbacks, not VM footprint. */
    } profile;
};
struct lm_ops {
    void *cookie;
    /* Called only once platform prepare has proved all domains accessible. */
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    uint64_t (*now_ns)(void *);
    uint64_t (*thread_ns)(void *); /* Optional diagnostic runtime clock only. */
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
    /* XNU opts in via lancelot_preflush (default 1). Zero-initialized
     * firmware callers retain their existing submission sequence. */
    bool pre_job_flush;
    /* Optional CPU-view snapshot after for_device/barrier and IRQ clear,
     * before preflush/JS NEXT. At most 32 callbacks per core instance. */
    void (*pre_job)(void *,uint64_t jc,const uint32_t words[4]);
    bool capture_descriptors; /* Diagnostic read only, disabled unless requested. */
    /* MT6768 donor: 35-bit G52 VA, three levels, uncached GPU table walks.
     * CPU table PoC clean and translation retirement remain mandatory. */
    bool g52_walk_nc;
    /* 0: legacy; 1: full-VA FLUSH_MEM; 2: full-VA FLUSH_PT after bind.
     * Exclusive modes, selected by XNU lancelot_preasflush (default 0). */
    uint32_t pre_as_flush;
    /* AARCH64_4K SH=inner with MEMATTR[0]=0x88 means GPU-internal sharing,
     * not CPU/GPU ACE coherency. XNU enables this by default; zero-initialized
     * firmware callers retain their existing page-table attributes. */
    bool gpu_shareable;
    /* Opt-in: callbacks accept up to 64 contiguous CPU-VA pages per range.
     * Physical pages may be noncontiguous. No page/line is skipped, and
     * mapping boundaries and the final completion barriers are preserved.
     * Zero-initialized firmware callers retain 4K callbacks. */
    bool cache_batch;
    bool profile_jobs;
    bool submit_scope; /* Explicit BO workset API only; legacy/firmware unchanged. */
    bool gpu_only_sync; /* First clean only for trusted GPU-owned BOs; requires scope. */
    /* Only CPU-mutated table pages need another clean before root publication.
     * Map/unmap, rollback, parent masking and RO restoration all mark dirty.
     * Data BO callbacks/barriers/AS and GPU flushes are unaffected. */
    bool table_dirty_sync;
    /* Optional preemptible short delay, 1..10us, returning an error if its
     * clock/iteration bound fails. NULL retains legacy/firmware 10us delays.
     * Core bounds total spin requests AND elapsed time, then uses delay_us. */
    int (*spin_us)(void *, uint32_t);
    /* Optional serialized DVFS admission BEFORE binding/launch, after input
     * validation. Nonzero returns with no reset/launch; the callback owns any
     * external revoke. Its elapsed time is inside this job's original budget.
     * Called for each actual VTC/FS dispatch, not just once per pair. */
    int (*before_job)(void *);
    /* Pure accounting only, under the caller's serializer. No hardware access,
     * rate change, VM/backing retirement or callback reentry is permitted.
     * Reports real launch/idle facts even when ordinary profiling is disabled.
     * Outer scope failure must still invalidate/stop the adapter's sampler. */
    void (*job_done)(void *,const struct lm_job_result *,int error);
};
struct lm_core *lm_core_create(const struct lm_ops *);
/* identity + reset + internal GPU core power. Platform supply already acquired. */
int lm_core_start(struct lm_core *);
bool lm_core_ready(const struct lm_core *);
/* Serialized opt-in DVFS core checks. prepare is allowed only with no VMs;
 * guard refreshes all present JS CURRENT/NEXT and proves all AS unmapped.
 * Neither substitutes for the adapter's external clock/rail owner contract. */
int lm_core_dvfs_prepare(struct lm_core *, struct lm_job_result *);
int lm_core_dvfs_guard(struct lm_core *, struct lm_job_result *);
struct lm_core_quiescence {
    uint32_t vm_count,mapped_page_refs,table_pages;
};
/* Fresh JS/AS proof plus every registered VM idle/nonquarantined, with all
 * mappings/table backing still retained. No reset/unmap/free occurs. */
int lm_core_return_guard(struct lm_core *,struct lm_core_quiescence *);
/* No MMIO/reset/free: unknown external DVFS state retains every VM/backing
 * until reboot, even when a local VM busy/quarantine bit had been clear. */
void lm_core_external_revoke(struct lm_core *);
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
int lm_core_submit_ranges(struct lm_vm *, uint64_t jc, uint32_t requirements,
                          uint64_t timeout_ns, struct lm_job_result *,
                          const struct lm_range *, uint32_t count);
/* Caller holds the device serializer across the whole pair. Both exact BO
 * lists/JC permissions are checked before the first launch. Real VTC completion
 * precedes FS; existing flush/AS/handoff semantics remain conservative. */
int lm_core_submit_pair(struct lm_vm *, uint64_t vt_jc, uint64_t fs_jc,
                        uint64_t timeout_ns, struct lm_job_result results[2],
                        const struct lm_range *vt_ranges, uint32_t vt_count,
                        const struct lm_range *fs_ranges, uint32_t fs_count);
int lm_core_vm_cpu_access(struct lm_vm *, uint64_t va, uint32_t pages);
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
struct lmp_dvfs_limits;
struct lm_gpu_dvfs_limit_provider {
    void *cookie;
    /* Kernel-internal real feedback only. Callable storage is retained for
     * the boot lifetime; report lifecycle even on failed reads. */
    int (*read)(void *,struct lmp_dvfs_limits *);
};
/* One permanent aggregate provider, before or after GPU initialization.
 * No remove/replace API can turn an already-started source into absence. */
int lm_gpu_register_dvfs_limits(const struct lm_gpu_dvfs_limit_provider *);
void lancelot_mali_init(void);
bool lm_gpu_ready(void);
/* Read-only cached first failure, rate-limited; never probes/restores hardware. */
void lm_gpu_report_first_failure(void);
int lm_gpu_param(uint32_t panfrost_param, uint64_t *value);
struct lm_vm *lm_gpu_vm_new(void);
int lm_gpu_vm_destroy(struct lm_vm *);
int lm_gpu_vm_map(struct lm_vm *, uint64_t va, const struct lm_page *, uint32_t pages, uint32_t flags);
int lm_gpu_vm_unmap(struct lm_vm *, uint64_t va, uint32_t pages);
bool lm_gpu_vm_safe_to_free(const struct lm_vm *);
int lm_gpu_submit_wait(struct lm_vm *, uint64_t jc, uint32_t requirements,
                       uint64_t timeout_ns, struct lm_job_result *);
int lm_gpu_submit_ranges(struct lm_vm *, uint64_t jc, uint32_t requirements,
                          uint64_t timeout_ns, struct lm_job_result *,
                          const struct lm_range *, uint32_t count);
int lm_gpu_submit_ranges_owned(struct lm_vm *, uint64_t jc, uint32_t requirements,
                              uint64_t timeout_ns, struct lm_job_result *,
                              const struct lm_range *, uint32_t count,
                              int32_t owner_pid, uint64_t queue_token);
int lm_gpu_submit_pair(struct lm_vm *, uint64_t vt_jc, uint64_t fs_jc,
                       uint64_t timeout_ns, struct lm_job_result results[2],
                       const struct lm_range *vt_ranges, uint32_t vt_count,
                       const struct lm_range *fs_ranges, uint32_t fs_count,
                       int32_t owner_pid, uint64_t pair_token);
int lm_gpu_vm_cpu_access(struct lm_vm *, uint64_t va, uint32_t pages);
/* Thread/IRQ-enabled drain, then frozen boot-core final proof before jump. */
int lm_gpu_return_prepare(void);
int lm_gpu_return_final(void);
#ifdef __cplusplus
}
#endif
#endif
