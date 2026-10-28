#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/lancelot_mali_platform.h"
#include "../../xnu/files/bsd/dev/lancelot_mali_pmic.h"

/* Include the production implementation so ASan/UBSan exercise the same code. */
#include "../../xnu/files/bsd/dev/lancelot_mali_platform.c"
#include "../../xnu/files/bsd/dev/lancelot_mali_pmic.c"

struct platform_model {
    uint32_t regs[LMP_BANK_COUNT][4096];
    uint64_t now;
    unsigned reads[LMP_BANK_COUNT], writes;
    bool stuck;
    bool rails[LMP_RAIL_COUNT];
    uint32_t rail_uv[LMP_RAIL_COUNT];
    unsigned pmic_enables[LMP_RAIL_COUNT], pmic_disables[LMP_RAIL_COUNT];
};

static uint64_t
model_now(void *cookie)
{
    struct platform_model *m = cookie;
    return m->now++;
}

static void
model_delay(void *cookie, uint32_t usec)
{
    struct platform_model *m = cookie;
    m->now += usec;
}

static int
model_read(void *cookie, enum lmp_bank bank, uint32_t offset, uint32_t *value)
{
    struct platform_model *m = cookie;
    assert(bank < LMP_BANK_COUNT && (offset & 3u) == 0);
    assert(offset < (bank == LMP_BANK_GPU ? 0x4000u : 0x1000u));
    m->reads[bank]++;
    *value = m->regs[bank][offset / 4];
    return LMP_OK;
}

static void
model_domain_status(struct platform_model *m, uint32_t offset,
                    uint32_t value)
{
    uint32_t mask = 0;
    unsigned bit = 0;
    switch (offset) {
    case LMP_SPM_MFG_PWR_CON: bit = 11; break;
    case LMP_SPM_MFG_CORE0_PWR_CON: bit = 12; break;
    case LMP_SPM_MFG_CORE1_PWR_CON: bit = 13; break;
    case LMP_SPM_MFG_ASYNC_PWR_CON: bit = 14; break;
    default: return;
    }
    mask = 1u << bit;
    if ((value & (LMP_PWR_ON | LMP_PWR_ON_2ND)) ==
        (LMP_PWR_ON | LMP_PWR_ON_2ND)) {
        m->regs[LMP_BANK_SPM][LMP_SPM_PWR_STATUS / 4] |= mask;
        m->regs[LMP_BANK_SPM][LMP_SPM_PWR_STATUS_2ND / 4] |= mask;
    } else if ((value & (LMP_PWR_ON | LMP_PWR_ON_2ND)) == 0) {
        m->regs[LMP_BANK_SPM][LMP_SPM_PWR_STATUS / 4] &= ~mask;
        m->regs[LMP_BANK_SPM][LMP_SPM_PWR_STATUS_2ND / 4] &= ~mask;
    }
}

static int
model_write(void *cookie, enum lmp_bank bank, uint32_t offset, uint32_t value)
{
    struct platform_model *m = cookie;
    uint32_t *reg;
    assert(bank < LMP_BANK_COUNT && (offset & 3u) == 0);
    assert(offset < (bank == LMP_BANK_GPU ? 0x4000u : 0x1000u));
    m->writes++;
    if (m->stuck)
        return LMP_OK;
    reg = &m->regs[bank][offset / 4];
    if (bank == LMP_BANK_INFRA && offset == LMP_INFRA_PROTECTEN_SET) {
        m->regs[LMP_BANK_INFRA][LMP_INFRA_PROTECTEN / 4] |= value;
        m->regs[LMP_BANK_INFRA][LMP_INFRA_PROTECTEN_STA1 / 4] |= value;
        return LMP_OK;
    }
    if (bank == LMP_BANK_INFRA && offset == LMP_INFRA_PROTECTEN_CLR) {
        m->regs[LMP_BANK_INFRA][LMP_INFRA_PROTECTEN / 4] &= ~value;
        m->regs[LMP_BANK_INFRA][LMP_INFRA_PROTECTEN_STA1 / 4] &= ~value;
        return LMP_OK;
    }
    if (bank == LMP_BANK_MFGCFG && offset == LMP_MFG_CG_CLR) {
        m->regs[LMP_BANK_MFGCFG][LMP_MFG_CG_CON / 4] &= ~value;
        return LMP_OK;
    }
    if (bank == LMP_BANK_MFGCFG && offset == LMP_MFG_CG_SET) {
        m->regs[LMP_BANK_MFGCFG][LMP_MFG_CG_CON / 4] |= value;
        return LMP_OK;
    }
    if (bank == LMP_BANK_SPM) {
        model_domain_status(m, offset, value);
        if (offset == LMP_SPM_MFG_PWR_CON ||
            offset == LMP_SPM_MFG_CORE0_PWR_CON ||
            offset == LMP_SPM_MFG_CORE1_PWR_CON) {
            if (value & LMP_SRAM_PDN)
                value |= LMP_SRAM_PDN_ACK;
            else
                value &= ~LMP_SRAM_PDN_ACK;
        }
    }
    *reg = value;
    return LMP_OK;
}

static int
model_rail_state(void *cookie, enum lmp_rail rail, bool *enabled)
{
    struct platform_model *m = cookie;
    assert(rail < LMP_RAIL_COUNT);
    *enabled = m->rails[rail];
    return LMP_OK;
}

static int
model_rail_enable(void *cookie, enum lmp_rail rail)
{
    struct platform_model *m = cookie;
    assert(rail < LMP_RAIL_COUNT);
    m->rails[rail] = true;
    m->pmic_enables[rail]++;
    return LMP_OK;
}

static int
model_rail_disable(void *cookie, enum lmp_rail rail)
{
    struct platform_model *m = cookie;
    assert(rail < LMP_RAIL_COUNT);
    m->rails[rail] = false;
    m->pmic_disables[rail]++;
    return LMP_OK;
}

static int
model_rail_voltage(void *cookie, enum lmp_rail rail, uint32_t *uv)
{
    struct platform_model *m = cookie;
    assert(rail < LMP_RAIL_COUNT && uv != NULL);
    *uv = m->rail_uv[rail];
    return LMP_OK;
}

static int
model_rail_set_voltage(void *cookie, enum lmp_rail rail, uint32_t uv)
{
    struct platform_model *m = cookie;
    assert(rail < LMP_RAIL_COUNT);
    if (rail == LMP_RAIL_VGPU && uv != 950000u) return LMP_EINVAL;
    if (rail == LMP_RAIL_VSRAM_GPU && uv != 1050000u) return LMP_EINVAL;
    m->rail_uv[rail] = uv;
    return LMP_OK;
}

static struct lmp_io_ops
model_ops(struct platform_model *m, bool with_pmic)
{
    struct lmp_io_ops ops = {
        .cookie = m, .read32 = model_read, .write32 = model_write,
        .now_ns = model_now, .delay_us = model_delay,
    };
    if (with_pmic) {
        ops.pmic.cookie = m;
        ops.pmic.is_enabled = model_rail_state;
        ops.pmic.enable = model_rail_enable;
        ops.pmic.disable = model_rail_disable;
        ops.pmic.set_voltage_uv = model_rail_set_voltage;
        ops.pmic.get_voltage_uv = model_rail_voltage;
    }
    return ops;
}

static struct lmp_config
model_config(enum lmp_mode mode)
{
    return (struct lmp_config){
        .platform = LMP_PLATFORM_LANCELOT_MT6768,
        .mode = mode,
        .timeout_ns = 50,
    };
}

static void
model_prepare_off(struct platform_model *m, bool protected)
{
    m->regs[LMP_BANK_TOP][LMP_TOP_CLK_CFG_1 / 4] = 0; /* clk26m */
    m->regs[LMP_BANK_MFGCFG][LMP_MFG_CG_CON / 4] = 1;
    if (protected) {
        m->regs[LMP_BANK_INFRA][LMP_INFRA_PROTECTEN / 4] = LMP_MFG_PROT_MASK;
        m->regs[LMP_BANK_INFRA][LMP_INFRA_PROTECTEN_STA1 / 4] = LMP_MFG_PROT_MASK;
    }
}

static void
test_platform_fail_closed(void)
{
    struct platform_model m = {0};
    struct lmp_state state;
    struct lmp_status status;
    struct lmp_config cfg;
    struct lmp_io_ops ops;
    unsigned writes;

    cfg = model_config(LMP_MODE_SPM_ONLY);
    ops = model_ops(&m, false);
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_status(&state, &status) == LMP_OK);
    assert(m.reads[LMP_BANK_SPM] == 2 && m.writes == 0);
    assert(lmp_start(&state) == LMP_ENOTSUP && m.writes == 0);

    m = (struct platform_model){0};
    cfg.platform = LMP_PLATFORM_UNKNOWN;
    assert(lmp_init(&state, &cfg, &ops) == LMP_ENOTSUP);
    assert(m.writes == 0 && m.reads[LMP_BANK_SPM] == 0);

    m = (struct platform_model){0};
    cfg = model_config(LMP_MODE_FULL);
    ops = model_ops(&m, true);
    m.regs[LMP_BANK_TOP][LMP_TOP_CLK_CFG_1 / 4] = 1; /* MFGPLL */
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_prepare(&state) == LMP_ENODEV);
    assert(m.writes == 0); /* upstream rejection precedes PMIC/SPM writes */

    m = (struct platform_model){0};
    model_prepare_off(&m, true);
    m.stuck = true;
    cfg = model_config(LMP_MODE_FULL);
    ops = model_ops(&m, true);
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    writes = m.writes;
    assert(lmp_start(&state) == LMP_ETIMEDOUT);
    assert(m.writes >= writes); /* bounded timeout, never an endless loop */
    assert(lmp_gpu_read32(&state, LMP_GPU_ID, &status.pwr_status) == LMP_ENODEV);
}

static void
test_platform_lifecycle(void)
{
    struct platform_model m = {0};
    struct lmp_state state;
    struct lmp_config cfg = model_config(LMP_MODE_FULL);
    struct lmp_io_ops ops = model_ops(&m, true);
    uint32_t id;

    model_prepare_off(&m, true);
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_start(&state) == LMP_OK);
    assert(state.gpu_ready && m.pmic_enables[0] == 1 && m.pmic_enables[1] == 1);
    m.regs[LMP_BANK_GPU][LMP_GPU_ID / 4] = 0x72121120u;
    assert(lmp_gpu_read32(&state, LMP_GPU_ID, &id) == LMP_OK);
    assert(id == 0x72121120u);
    assert(lmp_gpu_read32(&state, 0x4000, &id) == LMP_EINVAL);
    assert(lmp_teardown(&state) == LMP_OK);
    assert(!state.gpu_ready && m.pmic_disables[0] == 1 && m.pmic_disables[1] == 1);

    /* Inherited domain/clock/rails are never disabled by this instance. */
    m = (struct platform_model){0};
    m.regs[LMP_BANK_SPM][LMP_SPM_PWR_STATUS / 4] = LMP_MFG_DOMAIN_MASK;
    m.regs[LMP_BANK_SPM][LMP_SPM_PWR_STATUS_2ND / 4] = LMP_MFG_DOMAIN_MASK;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_PWR_CON / 4] = LMP_PWR_RST_B|LMP_PWR_ON|LMP_PWR_ON_2ND;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_CORE0_PWR_CON / 4] = LMP_PWR_RST_B|LMP_PWR_ON|LMP_PWR_ON_2ND;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_CORE1_PWR_CON / 4] = LMP_PWR_RST_B|LMP_PWR_ON|LMP_PWR_ON_2ND;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_ASYNC_PWR_CON / 4] = LMP_PWR_RST_B|LMP_PWR_ON|LMP_PWR_ON_2ND;
    m.regs[LMP_BANK_MFGCFG][LMP_MFG_CG_CON / 4] = 0;
    m.rails[0] = m.rails[1] = true;
    m.rail_uv[0] = 950000u; m.rail_uv[1] = 1050000u;
    model_prepare_off(&m, false);
    m.regs[LMP_BANK_MFGCFG][LMP_MFG_CG_CON / 4] = 0;
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_start(&state) == LMP_OK);
    assert(lmp_teardown(&state) == LMP_OK);
    assert(m.pmic_disables[0] == 0 && m.pmic_disables[1] == 0);

    /* Actual Mu->XNU observation: MFGPLL 0x021493b1 is about 535 MHz,
     * not Android's earlier 950 MHz sample. Inherited DA selectors decode
     * to VGPU=800mV / VSRAM=900mV; never retune or disable their owner. */
    m = (struct platform_model){0};
    model_prepare_off(&m, false);
    m.regs[LMP_BANK_TOP][LMP_TOP_CLK_CFG_1 / 4] = 0x01010201;
    m.regs[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON0 / 4] = 0x1e1;
    m.regs[LMP_BANK_APMIXED][(LMP_APMIXED_MFGPLL_CON0 + 4) / 4] = 0x021493b1;
    m.regs[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON3 / 4] = 0x80000001;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_PWR_CON / 4] = 0x1112;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_CORE0_PWR_CON / 4] = 0x1112;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_CORE1_PWR_CON / 4] = 0x1112;
    m.regs[LMP_BANK_SPM][LMP_SPM_MFG_ASYNC_PWR_CON / 4] = 0x112;
    m.rails[0] = m.rails[1] = true;
    m.rail_uv[0] = 800000; m.rail_uv[1] = 900000;
    ops.pmic.enable = NULL; ops.pmic.disable = NULL; ops.pmic.set_voltage_uv = NULL;
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_start(&state) == LMP_OK && state.source_hz == 534999909);
    assert(m.pmic_enables[0] == 0 && m.pmic_enables[1] == 0);
    assert(lmp_teardown(&state) == LMP_OK);
    assert(m.pmic_disables[0] == 0 && m.pmic_disables[1] == 0);

    /* Read-only providers cannot silently enable a missing rail. */
    m = (struct platform_model){0};model_prepare_off(&m, false);
    m.rails[0] = true; m.rails[1] = false;
    m.rail_uv[0] = 800000; m.rail_uv[1] = 900000;
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_start(&state) == LMP_ENOTSUP && m.writes == 0);

    /* The same inherited rail pair cannot admit a 950MHz source: reject
     * before even the first SPM/PMIC/GPU write, without voltage tolerance. */
    m = (struct platform_model){0};
    model_prepare_off(&m, false);
    m.regs[LMP_BANK_TOP][LMP_TOP_CLK_CFG_1 / 4] = 1;
    m.regs[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON0 / 4] = 0x1e1;
    m.regs[LMP_BANK_APMIXED][(LMP_APMIXED_MFGPLL_CON0 + 4) / 4] =
        (2u << 24) | (uint32_t)(UINT64_C(950000000) * 65536 / 26000000);
    m.regs[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON3 / 4] = 1;
    m.rails[0] = m.rails[1] = true;
    m.rail_uv[0] = 800000; m.rail_uv[1] = 900000;
    assert(lmp_init(&state, &cfg, &ops) == LMP_OK);
    assert(lmp_start(&state) == LMP_ENOTSUP && m.writes == 0);
    assert(m.reads[LMP_BANK_GPU] == 0);
    puts("Observed Mu 535MHz/800mV/900mV fixture admitted without inherited rail changes; incompatible 950MHz rejected before writes; RAM ONLY");
}

struct pwrap_model {
    uint32_t regs[0x1000 / 4];
    uint16_t pmic[0x8000];
    uint64_t now;
    bool stuck;
    unsigned writes;
    bool owned, deny_owner, hang_own_read;
    bool foreign_req, poisoned;
    unsigned poisons;
    unsigned foreign_fsm;
    unsigned acquires, releases, reads;
};
static int pwrap_acquire(void *cookie, uint64_t timeout_ns) {
    struct pwrap_model *m=cookie;assert(timeout_ns>0&&!m->owned);
    m->acquires++;if(m->deny_owner)return LMP_EBUSY;
    if(m->poisoned)return LMP_EIO;
    m->owned=true;return LMP_OK;
}
static void pwrap_release(void *cookie) {
    struct pwrap_model *m=cookie;assert(m->owned);m->owned=false;m->releases++;
}
static void pwrap_poison(void *cookie) {
    struct pwrap_model *m=cookie;assert(m->owned);m->poisoned=true;m->poisons++;
}

static uint64_t
pwrap_now(void *cookie)
{
    return ((struct pwrap_model *)cookie)->now++;
}

static void
pwrap_delay(void *cookie, uint32_t usec)
{
    ((struct pwrap_model *)cookie)->now += usec;
}

static uint32_t
pwrap_status(const struct pwrap_model *m)
{
    if(m->foreign_req)return (1u<<19)|(1u<<20)|(1u<<21); /* IDLE but REQ=1 */
    if (m->stuck || m->foreign_fsm)
        return ((m->foreign_fsm ? m->foreign_fsm : 6u) << 16) | (1u << 20) | (1u << 21);
    return (1u << 20) | (1u << 21);
}

static int
pwrap_read(void *cookie, uint32_t offset, uint32_t *value)
{
    struct pwrap_model *m = cookie;
    assert(m->owned);m->reads++;
    assert(offset < sizeof(m->regs) && !(offset & 3u));
    if (offset == LMP_PMIC_WACS2_RDATA)
        *value = m->stuck ? pwrap_status(m) :
            (m->regs[offset / 4] ? m->regs[offset / 4] : pwrap_status(m));
    else
        *value = offset == LMP_PMIC_INIT_DONE2 ? 1u : m->regs[offset / 4];
    return LMP_OK;
}

static int
pwrap_write(void *cookie, uint32_t offset, uint32_t value)
{
    struct pwrap_model *m = cookie;
    uint16_t addr;
    assert(m->owned);
    assert(offset < sizeof(m->regs) && !(offset & 3u));
    m->writes++;
    if (offset == LMP_PMIC_WACS2_CMD && !m->stuck) {
        addr = (uint16_t)(((value >> 16) & 0x7fffu) << 1);
        if (value >> 31) {
            m->pmic[addr / 2] = (uint16_t)value;
            if (addr == 0x1508u) m->pmic[0x1520u / 2] = (uint16_t)value;
            if (addr == 0x1bc8u) m->pmic[0x1be4u / 2] = (uint16_t)value;
            if (addr == 0x1526u) m->pmic[0x151eu / 2] = (uint16_t)value;
            if (addr == 0x1bf4u) m->pmic[0x1be2u / 2] = (uint16_t)(value << 8);
        } else if(m->hang_own_read) {
            m->regs[LMP_PMIC_WACS2_RDATA / 4]=(2u<<16)|(1u<<20)|(1u<<21);
        } else
            m->regs[LMP_PMIC_WACS2_RDATA / 4] =
                (uint32_t)m->pmic[addr / 2] | (6u << 16) | (1u << 20) | (1u << 21);
    } else if (offset == LMP_PMIC_WACS2_VLDCLR) {
        m->regs[LMP_PMIC_WACS2_RDATA / 4] = pwrap_status(m);
    }
    return LMP_OK;
}

static void
test_pmic_provider(void)
{
    struct pwrap_model model = {0};
    struct lmp_pmic_io io = {
        .cookie = &model, .read32 = pwrap_read, .write32 = pwrap_write,
        .now_ns = pwrap_now, .delay_us = pwrap_delay, .timeout_ns = 20,
        .acquire = pwrap_acquire, .release = pwrap_release,
        .poison = pwrap_poison,
    };
    struct lmp_pmic_registers regs;
    struct lmp_pmic_state state;
    struct lmp_pmic_ops ops;
    bool enabled;

    lmp_pmic_mt6358_registers(&regs);
    model.pmic[regs.swcid_addr / 2] = LMP_PMIC_CID_E2;
    model.regs[0x000 / 4] = 0;
    model.regs[0x004 / 4] = 1;
    model.regs[0x070 / 4] = 0;
    model.regs[0x090 / 4] = 1;
    struct lmp_pmic_io unowned=io;unowned.acquire=NULL;
    assert(lmp_pmic_probe(&state,&unowned,&regs)==LMP_ENOTSUP);
    assert(!model.reads&&!model.writes&&!model.acquires);
    unowned=io;unowned.poison=NULL;
    assert(lmp_pmic_probe(&state,&unowned,&regs)==LMP_ENOTSUP);
    assert(!model.reads&&!model.writes&&!model.acquires);
    model.deny_owner=true;
    assert(lmp_pmic_probe(&state,&io,&regs)==LMP_EBUSY);
    assert(!model.reads&&!model.writes&&!model.releases);
    model.deny_owner=false;model.acquires=0;
    assert(lmp_pmic_probe(&state, &io, &regs) == LMP_OK);
    assert(lmp_pmic_get_platform_ops(&state, &ops) == LMP_OK);
    assert(ops.is_enabled(ops.cookie, LMP_RAIL_VGPU, &enabled) == LMP_OK && !enabled);
    assert(ops.enable(ops.cookie, LMP_RAIL_VGPU) == LMP_OK);
    assert(ops.set_voltage_uv(ops.cookie, LMP_RAIL_VGPU, 900000) == LMP_OK);
    assert(ops.disable(ops.cookie, LMP_RAIL_VGPU) == LMP_OK);
    assert(lmp_pmic_set_voltage_uv(&state, LMP_RAIL_VSRAM_GPU, 123456) == LMP_EINVAL);
    assert(!model.owned&&model.acquires==model.releases);

    model = (struct pwrap_model){0};
    model.stuck = true;
    model.pmic[regs.swcid_addr / 2] = LMP_PMIC_CID_E2;
    model.regs[0x000 / 4] = 0; model.regs[0x004 / 4] = 1;
    model.regs[0x070 / 4] = 0; model.regs[0x090 / 4] = 1;
    /* the stuck model reports WFVLDCLR, which only its owner may clear */
    assert(lmp_pmic_probe(&state, &io, &regs) == LMP_EBUSY);
    for(unsigned fsm=2;fsm<=4;fsm+=2) {
        model=(struct pwrap_model){0};model.foreign_fsm=fsm;
        model.regs[0x004/4]=model.regs[0x090/4]=1;
        /* a busy foreign FSM is waited out (read-only) and times out */
        assert(lmp_pmic_probe(&state,&io,&regs)==LMP_ETIMEDOUT);
        assert(!model.writes&&!model.owned&&model.acquires==model.releases);
    }
    model=(struct pwrap_model){0};model.foreign_fsm=6; /* LMP_PMIC_FSM_WFVLDCLR */
    model.regs[0x004/4]=model.regs[0x090/4]=1;
    /* WFVLDCLR waits for its owner to clear it; this probe must not */
    assert(lmp_pmic_probe(&state,&io,&regs)==LMP_EBUSY);
    assert(!model.writes&&!model.owned&&model.acquires==model.releases);
    model=(struct pwrap_model){0};model.foreign_req=true;
    model.regs[0x004/4]=model.regs[0x090/4]=1;
    /* a pending foreign request is waited out, never cleared, and times out */
    assert(lmp_pmic_probe(&state,&io,&regs)==LMP_ETIMEDOUT);
    assert(!model.writes&&!model.poisons&&!model.owned);
    model=(struct pwrap_model){0};model.hang_own_read=true;
    model.regs[0x004/4]=model.regs[0x090/4]=1;
    assert(lmp_pmic_probe(&state,&io,&regs)==LMP_ETIMEDOUT);
    assert(model.writes==1&&!model.owned&&model.acquires==model.releases);
    assert(model.poisoned&&model.poisons==1);
    unsigned previous_writes=model.writes;
    assert(lmp_pmic_is_enabled(&state,LMP_RAIL_VGPU,&enabled)==LMP_ENODEV);
    assert(model.writes==previous_writes); /* no late clear/retry of pending command */
    unsigned previous_reads=model.reads;
    assert(lmp_pmic_probe(&state,&io,&regs)==LMP_EIO);
    assert(model.reads==previous_reads&&model.writes==previous_writes); /* no new probe consumes stale response */
    puts("PWRAP: IDLE+REQ foreign command refused without writes; OWN timeout poisons shared channel before release, later probe denied");
    regs.swcid_addr = 1;
    assert(lmp_pmic_validate_registers(&regs) == LMP_EINVAL);
}

int
main(void)
{
    test_platform_fail_closed();
    test_platform_lifecycle();
    test_pmic_provider();
    puts("Lancelot Mali platform/MT6358 provider ASan/UBSan fault tests passed");
    return 0;
}
