/* SPDX-License-Identifier: GPL-2.0-only */
#include "lancelot_mali_pmic.h"

static void
lmp_pmic_zero(void *ptr, uint32_t bytes)
{
    uint8_t *p = (uint8_t *)ptr;
    uint32_t i;
    for (i = 0; i < bytes; i++)
        p[i] = 0;
}

void
lmp_pmic_mt6358_registers(struct lmp_pmic_registers *regs)
{
    if (!regs)
        return;
    lmp_pmic_zero(regs, (uint32_t)sizeof(*regs));
    /* Values from commit 6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7:
     * drivers/misc/mediatek/include/mt-plat/mt6768/include/mach/upmu_hw.h.
     * SHA256 dd3b3eec9c11a708457c659e5001cde10ce4cf97f7573550a99a0a9f9f31f19c.
     */
    regs->swcid_addr = 0x000au;
    regs->vgpu_en = (struct lmp_pmic_field){0x1508u, 0x1u, 0u};
    regs->vgpu_da_en = (struct lmp_pmic_field){0x1520u, 0x1u, 0u};
    regs->vgpu_vosel = (struct lmp_pmic_field){0x1526u, 0x7fu, 0u};
    regs->vgpu_da_vosel = (struct lmp_pmic_field){0x151eu, 0x7fu, 0u};
    regs->vsram_gpu_en = (struct lmp_pmic_field){0x1bc8u, 0x1u, 0u};
    regs->vsram_gpu_da_en = (struct lmp_pmic_field){0x1be4u, 0x1u, 0u};
    regs->vsram_gpu_vosel = (struct lmp_pmic_field){0x1bf4u, 0x7fu, 0u};
    /* The generated header intentionally places this DA field at [14:8]. */
    regs->vsram_gpu_da_vosel = (struct lmp_pmic_field){0x1be2u, 0x7fu, 8u};
}

static bool
lmp_pmic_field_equal(struct lmp_pmic_field a, struct lmp_pmic_field b)
{
    return a.addr == b.addr && a.mask == b.mask && a.shift == b.shift;
}

static bool
lmp_pmic_addr_valid(uint16_t addr)
{
    return (addr & 1u) == 0;
}

int
lmp_pmic_validate_registers(const struct lmp_pmic_registers *regs)
{
    struct lmp_pmic_registers known;
    if (!regs)
        return LMP_EINVAL;
    lmp_pmic_mt6358_registers(&known);
    if (regs->swcid_addr != known.swcid_addr ||
        !lmp_pmic_field_equal(regs->vgpu_en, known.vgpu_en) ||
        !lmp_pmic_field_equal(regs->vgpu_da_en, known.vgpu_da_en) ||
        !lmp_pmic_field_equal(regs->vgpu_vosel, known.vgpu_vosel) ||
        !lmp_pmic_field_equal(regs->vgpu_da_vosel, known.vgpu_da_vosel) ||
        !lmp_pmic_field_equal(regs->vsram_gpu_en, known.vsram_gpu_en) ||
        !lmp_pmic_field_equal(regs->vsram_gpu_da_en, known.vsram_gpu_da_en) ||
        !lmp_pmic_field_equal(regs->vsram_gpu_vosel, known.vsram_gpu_vosel) ||
        !lmp_pmic_field_equal(regs->vsram_gpu_da_vosel, known.vsram_gpu_da_vosel))
        return LMP_EINVAL;
    return LMP_OK;
}

static int
lmp_pmic_wrap_read(struct lmp_pmic_state *state, uint32_t offset,
                   uint32_t *value)
{
    if (!state || !value || !state->io.read32 || offset > 0xffcu ||
        (offset & 3u) != 0)
        return LMP_EINVAL;
    return state->io.read32(state->io.cookie, offset, value);
}

static int
lmp_pmic_wrap_write(struct lmp_pmic_state *state, uint32_t offset,
                    uint32_t value)
{
    if (!state || !state->io.write32 || offset > 0xffcu ||
        (offset & 3u) != 0)
        return LMP_EINVAL;
    return state->io.write32(state->io.cookie, offset, value);
}

static int
lmp_pmic_wait_wacs(struct lmp_pmic_state *state, uint32_t wanted_fsm,
                   uint32_t *status_out, bool own_command)
{
    uint64_t start, now, timeout;
    uint32_t status, init_done;
    unsigned poll;
    int error;

    if (!state || !state->io.now_ns || !state->io.delay_us)
        return LMP_EINVAL;
    timeout = state->io.timeout_ns ? state->io.timeout_ns
                                   : LMP_PMIC_WACS_TIMEOUT_NS;
    start = state->io.now_ns(state->io.cookie);
    for (poll = 0; poll < LMP_PMIC_MAX_POLLS; poll++) {
        error = lmp_pmic_wrap_read(state, LMP_PMIC_WACS2_RDATA, &status);
        if (error)
            return error;
        init_done = (status >> 21) & 1u;
        if (!init_done)
            return LMP_ENODEV;
        if (!own_command && (((status >> 19) & 1u) != 0 ||
                             ((status >> 16) & 7u) != LMP_PMIC_FSM_IDLE ||
                              ((status >> 20) & 1u) != LMP_PMIC_SYNC_IDLE))
            return LMP_EBUSY; /* Never wait out or clear a foreign pending command. */
        if (((status >> 16) & 7u) == LMP_PMIC_FSM_WFVLDCLR &&
            wanted_fsm == LMP_PMIC_FSM_IDLE) {
            return LMP_EBUSY;
        } else if (((status >> 16) & 7u) == wanted_fsm &&
                   ((status >> 20) & 1u) == LMP_PMIC_SYNC_IDLE &&
                   (wanted_fsm != LMP_PMIC_FSM_IDLE || ((status >> 19) & 1u) == 0)) {
            if (status_out)
                *status_out = status;
            return LMP_OK;
        }
        now = state->io.now_ns(state->io.cookie);
        if (now - start >= timeout)
            return LMP_ETIMEDOUT;
        state->io.delay_us(state->io.cookie, 1);
    }
    return LMP_ETIMEDOUT;
}

static int
lmp_pmic_read16(struct lmp_pmic_state *state, uint16_t addr, uint16_t *value)
{
    uint32_t cmd, status;
    int error;
    if (!value || !lmp_pmic_addr_valid(addr))
        return LMP_EINVAL;
    error = lmp_pmic_wait_wacs(state, LMP_PMIC_FSM_IDLE, NULL, false);
    if (error)
        return error;
    cmd = ((uint32_t)(addr >> 1) << 16);
    state->command_pending = true; /* a failed write may still have issued CMD */
    error = lmp_pmic_wrap_write(state, LMP_PMIC_WACS2_CMD, cmd);
    if (error)
        return error;
    error = lmp_pmic_wait_wacs(state, LMP_PMIC_FSM_WFVLDCLR, &status, true);
    if (error)
        return error;
    *value = (uint16_t)(status & 0xffffu);
    error = lmp_pmic_wrap_write(state, LMP_PMIC_WACS2_VLDCLR, 1u);
    if (error)
        return error;
    error = lmp_pmic_wait_wacs(state, LMP_PMIC_FSM_IDLE, NULL, true);
    if (!error) state->command_pending = false;
    return error;
}

static int
lmp_pmic_write16(struct lmp_pmic_state *state, uint16_t addr, uint16_t value)
{
    uint32_t cmd;
    int error;
    if (!lmp_pmic_addr_valid(addr))
        return LMP_EINVAL;
    error = lmp_pmic_wait_wacs(state, LMP_PMIC_FSM_IDLE, NULL, false);
    if (error)
        return error;
    cmd = (1u << 31) | ((uint32_t)(addr >> 1) << 16) | value;
    state->command_pending = true;
    error = lmp_pmic_wrap_write(state, LMP_PMIC_WACS2_CMD, cmd);
    if (error)
        return error;
    error = lmp_pmic_wait_wacs(state, LMP_PMIC_FSM_IDLE, NULL, true);
    if (!error) state->command_pending = false;
    return error;
}

static int
lmp_pmic_field_read(struct lmp_pmic_state *state,
                    struct lmp_pmic_field field, uint16_t *value)
{
    uint16_t raw;
    int error = lmp_pmic_read16(state, field.addr, &raw);
    if (error)
        return error;
    *value = (uint16_t)((raw >> field.shift) & field.mask);
    return LMP_OK;
}

static int
lmp_pmic_field_write(struct lmp_pmic_state *state,
                     struct lmp_pmic_field field, uint16_t value)
{
    uint16_t raw;
    int error;
    value &= field.mask;
    error = lmp_pmic_read16(state, field.addr, &raw);
    if (error)
        return error;
    raw = (uint16_t)((raw & ~((uint16_t)(field.mask << field.shift))) |
                     (uint16_t)(value << field.shift));
    return lmp_pmic_write16(state, field.addr, raw);
}

static int
lmp_pmic_probe_status(struct lmp_pmic_state *state)
{
    uint32_t init_done, status, value;
    static const uint32_t offsets[] = { 0x000u, 0x004u, 0x070u, 0x090u };
    static const uint32_t expected[] = { 0u, 1u, 0u, 1u };
    unsigned i;
    int error;
    for (i = 0; i < 4; i++) {
        error = lmp_pmic_wrap_read(state, offsets[i], &value);
        if (error)
            return error;
        if ((value & 1u) != expected[i])
            return LMP_ENOTSUP;
    }
    error = lmp_pmic_wrap_read(state, LMP_PMIC_INIT_DONE2, &init_done);
    if (error)
        return error;
    if ((init_done & 1u) == 0)
        return LMP_ENODEV;
    error = lmp_pmic_wait_wacs(state, LMP_PMIC_FSM_IDLE, &status, false);
    if (error)
        return error;
    if (((status >> 16) & 7u) != LMP_PMIC_FSM_IDLE ||
        ((status >> 20) & 1u) != LMP_PMIC_SYNC_IDLE)
        return LMP_EIO;
    return LMP_OK;
}

int
lmp_pmic_probe(struct lmp_pmic_state *state, const struct lmp_pmic_io *io,
               const struct lmp_pmic_registers *regs)
{
    uint16_t chip_id;
    int error;
    if (!state || !io || !regs || !io->read32 || !io->write32 ||
        !io->now_ns || !io->delay_us)
        return LMP_EINVAL;
    if (!io->acquire || !io->release || !io->poison)
        return LMP_ENOTSUP; /* No PWRAP read/write without real exclusivity. */
    error = lmp_pmic_validate_registers(regs);
    if (error)
        return error;
    lmp_pmic_zero(state, (uint32_t)sizeof(*state));
    state->io = *io;
    state->regs = *regs;
    error = io->acquire(io->cookie, io->timeout_ns ? io->timeout_ns : LMP_PMIC_WACS_TIMEOUT_NS);
    if (error)
        return error;
    error = lmp_pmic_probe_status(state);
    if (!error)
        error = lmp_pmic_read16(state, state->regs.swcid_addr, &chip_id);
    if (!error && chip_id != LMP_PMIC_CID_E1 && chip_id != LMP_PMIC_CID_E2 &&
        chip_id != LMP_PMIC_CID_E3)
        error = LMP_ENODEV;
    if (!error) {
        state->chip_id = chip_id;
        state->validated = true;
    }
    if (state->command_pending) {
        state->validated = false;
        io->poison(io->cookie);
    }
    io->release(io->cookie);
    return error;
}

static bool
lmp_pmic_valid_rail(enum lmp_rail rail)
{
    return rail == LMP_RAIL_VGPU || rail == LMP_RAIL_VSRAM_GPU;
}

static struct lmp_pmic_field
lmp_pmic_en_field(const struct lmp_pmic_registers *regs, enum lmp_rail rail,
                  bool da)
{
    if (rail == LMP_RAIL_VGPU)
        return da ? regs->vgpu_da_en : regs->vgpu_en;
    return da ? regs->vsram_gpu_da_en : regs->vsram_gpu_en;
}

static struct lmp_pmic_field
lmp_pmic_vosel_field(const struct lmp_pmic_registers *regs, enum lmp_rail rail,
                     bool da)
{
    if (rail == LMP_RAIL_VGPU)
        return da ? regs->vgpu_da_vosel : regs->vgpu_vosel;
    return da ? regs->vsram_gpu_da_vosel : regs->vsram_gpu_vosel;
}

static int
lmp_pmic_is_enabled_locked(struct lmp_pmic_state *state, enum lmp_rail rail,
                    bool *enabled)
{
    uint16_t value;
    int error;
    if (!state || !state->validated || !enabled || !lmp_pmic_valid_rail(rail))
        return LMP_EINVAL;
    error = lmp_pmic_field_read(state, lmp_pmic_en_field(&state->regs, rail,
                                                          true), &value);
    if (error)
        return error;
    *enabled = value != 0;
    state->rail_enabled[rail] = *enabled;
    return LMP_OK;
}

static int
lmp_pmic_wait_enabled(struct lmp_pmic_state *state, enum lmp_rail rail,
                      bool wanted)
{
    uint64_t start, now, timeout;
    bool enabled;
    unsigned poll;
    int error;
    start = state->io.now_ns(state->io.cookie);
    timeout = state->io.timeout_ns ? state->io.timeout_ns
                                   : LMP_PMIC_WACS_TIMEOUT_NS;
    for (poll = 0; poll < LMP_PMIC_MAX_POLLS; poll++) {
        error = lmp_pmic_is_enabled_locked(state, rail, &enabled);
        if (error)
            return error;
        if (enabled == wanted)
            return LMP_OK;
        now = state->io.now_ns(state->io.cookie);
        if (now - start >= timeout)
            return LMP_ETIMEDOUT;
        state->io.delay_us(state->io.cookie, 1);
    }
    return LMP_ETIMEDOUT;
}

static int
lmp_pmic_enable_locked(struct lmp_pmic_state *state, enum lmp_rail rail)
{
    int error;
    if (!state || !state->validated || !lmp_pmic_valid_rail(rail))
        return LMP_EINVAL;
    error = lmp_pmic_field_write(state,
                                 lmp_pmic_en_field(&state->regs, rail, false),
                                 1u);
    if (error)
        return error;
    state->io.delay_us(state->io.cookie,
                        rail == LMP_RAIL_VGPU ? 200u : 240u);
    error = lmp_pmic_wait_enabled(state, rail, true);
    if (!error)
        state->rail_enabled[rail] = true;
    return error;
}

static int
lmp_pmic_disable_locked(struct lmp_pmic_state *state, enum lmp_rail rail)
{
    int error;
    if (!state || !state->validated || !lmp_pmic_valid_rail(rail))
        return LMP_EINVAL;
    error = lmp_pmic_field_write(state,
                                 lmp_pmic_en_field(&state->regs, rail, false),
                                 0u);
    if (error)
        return error;
    error = lmp_pmic_wait_enabled(state, rail, false);
    if (!error)
        state->rail_enabled[rail] = false;
    return error;
}

static int
lmp_pmic_get_voltage_uv_locked(struct lmp_pmic_state *state, enum lmp_rail rail,
                        uint32_t *uv)
{
    uint16_t selector;
    bool enabled;
    int error;
    if (!state || !state->validated || !uv || !lmp_pmic_valid_rail(rail))
        return LMP_EINVAL;
    error = lmp_pmic_is_enabled_locked(state, rail, &enabled);
    if (error)
        return error;
    error = lmp_pmic_field_read(state,
        lmp_pmic_vosel_field(&state->regs, rail, enabled), &selector);
    if (error)
        return error;
    *uv = 500000u + (uint32_t)selector * 6250u;
    return LMP_OK;
}

static int
lmp_pmic_set_voltage_uv_locked(struct lmp_pmic_state *state, enum lmp_rail rail,
                        uint32_t uv)
{
    uint32_t selector;
    int error;
    if (!state || !state->validated || !lmp_pmic_valid_rail(rail))
        return LMP_EINVAL;
    /* MT6358 DTS: 500mV..1293.75mV in 6.25mV steps for both rails. */
    if (uv < 500000u || uv > 1293750u || ((uv - 500000u) % 6250u) != 0)
        return LMP_EINVAL;
    selector = (uv - 500000u) / 6250u;
    if (selector > 0x7fu)
        return LMP_EINVAL;
    error = lmp_pmic_field_write(state,
                                 lmp_pmic_vosel_field(&state->regs, rail,
                                                      false),
                                 (uint16_t)selector);
    if (error)
        return error;
    /* Read DA when enabled and RG when off; disabled DA can be stale. */
    {
        uint32_t actual;
        state->io.delay_us(state->io.cookie, 240u);
        error = lmp_pmic_get_voltage_uv_locked(state, rail, &actual);
        if (error)
            return error;
        return actual == uv ? LMP_OK : LMP_EIO;
    }
}

/* Hold the external WACS2 arbiter across the entire provider operation,
 * including field read/modify/write, voltage/DA verification and settle waits.
 * Failure after a transaction conservatively requires a new probe: never
 * consume a pending response in a later unrelated operation. */
static int
lmp_pmic_transaction_begin(struct lmp_pmic_state *state)
{
    if (!state || !state->validated || !state->io.acquire || !state->io.release)
        return LMP_ENODEV;
    return state->io.acquire(state->io.cookie, state->io.timeout_ns ?
        state->io.timeout_ns : LMP_PMIC_WACS_TIMEOUT_NS);
}
static int
lmp_pmic_transaction_end(struct lmp_pmic_state *state, int error)
{
    if (state->command_pending) {
        state->validated = false;
        state->io.poison(state->io.cookie);
    }
    if (error && error != LMP_EINVAL)
        state->validated = false;
    state->io.release(state->io.cookie);
    return error;
}
int
lmp_pmic_is_enabled(struct lmp_pmic_state *state, enum lmp_rail rail, bool *value)
{
    int error = lmp_pmic_transaction_begin(state);
    if (error) return error;
    return lmp_pmic_transaction_end(state, lmp_pmic_is_enabled_locked(state, rail, value));
}
int
lmp_pmic_enable(struct lmp_pmic_state *state, enum lmp_rail rail)
{
    int error = lmp_pmic_transaction_begin(state);
    if (error) return error;
    return lmp_pmic_transaction_end(state, lmp_pmic_enable_locked(state, rail));
}
int
lmp_pmic_disable(struct lmp_pmic_state *state, enum lmp_rail rail)
{
    int error = lmp_pmic_transaction_begin(state);
    if (error) return error;
    return lmp_pmic_transaction_end(state, lmp_pmic_disable_locked(state, rail));
}
int
lmp_pmic_get_voltage_uv(struct lmp_pmic_state *state, enum lmp_rail rail, uint32_t *uv)
{
    int error = lmp_pmic_transaction_begin(state);
    if (error) return error;
    return lmp_pmic_transaction_end(state, lmp_pmic_get_voltage_uv_locked(state, rail, uv));
}
int
lmp_pmic_set_voltage_uv(struct lmp_pmic_state *state, enum lmp_rail rail, uint32_t uv)
{
    int error = lmp_pmic_transaction_begin(state);
    if (error) return error;
    return lmp_pmic_transaction_end(state, lmp_pmic_set_voltage_uv_locked(state, rail, uv));
}

static int
lmp_pmic_cb_is_enabled(void *cookie, enum lmp_rail rail, bool *enabled)
{
    return lmp_pmic_is_enabled((struct lmp_pmic_state *)cookie, rail, enabled);
}

static int
lmp_pmic_cb_enable(void *cookie, enum lmp_rail rail)
{
    return lmp_pmic_enable((struct lmp_pmic_state *)cookie, rail);
}

static int
lmp_pmic_cb_disable(void *cookie, enum lmp_rail rail)
{
    return lmp_pmic_disable((struct lmp_pmic_state *)cookie, rail);
}

static int
lmp_pmic_cb_set_voltage(void *cookie, enum lmp_rail rail, uint32_t uv)
{
    return lmp_pmic_set_voltage_uv((struct lmp_pmic_state *)cookie, rail, uv);
}

static int
lmp_pmic_cb_get_voltage(void *cookie, enum lmp_rail rail, uint32_t *uv)
{
    return lmp_pmic_get_voltage_uv((struct lmp_pmic_state *)cookie, rail, uv);
}

int
lmp_pmic_get_platform_ops(struct lmp_pmic_state *state,
                          struct lmp_pmic_ops *ops)
{
    if (!state || !ops || !state->validated)
        return LMP_EINVAL;
    lmp_pmic_zero(ops, (uint32_t)sizeof(*ops));
    ops->cookie = state;
    ops->is_enabled = lmp_pmic_cb_is_enabled;
    ops->enable = lmp_pmic_cb_enable;
    ops->disable = lmp_pmic_cb_disable;
    ops->set_voltage_uv = lmp_pmic_cb_set_voltage;
    ops->get_voltage_uv = lmp_pmic_cb_get_voltage;
    return LMP_OK;
}
