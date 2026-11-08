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

/* Direct AP GPU DVFS is an explicit, separately guarded backend. Never infer
 * GPU control from the CPU SSPM protocol or an idle PWRAP snapshot. */
static int
lmp_dvfs_fail(struct lmp_state *s, int error)
{
    s->failed = true;
    s->gpu_ready = false;
    s->status.gpu_accessible = false;
    s->teardown_safe = false;
    s->dvfs.attached = false;
    s->dvfs.transitioning = false;
    s->dvfs.meter_valid = false;
    s->dvfs.limit_policy_valid = false;
    s->dvfs.operation_active = false;
    if (!s->dvfs.error) s->dvfs.error = error;
    if (s->dvfs.ops.revoke) s->dvfs.ops.revoke(s->dvfs.ops.cookie, error);
    /* Leave the last known/unknown clock and voltage alone. A failed MMIO
     * write may have completed; rollback/reset at a guessed clock is unsafe. */
    return error;
}

static int
lmp_dvfs_guard(struct lmp_state *s)
{
    if (!s->dvfs.ops.guard) return LMP_ENOTSUP;
    if (s->dvfs.operation_active) {
        uint64_t now = s->ops.now_ns(s->ops.cookie);
        if (now < s->dvfs.operation_start_ns ||
            now - s->dvfs.operation_start_ns >= s->config.timeout_ns) return LMP_ETIMEDOUT;
    }
    int error = s->dvfs.ops.guard(s->dvfs.ops.cookie);
    if (!error && s->dvfs.operation_active) {
        uint64_t now = s->ops.now_ns(s->ops.cookie);
        if (now < s->dvfs.operation_start_ns ||
            now - s->dvfs.operation_start_ns >= s->config.timeout_ns) return LMP_ETIMEDOUT;
    }
    return error;
}
static void lmp_dvfs_begin(struct lmp_state *s) {
    s->dvfs.operation_start_ns = s->ops.now_ns(s->ops.cookie);
    s->dvfs.operation_active = true;
}
static int lmp_dvfs_finish(struct lmp_state *s,int error) {
    s->dvfs.operation_active = false;return error;
}

/* Meter setup and restore mutate a shared resource too. Recheck the mandatory
 * guard at each write boundary; a completed read/write may have revealed lost
 * rights since entry. On refusal do not restore a now-foreign counter. This
 * does not create an ownership token or replace a real producer-side lease. */
static int
lmp_dvfs_meter_write(struct lmp_state *s, uint32_t offset, uint32_t value)
{
    int error = lmp_dvfs_guard(s);
    if (error) return error;
    return lmp_write(s, LMP_BANK_TOP, offset, value);
}

static bool
lmp_dvfs_clock_equal(const struct lmp_dvfs_clock *a,
                     const struct lmp_dvfs_clock *b)
{
    if (a->top != b->top) return false;
    for (unsigned i = 0; i < 3; i++) {
        /* CON1's PCW-change latch may self-clear; DDS/postdiv must not. */
        uint32_t mask = i == 1 ? ~LGD_PLL_CHANGE : UINT32_MAX;
        if ((a->mfg[i] & mask) != (b->mfg[i] & mask) || a->main[i] != b->main[i]) return false;
    }
    return a->hz == b->hz && a->park_hz == b->park_hz;
}

static int
lmp_dvfs_read_clock(struct lmp_state *s, struct lmp_dvfs_clock *out)
{
    struct lmp_dvfs_clock sample[2] = {{0}};
    const uint32_t delta[3] = {0, 4, 12};
    uint32_t parent;
    int error;
    for (unsigned pass = 0; pass < 2; pass++) {
        struct lmp_dvfs_clock *c = &sample[pass];
        if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_CFG_1, &c->top))) return error;
        for (unsigned i = 0; i < 3; i++) {
            if ((error = lmp_read(s, LMP_BANK_APMIXED, LMP_APMIXED_MFGPLL_CON0 + delta[i], &c->mfg[i]))) return error;
            if ((error = lmp_read(s, LMP_BANK_APMIXED, LMP_APMIXED_MAINPLL_CON0 + delta[i], &c->main[i]))) return error;
        }
        parent = c->top & LMP_TOP_MFG_SEL_MASK;
        if ((parent != 1u && parent != 2u) || (c->top & LMP_TOP_MFG_GATE) ||
            !(c->mfg[0] & LMP_PLL_EN) || !(c->main[0] & LMP_PLL_EN) ||
            !(c->mfg[2] & LMP_PLL_PWR_ON) || !(c->main[2] & LMP_PLL_PWR_ON) ||
            (c->mfg[2] & LMP_PLL_ISO_EN) || (c->main[2] & LMP_PLL_ISO_EN) ||
            (c->mfg[1] & ~(LGD_PLL_FIELDS | LGD_PLL_CHANGE)) ||
            !lgd_pll_hz(c->main[1], 3u, &c->park_hz) ||
            !lgd_pll_hz(parent == 1u ? c->mfg[1] : c->main[1], parent == 1u ? 1u : 3u, &c->hz)) return LMP_ENOTSUP;
    }
    if (!lmp_dvfs_clock_equal(&sample[0], &sample[1])) return LMP_EIO;
    *out = sample[0]; return LMP_OK;
}

static int
lmp_dvfs_read_rails(struct lmp_state *s, struct lmp_gpu_rails *out, uint32_t hz)
{
    int error = s->ops.pmic.read_rails(s->ops.pmic.cookie, out);
    if (error) return error;
    if (!out->enabled[0] || !out->enabled[1] ||
        !lgd_rail_pair_valid(out->uv[0], out->uv[1]) ||
        out->uv[0] < lmp_required_vgpu(hz)) return LMP_EIO;
    return LMP_OK;
}

/* CKGEN ID5 = hf_fmfg_ck. Pinned clk-mt6768.c mt_get_ckgen_freq(),
 * not the CPU PLL meter and not a nominal-frequency echo. Only TOP debug,
 * divider-for-meter and counter control are touched, never clock muxes/gates.
 * Guard follows the explicit control policy; native AP does not advertise a
 * foreign-peer lease. Fresh busy/setup/result/restore checks remain mandatory.
 * Frequency resolution is 26MHz/1024 (~25.4kHz); tolerate four counts. */
static int
lmp_dvfs_measure(struct lmp_state *s, uint32_t expected_hz)
{
    uint32_t dbg, misc, control, count, restored;
    uint64_t measured, difference;
    int error;
    s->dvfs.meter_valid = false;
    if ((error = lmp_dvfs_guard(s))) return error;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK26CALI_0, &control))) return error;
    if (control != 0) return LMP_EBUSY;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_DBG_CFG, &dbg))) return error;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_MISC_CFG_0, &misc))) return error;
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK_DBG_CFG, (dbg & 0xffffc0fcu) | (5u << 8) | 1u))) return error;
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK_MISC_CFG_0, misc & 0x00ffffffu))) return error;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_DBG_CFG, &restored))) return error;
    if (restored != ((dbg & 0xffffc0fcu) | (5u << 8) | 1u)) return LMP_EIO;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_MISC_CFG_0, &restored))) return error;
    if (restored != (misc & 0x00ffffffu)) return LMP_EIO;
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK26CALI_0, 0x1000u))) return error;
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK26CALI_0, 0x1010u))) return error;
    /* Demand a fresh busy acknowledgement, not an old completed count after
     * a silently dropped START. Preemption can miss this short window: that
     * is an explicit conservative failure, never a fabricated measurement. */
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK26CALI_0, &control))) return error;
    if ((control & 0x1010u) != 0x1010u) return LMP_EIO;
    if ((error = lmp_wait_mask(s, LMP_BANK_TOP, LMP_TOP_CLK26CALI_0, 0x10u, 0))) return error;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK26CALI_1, &count))) return error;
    /* Restore only after known counter completion. On any transport error or
     * unknown completion, stop instead of guessing/issuing further commands. */
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK_DBG_CFG, dbg))) return error;
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK_MISC_CFG_0, misc))) return error;
    if ((error = lmp_dvfs_meter_write(s, LMP_TOP_CLK26CALI_0, 0))) return error;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_DBG_CFG, &restored))) return error;
    if (restored != dbg) return LMP_EIO;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK_MISC_CFG_0, &restored))) return error;
    if (restored != misc) return LMP_EIO;
    if ((error = lmp_read(s, LMP_BANK_TOP, LMP_TOP_CLK26CALI_0, &restored))) return error;
    if (restored != 0) return LMP_EIO;
    measured = (uint64_t)(count & 0xffffu) * 26000000u / 1024u;
    difference = measured > expected_hz ? measured - expected_hz : expected_hz - measured;
    if (!measured || difference > 102000u) return LMP_EIO;
    if ((error = lmp_dvfs_guard(s))) return error;
    s->dvfs.measured_hz = (uint32_t)measured;
    s->dvfs.meter_count = count & 0xffffu;
    s->dvfs.meter_samples++;
    s->dvfs.measured_ns = s->ops.now_ns(s->ops.cookie);
    s->dvfs.meter_valid = true;
    return LMP_OK;
}

static int
lmp_dvfs_checkpoint(struct lmp_state *s)
{
    struct lmp_dvfs_clock c;
    struct lmp_gpu_rails rails;
    struct lmp_status status;
    uint32_t cg, protect, ack;
    bool protected;
    int error;
    if ((error = lmp_dvfs_guard(s))) return error;
    if ((error = lmp_status(s, &status))) return error;
    if (!status.domain_on) return LMP_EIO;
    if ((error = lmp_check_pwr_con(s))) return error;
    if ((error = lmp_read_bus_state(s, &protect, &ack, &protected))) return error;
    if ((protect | ack) & LMP_MFG_PROT_MASK) return LMP_EIO;
    if ((error = lmp_read(s, LMP_BANK_MFGCFG, LMP_MFG_CG_CON, &cg))) return error;
    if (cg & 1u) return LMP_EIO;
    if ((error = lmp_dvfs_read_clock(s, &c))) return error;
    if (!lmp_dvfs_clock_equal(&s->dvfs.clock, &c)) return LMP_EIO;
    if ((error = lmp_dvfs_read_rails(s, &rails, c.hz))) return error;
    for (unsigned i = 0; i < LMP_RAIL_COUNT; i++)
        if (rails.uv[i] != s->dvfs.rails.uv[i] || rails.enabled[i] != s->dvfs.rails.enabled[i]) return LMP_EIO;
    return LMP_OK;
}

/* Optional startup absence is normal: <=850MHz still follows load in BOTH
 * directions. Existing valid lower hardware caps always win. Runtime failure
 * after a provider starts is NOT absence: preserve protection, never replace
 * a failed real sensor/limit with invented data or the850MHz startup ceiling.
 * Mandatory PMIC/clock/ownership/quiescence guards are entirely separate. */
static int
lmp_dvfs_live_cap(struct lmp_state *s, uint32_t *cap)
{
    struct lmp_dvfs_limits limits = {0};
    uint32_t invalid, result;
    uint64_t now;
    int error = s->dvfs.ops.limits ? s->dvfs.ops.limits(s->dvfs.ops.cookie, &limits) : LMP_ENODEV;
    now = s->ops.now_ns(s->ops.cookie);
    s->dvfs.limit_policy_valid = false;
    if ((limits.started_mask | limits.valid_mask | limits.fault_mask) & ~LMP_DVFS_LIMIT_ALL) {
        error = LMP_EINVAL; goto failed;
    }
    s->dvfs.provider_started_mask |= limits.started_mask | limits.valid_mask;
    s->dvfs.provider_valid_mask = limits.valid_mask;
    s->dvfs.provider_missing_mask = LMP_DVFS_LIMIT_ALL & ~s->dvfs.provider_started_mask;
    invalid = limits.fault_mask | (s->dvfs.provider_started_mask & ~limits.valid_mask);
    s->dvfs.provider_fault_mask = invalid;
    /* Aggregate ENODEV/ENOTSUP is startup absence only if every started
     * component remains valid and at least one component never started. */
    if (error && (error != LMP_ENODEV && error != LMP_ENOTSUP)) goto failed;
    if (invalid || (error && !s->dvfs.provider_missing_mask)) {
        if (!error) error = LMP_EIO;
        goto failed;
    }
    if (limits.valid_mask) {
        if (limits.sampled_ns > now) {
            error = LMP_EIO; goto stale;
        }
        if (limits.valid_until_ns <= now || now - limits.sampled_ns > UINT64_C(100000000)) {
            error = LMP_ETIMEDOUT; goto stale;
        }
    }
    result = limits.valid_mask == LMP_DVFS_LIMIT_ALL ? LGD_MAX_KHZ * 1000u : LMP_DVFS_OPTIONAL_MAX_HZ;
    if ((limits.valid_mask & LMP_DVFS_LIMIT_THERMAL) && result > limits.thermal_max_hz) result = limits.thermal_max_hz;
    if ((limits.valid_mask & LMP_DVFS_LIMIT_BATTERY) && result > limits.battery_max_hz) result = limits.battery_max_hz;
    if ((limits.valid_mask & LMP_DVFS_LIMIT_POWER) && result > limits.power_max_hz) result = limits.power_max_hz;
    *cap = result;
    s->dvfs.effective_max_hz = result;
    s->dvfs.provider_error = LMP_OK;
    s->dvfs.limit_policy_valid = true;
    return LMP_OK;
stale:
    s->dvfs.provider_fault_mask |= limits.valid_mask;
failed:
    s->dvfs.provider_error = error;
    return error;
}

static int
lmp_dvfs_permission(struct lmp_state *s, uint32_t demand_hz, uint32_t peak_hz,
                    bool needs_boost, uint32_t starting_hz)
{
    uint32_t cap;
    int error = lmp_dvfs_live_cap(s, &cap);
    if (error) return error;
    /* A lowering transition may transiently park above a new cap only when
     * still <= the inherited/current rate and no voltage increase is needed. */
    if (demand_hz > cap || (peak_hz > cap && (needs_boost || peak_hz > starting_hz))) return LMP_ENOTSUP;
    return LMP_OK;
}

int
lmp_dvfs_attach(struct lmp_state *s, const struct lmp_dvfs_ops *ops)
{
    struct lmp_dvfs_clock c;
    struct lmp_gpu_rails rails;
    uint32_t index, cap;
    int error = lmp_require_full(s);
    if (error) return error;
    if (!s->started || !s->gpu_ready) return LMP_ENODEV;
    if (s->dvfs.attached || s->dvfs.transitioning) return LMP_EBUSY;
    if (!ops || !ops->guard || !ops->revoke || ops->control_policy > LMP_DVFS_NATIVE_AP ||
        !s->ops.pmic.read_rails || !s->ops.pmic.set_voltage_uv) return LMP_ENOTSUP;
    s->dvfs.ops = *ops;
    lmp_dvfs_begin(s);
    if ((error = lmp_dvfs_guard(s))) return error == LMP_EBUSY ? lmp_dvfs_finish(s,error) : lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_read_clock(s, &c))) return lmp_dvfs_fail(s, error);
    if ((c.top & LMP_TOP_MFG_SEL_MASK) != 1u || !lgd_index_from_hz(c.hz, &index)) return lmp_dvfs_finish(s,LMP_ENOTSUP);
    if ((error = lmp_dvfs_read_rails(s, &rails, c.hz))) return lmp_dvfs_fail(s, error);
    s->dvfs.clock = c; s->dvfs.rails = rails; s->dvfs.current_opp = index;
    s->dvfs.stage = 1;
    if ((error = lmp_dvfs_checkpoint(s))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_measure(s, c.hz))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_live_cap(s, &cap))) return lmp_dvfs_fail(s, error);
    if (cap < 299000000u) return lmp_dvfs_fail(s, LMP_ENOTSUP);
    s->source_hz = c.hz;
    s->dvfs.stage = 0; s->dvfs.attached = true;
    return lmp_dvfs_finish(s,LMP_OK);
}

/* Each individual rail move is at most one selector (6.25mV), and both
 * endpoints satisfy the100..250mV delta. The entire analog slew between them
 * does too. This is a bounded springboard construction valid even for an
 * inherited overvolted pair, not just canonical OPP pairs. Up: SRAM then GPU;
 * down: GPU then SRAM, except at delta boundaries where the other rail must
 * move first. No blind restore if a provider write/DA verification fails. */
static int
lmp_dvfs_move_rails(struct lmp_state *s, const struct lmp_gpu_rails *goal,
                   uint32_t demand_hz, uint32_t peak_hz, bool boost, uint32_t start_hz)
{
    for (unsigned step = 0; step < 256u; step++) {
        uint32_t gpu = s->dvfs.rails.uv[0], sram = s->dvfs.rails.uv[1], next[2] = {gpu, sram};
        enum lmp_rail rail;
        int error;
        if (gpu == goal->uv[0] && sram == goal->uv[1]) return LMP_OK;
        if (sram < goal->uv[1] && lgd_rail_pair_valid(gpu, sram + LGD_RAIL_STEP_UV)) rail = LMP_RAIL_VSRAM_GPU;
        else if (gpu < goal->uv[0] && lgd_rail_pair_valid(gpu + LGD_RAIL_STEP_UV, sram)) rail = LMP_RAIL_VGPU;
        else if (gpu > goal->uv[0] && lgd_rail_pair_valid(gpu - LGD_RAIL_STEP_UV, sram)) rail = LMP_RAIL_VGPU;
        else if (sram > goal->uv[1] && lgd_rail_pair_valid(gpu, sram - LGD_RAIL_STEP_UV)) rail = LMP_RAIL_VSRAM_GPU;
        else return LMP_EIO;
        next[rail] = next[rail] < goal->uv[rail] ? next[rail] + LGD_RAIL_STEP_UV : next[rail] - LGD_RAIL_STEP_UV;
        if (next[0] < lmp_required_vgpu(s->dvfs.clock.hz)) return LMP_EIO;
        if ((error = lmp_dvfs_checkpoint(s))) return error;
        if ((error = lmp_dvfs_permission(s, demand_hz, peak_hz, boost, start_hz))) return error;
        if ((error = lmp_dvfs_guard(s))) return error;
        s->dvfs.changed = true; /* failed callback may still change the rail */
        if ((error = s->ops.pmic.set_voltage_uv(s->ops.pmic.cookie, rail, next[rail]))) return error;
        s->dvfs.rails.uv[rail] = next[rail];
        if ((error = lmp_dvfs_checkpoint(s))) return error;
    }
    return LMP_ETIMEDOUT;
}

static int
lmp_dvfs_mux(struct lmp_state *s, uint32_t parent, uint32_t demand_hz,
             uint32_t peak_hz, bool boost, uint32_t start_hz)
{
    const uint32_t offsets[3] = {LMP_TOP_CLK_CFG_1_CLR, LMP_TOP_CLK_CFG_1_SET, LMP_TOP_CLK_CFG_UPDATE};
    const uint32_t values[3] = {LMP_TOP_MFG_SEL_MASK, parent, LMP_TOP_MFG_UPDATE};
    int error;
    if ((error = lmp_dvfs_checkpoint(s))) return error;
    for (unsigned i = 0; i < 3; i++) {
        if ((error = lmp_dvfs_guard(s))) return error;
        if ((error = lmp_dvfs_permission(s, demand_hz, peak_hz, boost, start_hz))) return error;
        if ((error = lmp_dvfs_guard(s))) return error;
        s->dvfs.changed = true;
        if ((error = lmp_write(s, LMP_BANK_TOP, offsets[i], values[i]))) return error;
    }
    if ((error = lmp_wait_mask(s, LMP_BANK_TOP, LMP_TOP_CLK_CFG_UPDATE, LMP_TOP_MFG_UPDATE, 0))) return error;
    s->dvfs.clock.top = (s->dvfs.clock.top & ~LMP_TOP_MFG_SEL_MASK) | parent;
    if (!lgd_pll_hz(parent == 1u ? s->dvfs.clock.mfg[1] : s->dvfs.clock.main[1], parent == 1u ? 1u : 3u, &s->dvfs.clock.hz)) return LMP_EIO;
    if ((error = lmp_dvfs_checkpoint(s))) return error;
    return lmp_dvfs_measure(s, s->dvfs.clock.hz);
}

int
lmp_dvfs_target(struct lmp_state *s, uint32_t index)
{
    struct lgd_opp target, parking;
    struct lmp_gpu_rails peak, goal;
    uint32_t word, start_hz, peak_hz, difference, cap;
    bool boost;
    int error = lmp_require_full(s);
    if (error) return error;
    if (!s->dvfs.attached || !s->started || !s->gpu_ready) return LMP_ENOTSUP;
    if (s->dvfs.transitioning) return LMP_EBUSY;
    if (!lgd_get_opp(index, &target) || !lgd_pll_word(target.khz, &word)) return LMP_EINVAL;
    lmp_dvfs_begin(s);
    if ((error = lmp_dvfs_guard(s))) return error == LMP_EBUSY ? lmp_dvfs_finish(s,error) : lmp_dvfs_fail(s, error);
    s->dvfs.stage = 1; s->dvfs.target_opp = index;
    if ((error = lmp_dvfs_checkpoint(s))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_measure(s, s->dvfs.clock.hz))) return lmp_dvfs_fail(s, error);
    /* Even a same-OPP request must enforce a newly lower live cap. Demand is
     * clamped to a real OPP; a provider shutdown cap below the minimum revokes
     * service instead of allowing another job at an unsupported frequency. */
    if ((error = lmp_dvfs_live_cap(s, &cap))) return lmp_dvfs_fail(s, error);
    while (target.khz * 1000u > cap && index + 1u < LGD_OPP_COUNT) {
        index++;
        (void)lgd_get_opp(index, &target);
    }
    if (target.khz * 1000u > cap) return lmp_dvfs_fail(s, LMP_ENOTSUP);
    (void)lgd_pll_word(target.khz, &word);
    s->dvfs.target_opp = index;
    if (index == s->dvfs.current_opp) { s->dvfs.stage = 0; return lmp_dvfs_finish(s,LMP_OK); }
    if (!lgd_floor_opp(s->dvfs.clock.park_hz, &parking)) return lmp_dvfs_finish(s,LMP_ENOTSUP);
    /* A meter cannot distinguish coincident parent rates. Refuse rather than
     * call a shadow mux readback proof of safe parking. */
    difference = s->dvfs.clock.hz > s->dvfs.clock.park_hz ? s->dvfs.clock.hz - s->dvfs.clock.park_hz : s->dvfs.clock.park_hz - s->dvfs.clock.hz;
    if (difference <= 204000u) return lmp_dvfs_finish(s,LMP_ENOTSUP);
    difference = target.khz * 1000u > s->dvfs.clock.park_hz ? target.khz * 1000u - s->dvfs.clock.park_hz : s->dvfs.clock.park_hz - target.khz * 1000u;
    if (difference <= 204000u) return lmp_dvfs_finish(s,LMP_ENOTSUP);
    start_hz = s->dvfs.clock.hz;
    peak_hz = target.khz * 1000u;
    if (peak_hz < s->dvfs.clock.park_hz) peak_hz = s->dvfs.clock.park_hz;
    peak = s->dvfs.rails; goal = s->dvfs.rails;
    goal.uv[0] = target.vgpu_uv; goal.uv[1] = target.vsram_uv;
    if (peak.uv[0] < target.vgpu_uv) peak.uv[0] = target.vgpu_uv;
    if (peak.uv[0] < parking.vgpu_uv) peak.uv[0] = parking.vgpu_uv;
    if (peak.uv[1] < target.vsram_uv) peak.uv[1] = target.vsram_uv;
    if (peak.uv[1] < parking.vsram_uv) peak.uv[1] = parking.vsram_uv;
    if (!lgd_rail_pair_valid(peak.uv[0], peak.uv[1])) return lmp_dvfs_finish(s,LMP_ENOTSUP);
    boost = peak_hz > start_hz || peak.uv[0] > s->dvfs.rails.uv[0] || peak.uv[1] > s->dvfs.rails.uv[1];
    if ((error = lmp_dvfs_permission(s, target.khz * 1000u, peak_hz, boost, start_hz)))
        return s->dvfs.limit_policy_valid ? lmp_dvfs_finish(s,error) : lmp_dvfs_fail(s, error);
    s->dvfs.transitioning = true; s->dvfs.changed = false;
    s->dvfs.stage = 2;
    if ((error = lmp_dvfs_move_rails(s, &peak, target.khz * 1000u, peak_hz, boost, start_hz))) return lmp_dvfs_fail(s, error);
    s->dvfs.stage = 3;
    if ((error = lmp_dvfs_mux(s, 2u, target.khz * 1000u, peak_hz, boost, start_hz))) return lmp_dvfs_fail(s, error);
    s->dvfs.stage = 4;
    if ((error = lmp_dvfs_checkpoint(s))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_permission(s, target.khz * 1000u, peak_hz, boost, start_hz))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_guard(s))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_write(s, LMP_BANK_APMIXED, LMP_APMIXED_MFGPLL_CON1, word))) return lmp_dvfs_fail(s, error);
    s->dvfs.clock.mfg[1] = word;
    {
        uint64_t before = s->ops.now_ns(s->ops.cookie), after;
        s->ops.delay_us(s->ops.cookie, 20u);
        after = s->ops.now_ns(s->ops.cookie);
        if (after < before || after - before < 20000u) return lmp_dvfs_fail(s, LMP_ETIMEDOUT);
    }
    if ((error = lmp_dvfs_checkpoint(s))) return lmp_dvfs_fail(s, error);
    /* Prove output still on the parking source after the PLL write. */
    if ((error = lmp_dvfs_measure(s, s->dvfs.clock.park_hz))) return lmp_dvfs_fail(s, error);
    s->dvfs.stage = 5;
    if ((error = lmp_dvfs_mux(s, 1u, target.khz * 1000u, peak_hz, boost, start_hz))) return lmp_dvfs_fail(s, error);
    s->dvfs.stage = 6;
    if ((error = lmp_dvfs_move_rails(s, &goal, target.khz * 1000u, peak_hz, boost, start_hz))) return lmp_dvfs_fail(s, error);
    s->dvfs.stage = 7;
    if ((error = lmp_dvfs_checkpoint(s))) return lmp_dvfs_fail(s, error);
    if ((error = lmp_dvfs_measure(s, s->dvfs.clock.hz))) return lmp_dvfs_fail(s, error);
    s->source_hz = s->dvfs.clock.hz;
    for (unsigned i = 0; i < LMP_RAIL_COUNT; i++) s->observed_uv[i] = s->dvfs.rails.uv[i];
    s->dvfs.current_opp = index; s->dvfs.transitions++;
    s->dvfs.transitioning = false; s->dvfs.stage = 0;
    return lmp_dvfs_finish(s,LMP_OK);
}

int lmp_dvfs_admit(struct lmp_state *s,uint32_t index,uint64_t max_age_ns,bool *reused) {
    if(reused)*reused=false;
    int error=lmp_require_full(s);if(error)return error;
    if(!max_age_ns || max_age_ns>UINT64_C(10000000) ||
       s->dvfs.ops.control_policy!=LMP_DVFS_NATIVE_AP)return lmp_dvfs_target(s,index);
    if(!s->dvfs.attached || !s->started || !s->gpu_ready)return LMP_ENOTSUP;
    if(s->dvfs.transitioning || s->dvfs.operation_active)return LMP_EBUSY;
    struct lgd_opp target;
    if(!lgd_get_opp(index,&target))return LMP_EINVAL;
    uint64_t now=s->ops.now_ns(s->ops.cookie);
    if(s->dvfs.meter_valid && now<s->dvfs.measured_ns)return lmp_dvfs_fail(s,LMP_EIO);
    if(!s->dvfs.meter_valid || !s->dvfs.measured_ns || now-s->dvfs.measured_ns>=max_age_ns)
        return lmp_dvfs_target(s,index);
    lmp_dvfs_begin(s);
    /* These fresh checks include all mandatory resource guards and both DA
     * rail values. A fresh live cap can force a lower OPP immediately. */
    uint32_t cap;
    if((error=lmp_dvfs_checkpoint(s)) || (error=lmp_dvfs_live_cap(s,&cap)))
        return lmp_dvfs_fail(s,error);
    uint32_t selected=index;
    while(target.khz*1000u>cap && selected+1u<LGD_OPP_COUNT) {
        ++selected;(void)lgd_get_opp(selected,&target);
    }
    if(target.khz*1000u>cap)return lmp_dvfs_fail(s,LMP_ENOTSUP);
    now=s->ops.now_ns(s->ops.cookie);
    if(now<s->dvfs.measured_ns)return lmp_dvfs_fail(s,LMP_EIO);
    if(selected==s->dvfs.current_opp && now-s->dvfs.measured_ns<max_age_ns) {
        if((error=lmp_dvfs_guard(s)))return lmp_dvfs_fail(s,error);
        now=s->ops.now_ns(s->ops.cookie);
        if(now<s->dvfs.measured_ns)return lmp_dvfs_fail(s,LMP_EIO);
        if(now-s->dvfs.measured_ns<max_age_ns) {
            s->dvfs.target_opp=selected;s->dvfs.stage=0;
            if(reused)*reused=true;
            return lmp_dvfs_finish(s,LMP_OK); /* original measurement timestamp retained */
        }
    }
    (void)lmp_dvfs_finish(s,LMP_OK);
    return lmp_dvfs_target(s,index);
}

int lmp_dvfs_verify(struct lmp_state *s,struct lmp_dvfs_sample *out) {
    if(!out)return LMP_EINVAL;
    *out=(struct lmp_dvfs_sample){0};
    int error=lmp_require_full(s);if(error)return error;
    if(!s->dvfs.attached||!s->started||!s->gpu_ready)return LMP_ENODEV;
    if(s->dvfs.transitioning||s->dvfs.operation_active)return LMP_EBUSY;
    lmp_dvfs_begin(s);
    if((error=lmp_dvfs_checkpoint(s))||
       (error=lmp_dvfs_measure(s,s->dvfs.clock.hz))||
       (error=lmp_dvfs_checkpoint(s)))return lmp_dvfs_fail(s,error);
    out->clock=s->dvfs.clock;out->rails=s->dvfs.rails;
    out->measured_hz=s->dvfs.measured_hz;out->measured_ns=s->dvfs.measured_ns;
    return lmp_dvfs_finish(s,LMP_OK);
}
