/*
 * Portable MT6768/Lancelot Mali platform lifecycle.
 *
 * The platform layer owns only resources that it changes.  It never writes in
 * LMP_MODE_SPM_ONLY, and it refuses to infer a PMIC bus protocol.  The XNU
 * adapter supplies the callbacks after validating the loader/board contract;
 * host tests provide the same callbacks with a register model.
 *
 * Register facts are pinned to commit 6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7
 * (clk-mt6768.c, clk-mt6768-pg.c, clk-gate.c):
 *   SPM 0x10006000, infracfg_ao 0x10001000, mfg_cfg 0x13000000,
 *   Mali 0x13040000.  The platform layer uses offsets, never physical casts.
 */
#ifndef LANCELOT_MALI_PLATFORM_H
#define LANCELOT_MALI_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lancelot_mali_dvfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keep this header usable by XNU and freestanding host tests. */
#define LMP_OK         0
#define LMP_EINVAL    (-22)
#define LMP_ENOMEM    (-12)
#define LMP_EBUSY     (-16)
#define LMP_ENODEV    (-19)
#define LMP_ENOTSUP   (-95)
#define LMP_ETIMEDOUT (-110)
#define LMP_EIO       (-5)
#define LMP_EFAULT    (-14)

#define LMP_MAX_POLLS 100000u
#define LMP_DEFAULT_TIMEOUT_NS UINT64_C(100000000)

/* Only this board/SoC contract is accepted by this implementation. */
enum lmp_platform_id {
    LMP_PLATFORM_UNKNOWN = 0,
    LMP_PLATFORM_LANCELOT_MT6768 = 1,
};

enum lmp_mode {
    LMP_MODE_OFF = 0,
    /* Read SPM power state only.  This mode is structurally write-free. */
    LMP_MODE_SPM_ONLY = 1,
    /* Permit the full resource lifecycle when callbacks and state permit it. */
    LMP_MODE_FULL = 2,
};

enum lmp_bank {
    LMP_BANK_SPM = 0,
    LMP_BANK_INFRA = 1,
    LMP_BANK_MFGCFG = 2,
    LMP_BANK_GPU = 3,
    /* Always-on upstream clock banks checked before MFG/GPU access. */
    LMP_BANK_TOP = 4,
    LMP_BANK_APMIXED = 5,
    LMP_BANK_COUNT = 6,
};

enum lmp_rail {
    /* These names match the MT6358 regulator provider's DT names. */
    LMP_RAIL_VGPU = 0,
    LMP_RAIL_VSRAM_GPU = 1,
    LMP_RAIL_COUNT = 2,
};

struct lmp_gpu_rails {
    uint32_t uv[LMP_RAIL_COUNT];
    bool enabled[LMP_RAIL_COUNT];
};

struct lmp_pmic_ops {
    void *cookie;
    /* Provider operations, not raw PMIC/I2C register access. */
    int (*is_enabled)(void *cookie, enum lmp_rail rail, bool *enabled);
    int (*enable)(void *cookie, enum lmp_rail rail);
    int (*disable)(void *cookie, enum lmp_rail rail);
    int (*set_voltage_uv)(void *cookie, enum lmp_rail rail, uint32_t uv);
    int (*get_voltage_uv)(void *cookie, enum lmp_rail rail, uint32_t *uv);
    /* Stable pair under the SAME shared WACS2 arbiter. Required for DVFS. */
    int (*read_rails)(void *cookie, struct lmp_gpu_rails *rails);
};

#define LMP_DVFS_LIMIT_THERMAL 1u
#define LMP_DVFS_LIMIT_BATTERY 2u
#define LMP_DVFS_LIMIT_POWER   4u
#define LMP_DVFS_LIMIT_ALL     7u
#define LMP_DVFS_OPTIONAL_MAX_HZ 850000000u
struct lmp_dvfs_limits {
    /* Started means operational/start of real reads, NOT mere registration
     * or initialization still pending. Fill lifecycle BEFORE reads, even on
     * an error return. Valid feedback also implies started. Once seen, started
     * is sticky in DVFS:
     * removal/read failure cannot masquerade as a never-started provider. */
    uint32_t started_mask;
    uint32_t valid_mask;
    uint32_t fault_mask; /* Actual read/feedback failure, not ordinary absence. */
    uint32_t thermal_max_hz, battery_max_hz, power_max_hz;
    /* Common freshness interval must cover every valid component. Absent
     * components have no invented sensor value/cap and are ignored. A VALID
     * zero cap is a real shutdown request, not a missing-provider sentinel. */
    uint64_t sampled_ns, valid_until_ns;
};
struct lmp_dvfs_ops {
    void *cookie;
    /* Policy0 retains the original peer-exclusive contract. Policy1 is the
     * explicit native AP route: same XNU executor, live core/control-source
     * checks and unchanged parking source, NOT a firmware/all-peer grant. */
    uint32_t control_policy;
    /* Mandatory, success ONLY while caller owns global mali_lock, AP owns
     * MFGPLL/mfg_sel/GPU rails AND shared TOP CKGEN frequency-meter registers
     * against ALL foreign writers (including firmware), MAINPLL/syspll_d3
     * parking reference/rate is held stable for the entire transition (this
     * backend never writes that shared PLL), core is healthy,
     * no bound/quarantined/unknown-DMA VM, fresh ALL JS CURRENT/NEXT idle and
     * ALL present AS roots disabled/detached. Lock/known busy => EBUSY (no
     * mutation). Unknown ownership/completion => EIO (revoke). No cached bool.
     * Lock and exclusivity MUST remain held until this API returns for policy0.
     * Native policy1 checks local serialization and actual control state at
     * every boundary; external-peer exclusivity remains explicitly unverified.
     * Its final-return context requires frozen callers and independent AP-OFF. */
    int (*guard)(void *cookie);
    /* Mandatory: terminal-offline core/service and retain ALL backing, no
     * reset, DMA, power or clock writes. Called under that same mali_lock. */
    void (*revoke)(void *cookie, int error);
    /* Optional real hardware-backed XNU caps, never Android snapshots/mock
     * temperature. Any never-started/absent component limits the effective
     * maximum to850MHz but DOES NOT disable load DVFS or boost below that cap.
     * Every available valid lower cap still wins. All three started+valid allow
     * rated950MHz. NULL/ENODEV/ENOTSUP mean ordinary startup absence ONLY when
     * no previously/explicitly started component is lost or invalid; aggregate
     * ENODEV/ENOTSUP may report partial valid feedback with other components
     * absent. Other errors, fault_mask, stale feedback or loss after start are
     * protective failures. Sample again before every resource write. */
    int (*limits)(void *cookie, struct lmp_dvfs_limits *limits);
};
#define LMP_DVFS_PEER_EXCLUSIVE 0u
#define LMP_DVFS_NATIVE_AP 1u
struct lmp_dvfs_clock {
    uint32_t top, mfg[3], main[3]; /* CON0,CON1,CON3 */
    uint32_t hz, park_hz;
};
struct lmp_dvfs_state {
    struct lmp_dvfs_ops ops;
    struct lmp_dvfs_clock clock;
    struct lmp_gpu_rails rails;
    uint32_t current_opp, target_opp, stage, transitions;
    uint32_t measured_hz, meter_count, meter_samples;
    uint32_t provider_started_mask, provider_valid_mask, provider_missing_mask;
    uint32_t provider_fault_mask, effective_max_hz;
    uint64_t measured_ns;
    uint64_t operation_start_ns;
    int error, provider_error;
    bool attached, transitioning, changed, meter_valid, limit_policy_valid;
    bool operation_active;
};

struct lmp_io_ops {
    void *cookie;
    /* Return zero on success and write the value to *value. */
    int (*read32)(void *cookie, enum lmp_bank bank, uint32_t offset,
                  uint32_t *value);
    int (*write32)(void *cookie, enum lmp_bank bank, uint32_t offset,
                   uint32_t value);
    uint64_t (*now_ns)(void *cookie);
    void (*delay_us)(void *cookie, uint32_t usec);
    struct lmp_pmic_ops pmic;
};

struct lmp_config {
    enum lmp_platform_id platform;
    enum lmp_mode mode;
    /* Preserve all inherited resources and skip all teardown writes. */
    bool keep_resources;
    uint64_t timeout_ns;
};

struct lmp_status {
    uint32_t pwr_status;
    uint32_t pwr_status_2nd;
    uint32_t domain_mask;
    bool domain_on;
    bool domain_ack_match;
    bool gpu_accessible;
};

struct lmp_state {
    struct lmp_io_ops ops;
    struct lmp_config config;
    struct lmp_status status;
    bool initialized;
    bool prepared;
    bool started;
    bool failed;
    uint32_t last_stage; /* lmp_start step reached: 1 require, 2 prepare, 3 power, 4 clock, 5 reset, 6 bus */
    bool teardown_safe;
    bool gpu_ready;

    /* Initial values are retained for diagnostics and ownership decisions. */
    uint32_t initial_pwr_con[3]; /* MFG, CORE0, CORE1; async is separate. */
    uint32_t initial_async_pwr_con;
    uint32_t initial_cg_con;
    uint32_t initial_top_mfg_cfg;
    uint32_t initial_mfgpll_con0;
    uint32_t initial_mfgpll_con3;
    uint32_t initial_parent_pll_con0;
    uint32_t initial_parent_pll_con3;
    uint32_t initial_protect;
    uint32_t initial_protect_ack;
    bool initial_rail_on[LMP_RAIL_COUNT];
    uint32_t source_hz;
    uint32_t observed_uv[LMP_RAIL_COUNT];
    bool initial_domain_on[4]; /* MFG, CORE0, CORE1, ASYNC. */
    bool initial_clock_on;
    bool initial_bus_protected;
    bool initial_reset_released;

    /* True only when this instance changed/owns the resource. */
    bool rail_owned[LMP_RAIL_COUNT];
    bool domain_owned[4];
    bool clock_owned;
    bool reset_owned;
    bool bus_owned;
    struct lmp_dvfs_state dvfs;
};

/* Validate the board/ops contract. OFF mode intentionally needs no callbacks. */
int lmp_init(struct lmp_state *state, const struct lmp_config *config,
             const struct lmp_io_ops *ops);

/* Reads only always-on SPM status.  No MFG/GPU access occurs here. */
int lmp_status(struct lmp_state *state, struct lmp_status *status);

/* Snapshot safe-to-read state before any full-mode write. */
int lmp_prepare(struct lmp_state *state);

/* Verify the inherited TOP mux and selected APMIXED PLL are live. */
int lmp_upstream_clock_ready(struct lmp_state *state);

/* Full-mode staged lifecycle. Each function is idempotent when successful. */
int lmp_power_on(struct lmp_state *state);
int lmp_clock_on(struct lmp_state *state);
int lmp_reset_release(struct lmp_state *state);
int lmp_bus_unprotect(struct lmp_state *state);
int lmp_start(struct lmp_state *state);

/* GPU bank access is allowed only after lmp_start succeeds. */
int lmp_gpu_read32(struct lmp_state *state, uint32_t offset, uint32_t *value);
int lmp_gpu_write32(struct lmp_state *state, uint32_t offset, uint32_t value);

/* Teardown writes only resources owned by this state and only if safe. */
int lmp_teardown(struct lmp_state *state);

/* Explicit opt-in direct AP GPU backend; never inferred from FULL mode or
 * SSPM CPU handoff. Existing lifecycle/read-only CPU observers are unchanged.
 * Attach never changes clocks/rails; it uses the source-pinned CKGEN5 meter
 * (debug/counter registers only, restored after known completion). target is
 * an OPP index, 0=950MHz,29=299MHz. A successful return always has actual
 * stable PLL/mux, independent CKGEN output measurement and DA rail readback.
 * Errors after uncertainty/write revoke GPU rights; NEVER blindly rollback. */
/* Fresh caps / normal optional-provider850MHz ceiling clamp demand (including
 * a same-OPP request). Read current_opp
 * after success. The adapter must check the result before EVERY next job and
 * also run periodic idle governor ticks; this layer creates no background
 * worker and cannot enforce a cap if the caller never invokes it. */
int lmp_dvfs_attach(struct lmp_state *state, const struct lmp_dvfs_ops *ops);
int lmp_dvfs_target(struct lmp_state *state, uint32_t opp_index);
/* Job admission: fresh power/PLL/rail/limit checks every call; an unchanged
 * native-AP OPP may reuse a measured frequency for at most 10ms. Transitions,
 * expiry and missing samples use the full target/measurement path. */
int lmp_dvfs_admit(struct lmp_state *, uint32_t opp_index, uint64_t max_age_ns, bool *reused);
struct lmp_dvfs_sample {
    struct lmp_dvfs_clock clock;
    struct lmp_gpu_rails rails;
    uint32_t measured_hz;
    uint64_t measured_ns;
};
/* Fresh clock/rails/output verification, no OPP/voltage changes or provider
 * calls. Suitable only in the explicitly frozen singleton final context. */
int lmp_dvfs_verify(struct lmp_state *, struct lmp_dvfs_sample *);

/* Source-pinned offsets and masks are public for the XNU/core adapter/tests. */
#define LMP_SPM_PWR_STATUS       0x180u
#define LMP_SPM_PWR_STATUS_2ND   0x184u
#define LMP_SPM_MFG_CORE1_PWR_CON 0x310u
#define LMP_SPM_MFG_ASYNC_PWR_CON 0x334u
#define LMP_SPM_MFG_PWR_CON       0x338u
#define LMP_SPM_MFG_CORE0_PWR_CON 0x34cu
#define LMP_INFRA_PROTECTEN       0x220u
#define LMP_INFRA_PROTECTEN_STA1  0x228u
#define LMP_INFRA_PROTECTEN_SET   0x2a0u
#define LMP_INFRA_PROTECTEN_CLR   0x2a4u
#define LMP_MFG_CG_CON            0x000u
#define LMP_MFG_CG_SET            0x004u
#define LMP_MFG_CG_CLR            0x008u
#define LMP_TOP_CLK_CFG_1         0x050u
#define LMP_TOP_CLK_CFG_1_SET     0x054u
#define LMP_TOP_CLK_CFG_1_CLR     0x058u
#define LMP_TOP_CLK_CFG_UPDATE    0x004u
#define LMP_TOP_MFG_UPDATE        (1u << 4)
#define LMP_TOP_MFG_GATE          (1u << 7)
#define LMP_TOP_CLK_MISC_CFG_0    0x104u
#define LMP_TOP_CLK_DBG_CFG       0x10cu
#define LMP_TOP_CLK26CALI_0       0x220u
#define LMP_TOP_CLK26CALI_1       0x224u
#define LMP_TOP_MFG_SEL_SHIFT     0u
#define LMP_TOP_MFG_SEL_MASK      0x3u
#define LMP_APMIXED_MFGPLL_CON0   0x248u
#define LMP_APMIXED_MFGPLL_CON1   0x24cu
#define LMP_APMIXED_MFGPLL_CON3   0x254u
#define LMP_APMIXED_MAINPLL_CON0  0x258u
#define LMP_APMIXED_MAINPLL_CON3  0x264u
#define LMP_APMIXED_FHCTL_HP_EN   0xe04u
#define LMP_FHCTL_MFGPLL_HP_MASK  (1u << 3) /* vendor FH channel3, not CCF clock4 */
#define LMP_APMIXED_UNIVPLL_CON0  0x238u
#define LMP_APMIXED_UNIVPLL_CON3  0x244u
#define LMP_PLL_EN                (1u << 0)
#define LMP_PLL_ISO_EN            (1u << 1)
#define LMP_PLL_PWR_ON            (1u << 0)
#define LMP_GPU_ID                0x000u
#define LMP_GPU_STATUS            0x034u
#define LMP_GPU_L2_FEATURES       0x004u
#define LMP_GPU_TILER_FEATURES    0x00cu
#define LMP_GPU_MEM_FEATURES      0x010u
#define LMP_GPU_MMU_FEATURES      0x014u
#define LMP_GPU_AS_PRESENT        0x018u
#define LMP_GPU_JS_PRESENT        0x01cu
#define LMP_GPU_THREAD_FEATURES   0x0acu
#define LMP_GPU_COHERENCY_FEATURES 0x300u

#define LMP_PWR_RST_B             (1u << 0)
#define LMP_PWR_ISO               (1u << 1)
#define LMP_PWR_ON                (1u << 2)
#define LMP_PWR_ON_2ND            (1u << 3)
#define LMP_PWR_CLK_DIS           (1u << 4)
#define LMP_SRAM_PDN              (1u << 8)
#define LMP_SRAM_PDN_ACK          (1u << 12)
#define LMP_MFG_PWR_STA_MASK      (1u << 11)
#define LMP_MFG_CORE0_STA_MASK    (1u << 12)
#define LMP_MFG_CORE1_STA_MASK    (1u << 13)
#define LMP_MFG_ASYNC_STA_MASK    (1u << 14)
#define LMP_MFG_DOMAIN_MASK       (LMP_MFG_PWR_STA_MASK | \
                                   LMP_MFG_CORE0_STA_MASK | \
                                   LMP_MFG_CORE1_STA_MASK | \
                                   LMP_MFG_ASYNC_STA_MASK)
#define LMP_MFG_PROT_STEP1_MASK   (1u << 25)
#define LMP_MFG_PROT_STEP2_MASK   ((1u << 21) | (1u << 22))
#define LMP_MFG_PROT_MASK         (LMP_MFG_PROT_STEP1_MASK | \
                                   LMP_MFG_PROT_STEP2_MASK)

#ifdef __cplusplus
}
#endif
#endif
