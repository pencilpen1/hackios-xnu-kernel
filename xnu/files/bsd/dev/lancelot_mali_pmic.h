/*
 * MT6358 provider-backed PMIC access over an already initialized MT6768 PWRAP.
 *
 * This is a transport and allowlisted provider adapter. It never initializes,
 * resets, clocks, or reconfigures the shared PWRAP. The caller must supply
 * register addresses from the real MT6358 generated provider header; zero
 * descriptors are rejected rather than guessed.
 */
#ifndef LANCELOT_MALI_PMIC_H
#define LANCELOT_MALI_PMIC_H

#include <stdbool.h>
#include <stdint.h>
#include "lancelot_mali_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LMP_PMIC_WRAP_BASE_PHYS UINT64_C(0x1000d000)
#define LMP_PMIC_WACS2_CMD      0x0c20u
#define LMP_PMIC_WACS2_RDATA    0x0c24u
#define LMP_PMIC_WACS2_VLDCLR   0x0c28u
#define LMP_PMIC_INIT_DONE2     0x0094u
#define LMP_PMIC_FSM_IDLE       0x00u
#define LMP_PMIC_FSM_WFVLDCLR   0x06u
#define LMP_PMIC_SYNC_IDLE      0x01u
#define LMP_PMIC_INIT_DONE      0x01u
#define LMP_PMIC_WACS_TIMEOUT_NS UINT64_C(10000000)
#define LMP_PMIC_MAX_POLLS      100000u

#define LMP_PMIC_CID_E1 0x5810u
#define LMP_PMIC_CID_E2 0x5820u
#define LMP_PMIC_CID_E3 0x5830u

struct lmp_pmic_io {
    void *cookie;
    int (*read32)(void *cookie, uint32_t wrap_offset, uint32_t *value);
    int (*write32)(void *cookie, uint32_t wrap_offset, uint32_t value);
    uint64_t (*now_ns)(void *cookie);
    void (*delay_us)(void *cookie, uint32_t usec);
    uint64_t timeout_ns;
    /* Mandatory external arbiter: it must exclude EVERY WACS2 user, including
     * firmware/other CPUs. A Mali-local mutex or an observed idle FSM is not
     * ownership proof. acquire must return an error without granting ownership;
     * release is called only after successful acquire, on every exit. */
    int (*acquire)(void *cookie, uint64_t timeout_ns);
    void (*release)(void *cookie);
    /* Mandatory shared-channel poison for an OWN command whose completion is
     * unknown. Must block every future WACS2 user before releasing arbitration. */
    void (*poison)(void *cookie);
};

struct lmp_pmic_field {
    uint16_t addr;
    uint16_t mask;
    uint8_t shift;
};

/*
 * These are the only PMIC addresses this adapter can touch. The generated
 * MT6358 upmu_hw header supplies the values at the XNU integration boundary.
 */
struct lmp_pmic_registers {
    uint16_t swcid_addr;
    struct lmp_pmic_field vgpu_en;
    struct lmp_pmic_field vgpu_da_en;
    struct lmp_pmic_field vgpu_vosel;
    struct lmp_pmic_field vgpu_da_vosel;
    struct lmp_pmic_field vsram_gpu_en;
    struct lmp_pmic_field vsram_gpu_da_en;
    struct lmp_pmic_field vsram_gpu_vosel;
    struct lmp_pmic_field vsram_gpu_da_vosel;
};

struct lmp_pmic_state {
    struct lmp_pmic_io io;
    struct lmp_pmic_registers regs;
    uint16_t chip_id;
    bool validated;
    bool command_pending;
    bool rail_enabled[LMP_RAIL_COUNT];
};

/* Values mirrored from the locally available MT6768 upmu_hw.h. */
void lmp_pmic_mt6358_registers(struct lmp_pmic_registers *regs);

/* Validate a source-derived map without touching hardware. */
int lmp_pmic_validate_registers(const struct lmp_pmic_registers *regs);

/* Read WACS2 state and MT6358 SWCID; no PWRAP setup writes are performed. */
int lmp_pmic_probe(struct lmp_pmic_state *state,
                   const struct lmp_pmic_io *io,
                   const struct lmp_pmic_registers *regs);

/* Expose provider callbacks to lmp_init/lmp_power_on. */
int lmp_pmic_get_platform_ops(struct lmp_pmic_state *state,
                              struct lmp_pmic_ops *ops);

/* Explicit provider operations for tests and diagnostics. */
int lmp_pmic_is_enabled(struct lmp_pmic_state *state, enum lmp_rail rail,
                        bool *enabled);
int lmp_pmic_enable(struct lmp_pmic_state *state, enum lmp_rail rail);
int lmp_pmic_disable(struct lmp_pmic_state *state, enum lmp_rail rail);
int lmp_pmic_get_voltage_uv(struct lmp_pmic_state *state, enum lmp_rail rail,
                            uint32_t *uv);
int lmp_pmic_set_voltage_uv(struct lmp_pmic_state *state, enum lmp_rail rail,
                            uint32_t uv);

#ifdef __cplusplus
}
#endif
#endif
