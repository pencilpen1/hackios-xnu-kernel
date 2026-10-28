#include "lancelot_mali_platform.h"

/*
 * This file deliberately has no XNU, Linux, IOKit, or PMIC bus dependency.
 * The XNU side maps each bank lazily and supplies lmp_io_ops.  The PMIC side
 * must be backed by the MT6358 regulator provider (vgpu/vsram_gpu); a missing
 * provider is an explicit unsupported result.
 */

static uint32_t
lmp_bank_size(enum lmp_bank bank)
{
    switch (bank) {
    case LMP_BANK_SPM:
    case LMP_BANK_INFRA:
    case LMP_BANK_MFGCFG:
        return 0x1000u;
    case LMP_BANK_GPU:
    case LMP_BANK_TOP:
    case LMP_BANK_APMIXED:
        return bank == LMP_BANK_GPU ? 0x4000u : 0x1000u;
    default:
        return 0;
    }
}

static bool
lmp_valid_offset(enum lmp_bank bank, uint32_t offset)
{
    uint32_t size = lmp_bank_size(bank);
    return size != 0 && (offset & 3u) == 0 && offset < size;
}

static void
lmp_clear_state(struct lmp_state *state)
{
    size_t i;
    uint8_t *bytes = (uint8_t *)state;
    for (i = 0; i < sizeof(*state); i++)
        bytes[i] = 0;
}

static int
lmp_read(struct lmp_state *state, enum lmp_bank bank, uint32_t offset,
         uint32_t *value)
{
    if (!state || !value || !state->ops.read32 ||
        !lmp_valid_offset(bank, offset))
        return LMP_EINVAL;
    return state->ops.read32(state->ops.cookie, bank, offset, value);
}

static int
lmp_write(struct lmp_state *state, enum lmp_bank bank, uint32_t offset,
          uint32_t value)
{
    /* This branch is also the structural guarantee for SPM-only zero writes. */
    if (!state || state->config.mode != LMP_MODE_FULL)
        return LMP_ENOTSUP;
    if (!state->ops.write32 || !lmp_valid_offset(bank, offset))
        return LMP_EINVAL;
    return state->ops.write32(state->ops.cookie, bank, offset, value);
}

static int
lmp_require_full(struct lmp_state *state)
{
    if (!state || !state->initialized)
        return LMP_EINVAL;
    if (state->config.mode != LMP_MODE_FULL)
        return LMP_ENOTSUP;
    if (state->failed)
        return LMP_EBUSY;
    return LMP_OK;
}

static int
lmp_failed(struct lmp_state *state, int error)
{
    if (state && error != LMP_OK)
        state->failed = true;
    return error;
}

static uint64_t
lmp_timeout(const struct lmp_state *state)
{
    return state->config.timeout_ns ? state->config.timeout_ns
                                    : LMP_DEFAULT_TIMEOUT_NS;
}

/*
 * Every polling operation has both a time deadline and a finite iteration
 * ceiling.  The second limit protects against a broken test/XNU clock that
 * never advances.
 */
static int
lmp_wait_mask(struct lmp_state *state, enum lmp_bank bank, uint32_t offset,
              uint32_t mask, uint32_t expected)
{
    uint64_t start, now;
    uint32_t value;
    unsigned poll;
    int error;

    if (!state->ops.now_ns || !state->ops.delay_us)
        return LMP_EINVAL;
    start = state->ops.now_ns(state->ops.cookie);
    for (poll = 0; poll < LMP_MAX_POLLS; poll++) {
        error = lmp_read(state, bank, offset, &value);
        if (error)
            return error;
        if ((value & mask) == expected)
            return LMP_OK;
        now = state->ops.now_ns(state->ops.cookie);
        if (now - start >= lmp_timeout(state))
            return LMP_ETIMEDOUT;
        state->ops.delay_us(state->ops.cookie, 1);
    }
    return LMP_ETIMEDOUT;
}

static int
lmp_wait_domains(struct lmp_state *state, uint32_t mask, bool on)
{
    uint64_t start, now;
    uint32_t status1, status2;
    unsigned poll;
    int error;

    if (!state->ops.now_ns || !state->ops.delay_us)
        return LMP_EINVAL;
    start = state->ops.now_ns(state->ops.cookie);
    for (poll = 0; poll < LMP_MAX_POLLS; poll++) {
        error = lmp_read(state, LMP_BANK_SPM, LMP_SPM_PWR_STATUS, &status1);
        if (error)
            return error;
        error = lmp_read(state, LMP_BANK_SPM, LMP_SPM_PWR_STATUS_2ND,
                         &status2);
        if (error)
            return error;
        if (on) {
            if ((status1 & mask) == mask && (status2 & mask) == mask)
                return LMP_OK;
        } else if ((status1 & mask) == 0 && (status2 & mask) == 0) {
            return LMP_OK;
        }
        now = state->ops.now_ns(state->ops.cookie);
        if (now - start >= lmp_timeout(state))
            return LMP_ETIMEDOUT;
        state->ops.delay_us(state->ops.cookie, 1);
    }
    return LMP_ETIMEDOUT;
}

static int
lmp_read_upstream(struct lmp_state *state, enum lmp_bank bank,
                  uint32_t offset, uint32_t *value)
{
    return lmp_read(state, bank, offset, value);
}

int
lmp_upstream_clock_ready(struct lmp_state *state)
{
    uint32_t top, con0, con3, con1, parent;
    uint32_t con0_offset, con3_offset, divisor;
    uint64_t denominator, hz;
    int error;

    error = lmp_require_full(state);
    if (error)
        return error;
    error = lmp_read_upstream(state, LMP_BANK_TOP, LMP_TOP_CLK_CFG_1, &top);
    if (error)
        return lmp_failed(state, error);
    state->initial_top_mfg_cfg = top;
    parent = (top >> LMP_TOP_MFG_SEL_SHIFT) & LMP_TOP_MFG_SEL_MASK;
    /* clk-mt6768.c mfg_parents: clk26m, mfgpll, syspll_d3, univpll_d3. */
    if (top & (1u << 7))
        return lmp_failed(state, LMP_ENODEV);
    if (parent == 0) {
        state->source_hz = 26000000u;
        return LMP_OK;
    }
    divisor = parent == 1 ? 1u : parent == 2 ? 3u : 6u;
    if (parent == 1) {
        con0_offset = LMP_APMIXED_MFGPLL_CON0;
        con3_offset = LMP_APMIXED_MFGPLL_CON3;
    } else if (parent == 2) {
        con0_offset = LMP_APMIXED_MAINPLL_CON0;
        con3_offset = LMP_APMIXED_MAINPLL_CON3;
    } else if (parent == 3) {
        con0_offset = LMP_APMIXED_UNIVPLL_CON0;
        con3_offset = LMP_APMIXED_UNIVPLL_CON3;
    } else {
        return lmp_failed(state, LMP_EIO);
    }
    error = lmp_read_upstream(state, LMP_BANK_APMIXED, con0_offset, &con0);
    if (error)
        return lmp_failed(state, error);
    error = lmp_read_upstream(state, LMP_BANK_APMIXED, con3_offset, &con3);
    if (error)
        return lmp_failed(state, error);
    if (parent == 1) {
        state->initial_mfgpll_con0 = con0;
        state->initial_mfgpll_con3 = con3;
    } else {
        state->initial_parent_pll_con0 = con0;
        state->initial_parent_pll_con3 = con3;
    }
    if ((con0 & LMP_PLL_EN) == 0 || (con3 & LMP_PLL_PWR_ON) == 0 ||
        (con3 & LMP_PLL_ISO_EN) != 0)
        return lmp_failed(state, LMP_ENODEV);
    error = lmp_read(state, LMP_BANK_APMIXED, con0_offset + 4u, &con1);
    if (error)
        return lmp_failed(state, error);
    denominator = UINT64_C(16384) * (1u << ((con1 >> 24) & 7u)) * divisor;
    hz = (UINT64_C(26000000) * (con1 & 0x3fffffu) + denominator - 1u) /
         denominator;
    if (!hz || hz > UINT64_C(1000000000))
        return lmp_failed(state, LMP_ENOTSUP);
    state->source_hz = (uint32_t)hz;
    return LMP_OK;
}

/* Conservative stock OPP floor, pinned mtk_gpufreq_core.h at 6dd120fc.
 * Higher voltage than required is accepted only within source rail maxima. */
static uint32_t
lmp_required_vgpu(uint32_t hz)
{
    static const uint32_t khz[32] = {
        1000000,975000,950000,925000,900000,875000,850000,823000,
        796000,769000,743000,716000,690000,663000,637000,611000,
        586000,560000,535000,509000,484000,467000,450000,434000,
        417000,400000,383000,366000,349000,332000,315000,299000
    };
    static const uint32_t uv[32] = {
        950000,925000,900000,875000,850000,825000,800000,793750,
        781250,768750,756250,750000,737500,725000,712500,706250,
        700000,693750,687500,681250,668750,668750,662500,656250,
        650000,643750,643750,637500,631250,625000,618750,612500
    };
    unsigned i;
    for (i = 31; i > 0; i--)
        if (hz <= khz[i] * 1000u)
            return uv[i];
    return uv[0];
}

static int
lmp_check_rails(struct lmp_state *state, bool require_enabled)
{
    unsigned i;
    bool enabled;
    uint32_t gpu, sram;
    int error;
    if (!state->ops.pmic.is_enabled || !state->ops.pmic.get_voltage_uv)
        return LMP_ENOTSUP;
    for (i = 0; i < LMP_RAIL_COUNT; i++) {
        error = state->ops.pmic.is_enabled(state->ops.pmic.cookie,
                                           (enum lmp_rail)i, &enabled);
        if (error)
            return error;
        if (require_enabled && !enabled)
            return LMP_ENODEV;
        error = state->ops.pmic.get_voltage_uv(state->ops.pmic.cookie,
                            (enum lmp_rail)i, &state->observed_uv[i]);
        if (error)
            return error;
    }
    gpu = state->observed_uv[LMP_RAIL_VGPU];
    sram = state->observed_uv[LMP_RAIL_VSRAM_GPU];
    if (gpu < lmp_required_vgpu(state->source_hz) || gpu > 950000u ||
        sram < 850000u || sram > 1050000u || sram < gpu ||
        sram - gpu < 100000u || sram - gpu > 250000u)
        return LMP_ENOTSUP;
    return LMP_OK;
}

static int
lmp_check_pwr_con(struct lmp_state *state)
{
    static const uint32_t offsets[4] = {
        LMP_SPM_MFG_PWR_CON, LMP_SPM_MFG_CORE0_PWR_CON,
        LMP_SPM_MFG_CORE1_PWR_CON, LMP_SPM_MFG_ASYNC_PWR_CON
    };
    const uint32_t required = LMP_PWR_RST_B | LMP_PWR_ON | LMP_PWR_ON_2ND;
    unsigned i;
    uint32_t value;
    int error;
    for (i = 0; i < 4; i++) {
        error = lmp_read(state, LMP_BANK_SPM, offsets[i], &value);
        if (error)
            return error;
        if ((value & required) != required ||
            (value & (LMP_PWR_ISO | LMP_PWR_CLK_DIS)) != 0 ||
            (i != 3 && (value & (LMP_SRAM_PDN | LMP_SRAM_PDN_ACK)) != 0))
            return LMP_ENODEV;
    }
    return LMP_OK;
}

static int
lmp_read_pwr_con(struct lmp_state *state, uint32_t offset, uint32_t *value)
{
    return lmp_read(state, LMP_BANK_SPM, offset, value);
}

static int
lmp_update_pwr_con(struct lmp_state *state, uint32_t offset,
                   uint32_t set_bits, uint32_t clear_bits)
{
    uint32_t value;
    int error = lmp_read_pwr_con(state, offset, &value);
    if (error)
        return error;
    value |= set_bits;
    value &= ~clear_bits;
    return lmp_write(state, LMP_BANK_SPM, offset, value);
}

static bool
lmp_domain_is_on(const struct lmp_status *status, uint32_t mask)
{
    return status && status->domain_ack_match &&
           (status->pwr_status & mask) == mask &&
           (status->pwr_status_2nd & mask) == mask;
}

static int
lmp_read_bus_state(struct lmp_state *state, uint32_t *protect,
                   uint32_t *ack, bool *protected)
{
    int error;
    error = lmp_read(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN, protect);
    if (error)
        return error;
    error = lmp_read(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_STA1, ack);
    if (error)
        return error;
    if (((*protect ^ *ack) & LMP_MFG_PROT_MASK) != 0)
        return LMP_EIO;
    *protected = ((*protect & LMP_MFG_PROT_MASK) == LMP_MFG_PROT_MASK);
    return LMP_OK;
}

static int
lmp_set_bus_protected(struct lmp_state *state)
{
    uint32_t protect, ack;
    bool already;
    int error;

    error = lmp_read_bus_state(state, &protect, &ack, &already);
    if (error)
        return error;
    if (already)
        return LMP_OK;

    error = lmp_write(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_SET,
                      LMP_MFG_PROT_STEP1_MASK);
    if (error)
        return error;
    error = lmp_wait_mask(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_STA1,
                          LMP_MFG_PROT_STEP1_MASK, LMP_MFG_PROT_STEP1_MASK);
    if (error)
        return error;
    error = lmp_write(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_SET,
                      LMP_MFG_PROT_STEP2_MASK);
    if (error)
        return error;
    return lmp_wait_mask(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_STA1,
                         LMP_MFG_PROT_STEP2_MASK, LMP_MFG_PROT_STEP2_MASK);
}

static int
lmp_set_bus_unprotected(struct lmp_state *state)
{
    int error;
    error = lmp_write(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_CLR,
                      LMP_MFG_PROT_STEP2_MASK);
    if (error)
        return error;
    error = lmp_write(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_CLR,
                      LMP_MFG_PROT_STEP1_MASK);
    if (error)
        return error;
    /* The pinned source ignores release ACKs; bounded verification is safer. */
    return lmp_wait_mask(state, LMP_BANK_INFRA, LMP_INFRA_PROTECTEN_STA1,
                         LMP_MFG_PROT_MASK, 0);
}

static int
lmp_enable_rail(struct lmp_state *state, enum lmp_rail rail)
{
    bool enabled;
    int error;

    if (!state->ops.pmic.is_enabled)
        return LMP_ENOTSUP;
    error = state->ops.pmic.is_enabled(state->ops.pmic.cookie, rail,
                                        &enabled);
    if (error)
        return error;
    if (enabled)
        return LMP_OK;
    if (!state->ops.pmic.enable)
        return LMP_ENOTSUP;
    error = state->ops.pmic.enable(state->ops.pmic.cookie, rail);
    if (!error)
        state->rail_owned[rail] = true;
    return error;
}

static int
lmp_disable_rail(struct lmp_state *state, enum lmp_rail rail)
{
    if (!state->ops.pmic.disable)
        return LMP_ENOTSUP;
    return state->ops.pmic.disable(state->ops.pmic.cookie, rail);
}

static int
lmp_power_domain_on(struct lmp_state *state, uint32_t pwr_offset,
                    uint32_t status_mask, unsigned domain_index,
                    bool has_sram)
{
    int error;

    if (state->initial_domain_on[domain_index])
        return LMP_OK;
    error = lmp_update_pwr_con(state, pwr_offset, LMP_PWR_ON, 0);
    if (error)
        return error;
    error = lmp_update_pwr_con(state, pwr_offset, LMP_PWR_ON_2ND, 0);
    if (error)
        return error;
    error = lmp_wait_domains(state, status_mask, true);
    if (error)
        return error;
    state->domain_owned[domain_index] = true;
    error = lmp_update_pwr_con(state, pwr_offset,
                               0, LMP_PWR_CLK_DIS | LMP_PWR_ISO);
    if (error)
        return error;
    error = lmp_update_pwr_con(state, pwr_offset, LMP_PWR_RST_B, 0);
    if (error)
        return error;
    if (has_sram) {
        error = lmp_update_pwr_con(state, pwr_offset, 0, LMP_SRAM_PDN);
        if (error)
            return error;
        error = lmp_wait_mask(state, LMP_BANK_SPM, pwr_offset,
                              LMP_SRAM_PDN_ACK, 0);
        if (error)
            return error;
    }
    return LMP_OK;
}

static int
lmp_power_domain_off(struct lmp_state *state, uint32_t pwr_offset,
                     uint32_t status_mask, unsigned domain_index,
                     bool has_sram)
{
    int error;

    if (!state->domain_owned[domain_index])
        return LMP_OK;
    if (has_sram) {
        error = lmp_update_pwr_con(state, pwr_offset, LMP_SRAM_PDN, 0);
        if (error)
            return error;
        error = lmp_wait_mask(state, LMP_BANK_SPM, pwr_offset,
                              LMP_SRAM_PDN_ACK, LMP_SRAM_PDN_ACK);
        if (error)
            return error;
    }
    error = lmp_update_pwr_con(state, pwr_offset,
                               LMP_PWR_ISO | LMP_PWR_CLK_DIS,
                               LMP_PWR_RST_B | LMP_PWR_ON | LMP_PWR_ON_2ND);
    if (error)
        return error;
    error = lmp_wait_domains(state, status_mask, false);
    if (!error)
        state->domain_owned[domain_index] = false;
    return error;
}

int
lmp_init(struct lmp_state *state, const struct lmp_config *config,
         const struct lmp_io_ops *ops)
{
    if (!state || !config)
        return LMP_EINVAL;
    lmp_clear_state(state);
    if (config->platform != LMP_PLATFORM_LANCELOT_MT6768)
        return LMP_ENOTSUP;
    if (config->mode != LMP_MODE_OFF &&
        config->mode != LMP_MODE_SPM_ONLY &&
        config->mode != LMP_MODE_FULL)
        return LMP_EINVAL;
    if (config->mode != LMP_MODE_OFF && (!ops || !ops->read32))
        return LMP_EINVAL;
    if (config->mode == LMP_MODE_FULL &&
        (!ops->write32 || !ops->now_ns || !ops->delay_us))
        return LMP_EINVAL;
    state->config = *config;
    if (!state->config.timeout_ns)
        state->config.timeout_ns = LMP_DEFAULT_TIMEOUT_NS;
    if (ops)
        state->ops = *ops;
    state->initialized = true;
    return LMP_OK;
}

int
lmp_status(struct lmp_state *state, struct lmp_status *status)
{
    uint32_t s1, s2;
    int error;

    if (!state || !state->initialized || !status)
        return LMP_EINVAL;
    if (state->config.mode == LMP_MODE_OFF) {
        uint8_t *bytes = (uint8_t *)status;
        size_t i;
        for (i = 0; i < sizeof(*status); i++)
            bytes[i] = 0;
        return LMP_OK;
    }
    error = lmp_read(state, LMP_BANK_SPM, LMP_SPM_PWR_STATUS, &s1);
    if (error)
        return lmp_failed(state, error);
    error = lmp_read(state, LMP_BANK_SPM, LMP_SPM_PWR_STATUS_2ND, &s2);
    if (error)
        return lmp_failed(state, error);
    status->pwr_status = s1;
    status->pwr_status_2nd = s2;
    status->domain_mask = LMP_MFG_DOMAIN_MASK;
    status->domain_ack_match = ((s1 ^ s2) & LMP_MFG_DOMAIN_MASK) == 0;
    status->domain_on = lmp_domain_is_on(status, LMP_MFG_DOMAIN_MASK);
    status->gpu_accessible = state->gpu_ready && !state->failed;
    state->status = *status;
    return LMP_OK;
}

int
lmp_prepare(struct lmp_state *state)
{
    struct lmp_status status;
    uint32_t value, protect, ack;
    bool bus_protected;
    int error;

    if (!state || !state->initialized)
        return LMP_EINVAL;
    if (state->config.mode == LMP_MODE_OFF)
        return LMP_OK;
    if (state->failed)
        return LMP_EBUSY;
    error = lmp_status(state, &status);
    if (error)
        return error;
    if (state->config.mode == LMP_MODE_SPM_ONLY) {
        state->prepared = true;
        return LMP_OK;
    }
    error = lmp_upstream_clock_ready(state);
    if (error)
        return error;
    error = lmp_read_pwr_con(state, LMP_SPM_MFG_PWR_CON, &value);
    if (error)
        return lmp_failed(state, error);
    state->initial_pwr_con[0] = value;
    error = lmp_read_pwr_con(state, LMP_SPM_MFG_CORE0_PWR_CON, &value);
    if (error)
        return lmp_failed(state, error);
    state->initial_pwr_con[1] = value;
    error = lmp_read_pwr_con(state, LMP_SPM_MFG_CORE1_PWR_CON, &value);
    if (error)
        return lmp_failed(state, error);
    state->initial_pwr_con[2] = value;
    error = lmp_read_pwr_con(state, LMP_SPM_MFG_ASYNC_PWR_CON, &value);
    if (error)
        return lmp_failed(state, error);
    state->initial_async_pwr_con = value;
    state->initial_domain_on[0] = lmp_domain_is_on(&status, LMP_MFG_PWR_STA_MASK);
    state->initial_domain_on[1] = lmp_domain_is_on(&status, LMP_MFG_CORE0_STA_MASK);
    state->initial_domain_on[2] = lmp_domain_is_on(&status, LMP_MFG_CORE1_STA_MASK);
    state->initial_domain_on[3] = lmp_domain_is_on(&status, LMP_MFG_ASYNC_STA_MASK);
    error = lmp_read_bus_state(state, &protect, &ack, &bus_protected);
    if (error)
        return lmp_failed(state, error);
    state->initial_protect = protect;
    state->initial_protect_ack = ack;
    state->initial_bus_protected = bus_protected;
    if (!status.domain_ack_match)
        return lmp_failed(state, LMP_EIO);
    /* Refuse partial inherited domain trees: changing their parent ownership
     * cannot be proved safe without the original owner's reference counts. */
    if ((status.pwr_status & LMP_MFG_DOMAIN_MASK) != 0 && !status.domain_on)
        return lmp_failed(state, LMP_ENOTSUP);
    if (status.domain_on) {
        error = lmp_check_pwr_con(state);
        if (error)
            return lmp_failed(state, error);
        if ((protect & LMP_MFG_PROT_MASK) != 0 ||
            (ack & LMP_MFG_PROT_MASK) != 0)
            return lmp_failed(state, LMP_ENODEV);
        error = lmp_check_rails(state, true);
        if (error)
            return lmp_failed(state, error);
        error = lmp_read(state, LMP_BANK_MFGCFG, LMP_MFG_CG_CON, &value);
        if (error)
            return lmp_failed(state, error);
        state->initial_cg_con = value;
        state->initial_clock_on = (value & 1u) == 0;
        state->initial_reset_released =
            (state->initial_pwr_con[0] & LMP_PWR_RST_B) != 0;
    }
    state->prepared = true;
    return LMP_OK;
}

int
lmp_power_on(struct lmp_state *state)
{
    int error;

    error = lmp_require_full(state);
    if (error)
        return error;
    if (!state->prepared) {
        error = lmp_prepare(state);
        if (error)
            return error;
    }
    if (state->status.domain_on)
        return LMP_OK;
    if (!state->status.domain_ack_match)
        return lmp_failed(state, LMP_EIO);

    /* Program the source boot pair only while BOTH rails and all domains
     * are off. Never retune an inherited live regulator or PLL. */
    {
        unsigned i;
        if (!state->ops.pmic.is_enabled || !state->ops.pmic.get_voltage_uv)
            return lmp_failed(state, LMP_ENOTSUP);
        for (i = 0; i < LMP_RAIL_COUNT; i++) {
            error = state->ops.pmic.is_enabled(state->ops.pmic.cookie,
                      (enum lmp_rail)i, &state->initial_rail_on[i]);
            if (error)
                return lmp_failed(state, error);
        }
        /* A read-only provider can preserve an already-live rail pair.
         * Missing writers must reject before any resource write if either
         * rail is off, rather than requiring no-op enable/disable callbacks. */
        if ((!state->initial_rail_on[0] || !state->initial_rail_on[1]) &&
            (!state->ops.pmic.enable || !state->ops.pmic.disable))
            return lmp_failed(state, LMP_ENOTSUP);
        if (!state->initial_rail_on[0] && !state->initial_rail_on[1]) {
            if (!state->ops.pmic.set_voltage_uv)
                return lmp_failed(state, LMP_ENOTSUP);
            error = state->ops.pmic.set_voltage_uv(state->ops.pmic.cookie,
                                      LMP_RAIL_VSRAM_GPU, 1050000u);
            if (error)
                return lmp_failed(state, error);
            error = state->ops.pmic.set_voltage_uv(state->ops.pmic.cookie,
                                      LMP_RAIL_VGPU, 950000u);
            if (error)
                return lmp_failed(state, error);
        }
        error = lmp_check_rails(state, false);
        if (error)
            return lmp_failed(state, error);
    }
    error = lmp_enable_rail(state, LMP_RAIL_VGPU);
    if (error)
        return lmp_failed(state, error);
    error = lmp_enable_rail(state, LMP_RAIL_VSRAM_GPU);
    if (error)
        return lmp_failed(state, error);
    error = lmp_check_rails(state, true);
    if (error)
        return lmp_failed(state, error);

    /* Protect before touching a domain that may have outstanding AXI traffic. */
    error = lmp_set_bus_protected(state);
    if (error)
        return lmp_failed(state, error);
    if (!state->initial_bus_protected)
        state->bus_owned = true;

    /* Order mirrors clk-mt6768-pg.c bring-up: async, MFG, release bus,
     * then MFG core0/core1 SRAM domains. */
    error = lmp_power_domain_on(state, LMP_SPM_MFG_ASYNC_PWR_CON,
                                LMP_MFG_ASYNC_STA_MASK, 3, false);
    if (error)
        return lmp_failed(state, error);
    error = lmp_power_domain_on(state, LMP_SPM_MFG_PWR_CON,
                                LMP_MFG_PWR_STA_MASK, 0, false);
    if (error)
        return lmp_failed(state, error);
    /* clk-mt6768-pg.c releases MFG SRAM then protection before core0/core1. */
    error = lmp_update_pwr_con(state, LMP_SPM_MFG_PWR_CON, 0, LMP_SRAM_PDN);
    if (error)
        return lmp_failed(state, error);
    error = lmp_wait_mask(state, LMP_BANK_SPM, LMP_SPM_MFG_PWR_CON,
                          LMP_SRAM_PDN_ACK, 0);
    if (error)
        return lmp_failed(state, error);
    {
        uint32_t protect, ack;
        bool protected;
        error = lmp_read_bus_state(state, &protect, &ack, &protected);
        if (error)
            return lmp_failed(state, error);
        if (protected) {
            state->bus_owned = state->initial_bus_protected;
            error = lmp_set_bus_unprotected(state);
            if (error)
                return lmp_failed(state, error);
        }
    }
    error = lmp_power_domain_on(state, LMP_SPM_MFG_CORE0_PWR_CON,
                                LMP_MFG_CORE0_STA_MASK, 1, true);
    if (error)
        return lmp_failed(state, error);
    error = lmp_power_domain_on(state, LMP_SPM_MFG_CORE1_PWR_CON,
                                LMP_MFG_CORE1_STA_MASK, 2, true);
    if (error)
        return lmp_failed(state, error);
    error = lmp_status(state, &state->status);
    if (error)
        return error;
    if (!state->status.domain_on)
        return lmp_failed(state, LMP_EIO);
    return LMP_OK;
}

int
lmp_clock_on(struct lmp_state *state)
{
    uint32_t value;
    int error;

    error = lmp_require_full(state);
    if (error)
        return error;
    if (!state->prepared)
        return lmp_failed(state, LMP_EINVAL);
    if (!state->status.domain_on)
        return lmp_failed(state, LMP_ENODEV);
    error = lmp_upstream_clock_ready(state);
    if (error)
        return error;
    error = lmp_check_pwr_con(state);
    if (error)
        return lmp_failed(state, error);
    error = lmp_check_rails(state, true);
    if (error)
        return lmp_failed(state, error);
    error = lmp_read(state, LMP_BANK_MFGCFG, LMP_MFG_CG_CON, &value);
    if (error)
        return lmp_failed(state, error);
    if ((value & 1u) == 0) {
        state->initial_clock_on = true;
        return LMP_OK;
    }
    error = lmp_write(state, LMP_BANK_MFGCFG, LMP_MFG_CG_CLR, 1u);
    if (error)
        return lmp_failed(state, error);
    error = lmp_wait_mask(state, LMP_BANK_MFGCFG, LMP_MFG_CG_CON, 1u, 0);
    if (error)
        return lmp_failed(state, error);
    state->clock_owned = true;
    return LMP_OK;
}

int
lmp_reset_release(struct lmp_state *state)
{
    uint32_t value;
    int error;

    error = lmp_require_full(state);
    if (error)
        return error;
    if (!state->prepared || !state->status.domain_on)
        return lmp_failed(state, LMP_ENODEV);
    error = lmp_read_pwr_con(state, LMP_SPM_MFG_PWR_CON, &value);
    if (error)
        return lmp_failed(state, error);
    if (value & LMP_PWR_RST_B) {
        state->initial_reset_released = true;
        return LMP_OK;
    }
    /* The boot chain can leave MFG powered with reset asserted (seen after a
     * warm reboot). In full mode the board contract and the WACS2 handoff have
     * already proved that the AP owns the GPU and the boot firmware is done
     * with it, so the domain is taken over with the same documented reset
     * release used for a domain this driver powered itself. The GPU core is
     * soft-reset again before the first job, so no boot-time state survives.
     * When the domain was not inherited powered-on the normal path below has
     * always released reset the same way. The caller reports the inherited
     * domain bits; this file stays free of printf so the loader build can
     * include it. */
    error = lmp_update_pwr_con(state, LMP_SPM_MFG_PWR_CON,
                               LMP_PWR_RST_B, 0);
    if (error)
        return lmp_failed(state, error);
    error = lmp_wait_mask(state, LMP_BANK_SPM, LMP_SPM_MFG_PWR_CON,
                          LMP_PWR_RST_B, LMP_PWR_RST_B);
    if (error)
        return lmp_failed(state, error);
    state->reset_owned = true;
    return LMP_OK;
}

int
lmp_bus_unprotect(struct lmp_state *state)
{
    uint32_t protect, ack;
    bool protected;
    int error;

    error = lmp_require_full(state);
    if (error)
        return error;
    if (!state->prepared || !state->status.domain_on)
        return lmp_failed(state, LMP_ENODEV);
    error = lmp_read_bus_state(state, &protect, &ack, &protected);
    if (error)
        return lmp_failed(state, error);
    if (!protected)
        return LMP_OK;
    /* A protected inherited bus must be restored during teardown. */
    state->bus_owned = state->initial_bus_protected;
    error = lmp_set_bus_unprotected(state);
    if (error)
        return lmp_failed(state, error);
    state->status.gpu_accessible = true;
    return LMP_OK;
}

int
lmp_start(struct lmp_state *state)
{
    int error;

    if (state)
        state->last_stage = 1;
    error = lmp_require_full(state);
    if (error)
        return error;
    if (!state->prepared) {
        state->last_stage = 2;
        error = lmp_prepare(state);
        if (error)
            return error;
    }
    if (!state->status.domain_on) {
        state->last_stage = 3;
        error = lmp_power_on(state);
        if (error)
            return error;
    }
    state->last_stage = 4;
    error = lmp_clock_on(state);
    if (error)
        return error;
    state->last_stage = 5;
    error = lmp_reset_release(state);
    if (error)
        return error;
    state->last_stage = 6;
    error = lmp_bus_unprotect(state);
    if (error)
        return error;
    state->last_stage = 0;
    state->started = true;
    state->gpu_ready = true;
    state->teardown_safe = true;
    return LMP_OK;
}

int
lmp_gpu_read32(struct lmp_state *state, uint32_t offset, uint32_t *value)
{
    if (lmp_require_full(state) != LMP_OK || !state->gpu_ready)
        return LMP_ENODEV;
    return lmp_read(state, LMP_BANK_GPU, offset, value);
}

int
lmp_gpu_write32(struct lmp_state *state, uint32_t offset, uint32_t value)
{
    if (lmp_require_full(state) != LMP_OK || !state->gpu_ready)
        return LMP_ENODEV;
    return lmp_write(state, LMP_BANK_GPU, offset, value);
}

int
lmp_teardown(struct lmp_state *state)
{
    int error;

    if (!state || !state->initialized)
        return LMP_EINVAL;
    if (state->config.mode == LMP_MODE_OFF ||
        state->config.mode == LMP_MODE_SPM_ONLY)
        return LMP_OK;
    if (state->config.keep_resources)
        return LMP_OK;
    /* Never guess how to undo a failed partial sequence. */
    if (state->failed || !state->started || !state->teardown_safe)
        return LMP_EBUSY;

    state->gpu_ready = false;
    /* Core SRAM shutdown needs the inherited live MFG source clock. */
    error = lmp_power_domain_off(state, LMP_SPM_MFG_CORE1_PWR_CON,
                                 LMP_MFG_CORE1_STA_MASK, 2, true);
    if (error)
        return error;
    error = lmp_power_domain_off(state, LMP_SPM_MFG_CORE0_PWR_CON,
                                 LMP_MFG_CORE0_STA_MASK, 1, true);
    if (error)
        return error;
    if (state->domain_owned[0] || (state->bus_owned && state->initial_bus_protected)) {
        error = lmp_set_bus_protected(state);
        if (error)
            return error;
    }
    if (state->domain_owned[0]) {
        error = lmp_update_pwr_con(state, LMP_SPM_MFG_PWR_CON, LMP_SRAM_PDN, 0);
        if (error)
            return error;
        error = lmp_wait_mask(state, LMP_BANK_SPM, LMP_SPM_MFG_PWR_CON,
                              LMP_SRAM_PDN_ACK, LMP_SRAM_PDN_ACK);
        if (error)
            return error;
    }
    if (state->clock_owned) {
        error = lmp_write(state, LMP_BANK_MFGCFG, LMP_MFG_CG_SET, 1u);
        if (error)
            return error;
        error = lmp_wait_mask(state, LMP_BANK_MFGCFG, LMP_MFG_CG_CON, 1u, 1u);
        if (error)
            return error;
        state->clock_owned = false;
    }
    error = lmp_power_domain_off(state, LMP_SPM_MFG_PWR_CON,
                                 LMP_MFG_PWR_STA_MASK, 0, false);
    if (error)
        return error;
    error = lmp_power_domain_off(state, LMP_SPM_MFG_ASYNC_PWR_CON,
                                 LMP_MFG_ASYNC_STA_MASK, 3, false);
    if (error)
        return error;

    if (state->rail_owned[LMP_RAIL_VSRAM_GPU]) {
        error = lmp_disable_rail(state, LMP_RAIL_VSRAM_GPU);
        if (error)
            return error;
        state->rail_owned[LMP_RAIL_VSRAM_GPU] = false;
    }
    if (state->rail_owned[LMP_RAIL_VGPU]) {
        error = lmp_disable_rail(state, LMP_RAIL_VGPU);
        if (error)
            return error;
        state->rail_owned[LMP_RAIL_VGPU] = false;
    }
    state->bus_owned = false;
    state->reset_owned = false;
    state->started = false;
    state->teardown_safe = false;
    return LMP_OK;
}
