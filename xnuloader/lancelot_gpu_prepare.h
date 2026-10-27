/* SPDX-License-Identifier: GPL-2.0-only
 * Firmware-stage GPU power/ID diagnostic, using the same portable platform
 * lifecycle as XNU. Live rails are preserved via the real Mu read-only provider.
 * No XNU WACS2 ownership claim, PMIC write callback or render service. An
 * explicit separately guarded private DMA callback is optional, never default.
 * The caller must remain in UEFI and suppress its AP callbacks across this run.
 */
#ifndef XNULOADER_LANCELOT_GPU_PREPARE_H
#define XNULOADER_LANCELOT_GPU_PREPARE_H
#include "lancelot_pmic.h"
#include "lancelot_gpu_features.h"
#include "../xnu/files/bsd/dev/lancelot_mali_platform.c"

struct lpg_context {
    const efi_mtk_gpu_map *map;
    const efi_mtk_pwrap_checked *pmic;
    void *cookie;
    int (*read)(void *, uint64_t, uint32_t *);
    int (*write)(void *, uint64_t, uint32_t);
    uint64_t (*now_ns)(void *);
    void (*delay_us)(void *, uint32_t);
    uint64_t start;
    uint64_t recovery_start;
    unsigned writes, pmic_reads;
    bool features_requested;
    bool dma_requested;
    bool recovery_active;
    int transport_error;
    uint32_t transport_address;
    void *dma_cookie;
    int (*dma_run)(void *,struct lmp_state *,const struct lpg_features *,bool *);
};
struct lpg_result {
    int error, teardown_error;
    uint32_t gpu_id, gpu_status, source_hz;
    struct lpm_snapshot pmic;
    struct lpg_features features;
    bool features_valid, private_dma_candidate;
    bool started, domains_restored;
    bool dma_attempted,dma_safe_to_teardown,dma_halt_required;
    uint32_t teardown_state;
};
static inline bool lpg_dma_handoff_safe(const struct lpg_result*r) {
    return r && !r->error && !r->teardown_error && r->domains_restored &&
        r->dma_attempted && r->dma_safe_to_teardown && !r->dma_halt_required;
}
/* Read-only XNU diagnostics may continue after platform recovery failed only
 * when the actual core/VM proof already released every DMA resource. This is
 * not a full GPU PASS or permission to power/read the GPU from XNU. */
static inline bool lpg_dma_readonly_resume_safe(const struct lpg_result*r) {
    return r && r->dma_attempted && r->dma_safe_to_teardown && !r->dma_halt_required;
}
/* Final loader decision: a failed test is not evidence of live DMA. Preserve
 * the lifecycle's uncertainty flag and require the actual arena release too. */
static inline void lpg_dma_finish_handoff(struct lpg_result*r,bool released) {
    r->dma_halt_required=r->dma_halt_required || !r->dma_attempted ||
        !r->dma_safe_to_teardown || !released;
}
static const uint64_t lpg_base[6] = {
    0x10006000,0x10001000,0x13000000,0x13040000,0x10000000,0x1000c000
};
static bool lpg_map_valid(const efi_mtk_gpu_map *map) {
    if (!map || map->revision != 1 || map->size != sizeof(*map)) return false;
    for (unsigned i=0;i<6;i++)
        if (map->window[i].base!=lpg_base[i] || map->window[i].size!=(i==3?0x4000u:0x1000u)) return false;
    return true;
}
static bool lpg_expired(struct lpg_context *c) {
    uint64_t now=c->now_ns(c->cookie);
    uint64_t start=c->recovery_active?c->recovery_start:c->start;
    uint64_t limit=c->recovery_active?UINT64_C(1000000000):UINT64_C(2000000000);
    return now<start || now-start>=limit;
}
static int lpg_transport_read(struct lpg_context*c,uint64_t pa,uint32_t*out) {
    if(c->transport_error)return c->transport_error;
    int e=c->read(c->cookie,pa,out);
    if(e){c->transport_error=e;c->transport_address=(uint32_t)pa;}
    return e;
}
static int lpg_transport_write(struct lpg_context*c,uint64_t pa,uint32_t value) {
    if(c->transport_error)return c->transport_error;
    int e=c->write(c->cookie,pa,value);
    if(e){c->transport_error=e;c->transport_address=(uint32_t)pa;}
    return e;
}
static int lpg_begin_recovery(struct lpg_context*c) {
    uint64_t now=c->now_ns(c->cookie);
    if(c->transport_error||c->recovery_active||now<c->start||now-c->start<UINT64_C(2000000000))return LMP_EIO;
    c->recovery_start=now;c->recovery_active=true;return LMP_OK;
}
static bool lpg_read_allowed(enum lmp_bank bank,uint32_t off) {
    switch(bank) {
    case LMP_BANK_SPM:
        return off==0 || off==LMP_SPM_PWR_STATUS || off==LMP_SPM_PWR_STATUS_2ND ||
            off==LMP_SPM_MFG_PWR_CON || off==LMP_SPM_MFG_CORE0_PWR_CON ||
            off==LMP_SPM_MFG_CORE1_PWR_CON || off==LMP_SPM_MFG_ASYNC_PWR_CON;
    case LMP_BANK_INFRA:return off==LMP_INFRA_PROTECTEN || off==LMP_INFRA_PROTECTEN_STA1;
    case LMP_BANK_MFGCFG:return off==LMP_MFG_CG_CON;
    case LMP_BANK_GPU:return off==LMP_GPU_ID || off==LMP_GPU_STATUS || lpg_feature_read_allowed(off);
    case LMP_BANK_TOP:return off==LMP_TOP_CLK_CFG_1;
    case LMP_BANK_APMIXED:
        return off==LMP_APMIXED_MFGPLL_CON0 || off==LMP_APMIXED_MFGPLL_CON0+4 || off==LMP_APMIXED_MFGPLL_CON3 ||
            off==LMP_APMIXED_MAINPLL_CON0 || off==LMP_APMIXED_MAINPLL_CON0+4 || off==LMP_APMIXED_MAINPLL_CON3 ||
            off==LMP_APMIXED_UNIVPLL_CON0 || off==LMP_APMIXED_UNIVPLL_CON0+4 || off==LMP_APMIXED_UNIVPLL_CON3;
    default:return false;
    }
}
static int lpg_domains_live(struct lpg_context *c,uint32_t mask) {
    uint32_t first,second;
    int e=lpg_transport_read(c,lpg_base[0]+LMP_SPM_PWR_STATUS,&first);
    if(!e)e=lpg_transport_read(c,lpg_base[0]+LMP_SPM_PWR_STATUS_2ND,&second);
    if(e)return e;
    return ((first&mask)==mask && (second&mask)==mask)?0:LMP_ENODEV;
}
static int lpg_read_bank(void *cookie,enum lmp_bank bank,uint32_t off,uint32_t *out) {
    struct lpg_context *c=cookie;
    if(!out || !lpg_read_allowed(bank,off))return LMP_EINVAL;
    if(lpg_expired(c))return LMP_ETIMEDOUT;
    if(bank==LMP_BANK_MFGCFG || bank==LMP_BANK_GPU) {
        uint32_t mask=bank==LMP_BANK_GPU?LMP_MFG_DOMAIN_MASK:(LMP_MFG_PWR_STA_MASK|LMP_MFG_ASYNC_STA_MASK);
        int e=lpg_domains_live(c,mask);if(e)return e;
        if(bank==LMP_BANK_GPU) {
            uint32_t clock,protect,ack;
            e=lpg_transport_read(c,lpg_base[LMP_BANK_MFGCFG]+LMP_MFG_CG_CON,&clock);
            if(!e)e=lpg_transport_read(c,lpg_base[LMP_BANK_INFRA]+LMP_INFRA_PROTECTEN,&protect);
            if(!e)e=lpg_transport_read(c,lpg_base[LMP_BANK_INFRA]+LMP_INFRA_PROTECTEN_STA1,&ack);
            if(e)return e;
            if((clock&1u) || ((protect|ack)&LMP_MFG_PROT_MASK))return LMP_ENODEV;
        }
    }
    int e=lpg_transport_read(c,lpg_base[bank]+off,out);
    return !e && lpg_expired(c)?LMP_ETIMEDOUT:e;
}
static inline int lpg_gpu_access_guard(struct lpg_context *c) {
    if(!c || lpg_expired(c))return LMP_ETIMEDOUT;
    int e=lpg_domains_live(c,LMP_MFG_DOMAIN_MASK);if(e)return e;
    uint32_t clock,protect,ack,status;
    e=lpg_transport_read(c,lpg_base[LMP_BANK_MFGCFG]+LMP_MFG_CG_CON,&clock);
    if(!e)e=lpg_transport_read(c,lpg_base[LMP_BANK_INFRA]+LMP_INFRA_PROTECTEN,&protect);
    if(!e)e=lpg_transport_read(c,lpg_base[LMP_BANK_INFRA]+LMP_INFRA_PROTECTEN_STA1,&ack);
    if(!e)e=lpg_transport_read(c,lpg_base[LMP_BANK_GPU]+LMP_GPU_STATUS,&status);
    if(e)return e;
    if((clock&1u) || ((protect|ack)&LMP_MFG_PROT_MASK) || (status&(1u<<7)))return LMP_ENODEV;
    return lpg_expired(c)?LMP_ETIMEDOUT:LMP_OK;
}
static int lpg_write_bank(void *cookie,enum lmp_bank bank,uint32_t off,uint32_t value) {
    struct lpg_context *c=cookie;
    if(lpg_expired(c))return LMP_ETIMEDOUT;
    if(bank==LMP_BANK_SPM) {
        if(off!=LMP_SPM_MFG_PWR_CON && off!=LMP_SPM_MFG_CORE0_PWR_CON &&
           off!=LMP_SPM_MFG_CORE1_PWR_CON && off!=LMP_SPM_MFG_ASYNC_PWR_CON)return LMP_EINVAL;
        uint32_t old;int e=lpg_transport_read(c,lpg_base[bank]+off,&old);if(e)return e;
        const uint32_t mask=LMP_PWR_RST_B|LMP_PWR_ISO|LMP_PWR_ON|LMP_PWR_ON_2ND|LMP_PWR_CLK_DIS|LMP_SRAM_PDN|LMP_SRAM_PDN_ACK;
        if((old^value)&~mask)return LMP_EINVAL;
    } else if(bank==LMP_BANK_INFRA) {
        if((off!=LMP_INFRA_PROTECTEN_SET && off!=LMP_INFRA_PROTECTEN_CLR) ||
           (value!=LMP_MFG_PROT_STEP1_MASK && value!=LMP_MFG_PROT_STEP2_MASK))return LMP_EINVAL;
    } else if(bank==LMP_BANK_MFGCFG) {
        if((off!=LMP_MFG_CG_SET && off!=LMP_MFG_CG_CLR) || value!=1)return LMP_EINVAL;
        int e=lpg_domains_live(c,LMP_MFG_PWR_STA_MASK|LMP_MFG_ASYNC_STA_MASK);if(e)return e;
    } else return LMP_ENOTSUP; // No TOP/PLL/GPU/PMIC command writes.
    int e=lpg_transport_write(c,lpg_base[bank]+off,value);c->writes++;
    return !e && lpg_expired(c)?LMP_ETIMEDOUT:e;
}
static uint64_t lpg_now(void *cookie) {struct lpg_context*c=cookie;return c->now_ns(c->cookie);}
static void lpg_delay(void *cookie,uint32_t us) {struct lpg_context*c=cookie;c->delay_us(c->cookie,us);}
static int lpg_pmic_read(struct lpg_context *c,uint16_t address,uint16_t *out) {
    if(lpg_expired(c))return LMP_ETIMEDOUT;
    efi_status status=c->pmic->read(address,out);c->pmic_reads++;
    if(!status && lpg_expired(c))return LMP_ETIMEDOUT;
    return status==0?0:status==(EFI_ERROR_BIT|18)?LMP_ETIMEDOUT:LMP_EIO;
}
static int lpg_rail_enabled(void *cookie,enum lmp_rail rail,bool *out) {
    struct lpg_context*c=cookie;uint16_t sw,da;
    if(!out || rail>=LMP_RAIL_COUNT)return LMP_EINVAL;
    int e=lpg_pmic_read(c,rail==0?0x1508:0x1bc8,&sw);
    if(!e)e=lpg_pmic_read(c,rail==0?0x1520:0x1be4,&da);
    if(e)return e;
    if((sw^da)&1)return LMP_EIO;
    *out=(da&1)!=0;return 0;
}
static int lpg_rail_voltage(void *cookie,enum lmp_rail rail,uint32_t *uv) {
    struct lpg_context*c=cookie;uint16_t sw,da;bool enabled;
    int e=lpg_rail_enabled(cookie,rail,&enabled);if(e)return e;
    if(!enabled || !uv)return LMP_ENODEV;
    e=lpg_pmic_read(c,rail==0?0x1526:0x1bf4,&sw);
    if(!e)e=lpg_pmic_read(c,rail==0?0x151e:0x1be2,&da);
    if(e)return e;
    sw&=0x7f;da=(da>>(rail==0?0:8))&0x7f;
    if(sw!=da)return LMP_EIO;
    *uv=500000u+da*6250u;return 0; // DA selector encoding, not ADC measurement.
}
static int lpg_feature_platform_read(void *cookie,uint32_t off,uint32_t *out) {
    return lmp_gpu_read32((struct lmp_state *)cookie,off,out);
}
static struct lpg_result lpg_observed_return(struct lpg_result r,const struct lmp_state*s) {
    r.teardown_state=(s->initialized?1u:0u)|(s->started?2u:0u)|
        (s->failed?4u:0u)|(s->teardown_safe?8u:0u)|(s->gpu_ready?16u:0u);
    return r;
}
static struct lpg_result lpg_run(struct lpg_context *c) {
    struct lpg_result r={.error=LMP_ENOTSUP};struct lmp_state state;
    if(!c || !lpg_map_valid(c->map) || !c->pmic || !c->pmic->read ||
       !c->read || !c->write || !c->now_ns || !c->delay_us)return r;
    if(c->dma_requested && (!c->features_requested || !c->dma_run))return r;
    c->start=c->now_ns(c->cookie);c->writes=c->pmic_reads=0;c->recovery_active=false;
    efi_status pe=lpm_capture(c->pmic,&r.pmic);
    if(pe){r.error=LMP_EIO;return r;}
    uint32_t cfg;
    r.error=lpg_read_bank(c,LMP_BANK_SPM,0,&cfg);
    if(r.error)return r;
    if(!(cfg&1)){r.error=LMP_ENOTSUP;return r;} // Never unlock global SPM configuration.
    struct lmp_config config={LMP_PLATFORM_LANCELOT_MT6768,LMP_MODE_FULL,false,LMP_DEFAULT_TIMEOUT_NS};
    struct lmp_io_ops ops={.cookie=c,.read32=lpg_read_bank,.write32=lpg_write_bank,
        .now_ns=lpg_now,.delay_us=lpg_delay,.pmic={.cookie=c,.is_enabled=lpg_rail_enabled,.get_voltage_uv=lpg_rail_voltage}};
    r.error=lmp_init(&state,&config,&ops);if(r.error)return lpg_observed_return(r,&state);
    struct lmp_status status;
    r.error=lmp_status(&state,&status);if(r.error)return lpg_observed_return(r,&state);
    if((status.pwr_status|status.pwr_status_2nd)&LMP_MFG_DOMAIN_MASK){r.error=LMP_EBUSY;return lpg_observed_return(r,&state);}
    const uint32_t ctrl[]={LMP_SPM_MFG_PWR_CON,LMP_SPM_MFG_CORE0_PWR_CON,
        LMP_SPM_MFG_CORE1_PWR_CON,LMP_SPM_MFG_ASYNC_PWR_CON};
    for(unsigned i=0;i<4;i++) {
        uint32_t value;r.error=lpg_read_bank(c,LMP_BANK_SPM,ctrl[i],&value);if(r.error)return lpg_observed_return(r,&state);
        if((value&(LMP_PWR_ON|LMP_PWR_ON_2ND|LMP_PWR_RST_B)) ||
           (value&(LMP_PWR_ISO|LMP_PWR_CLK_DIS))!=(LMP_PWR_ISO|LMP_PWR_CLK_DIS) ||
           (i<3 && (value&(LMP_SRAM_PDN|LMP_SRAM_PDN_ACK))!=(LMP_SRAM_PDN|LMP_SRAM_PDN_ACK))) {
            r.error=LMP_EBUSY;return lpg_observed_return(r,&state); // Do not wait out an inherited pending power transition.
        }
    }
    uint32_t protect;r.error=lpg_read_bank(c,LMP_BANK_INFRA,LMP_INFRA_PROTECTEN,&protect);if(r.error)return lpg_observed_return(r,&state);
    protect&=LMP_MFG_PROT_MASK;
    if(protect && protect!=LMP_MFG_PROT_MASK){r.error=LMP_EBUSY;return lpg_observed_return(r,&state);}
    r.error=lmp_start(&state);r.source_hz=state.source_hz;r.started=state.started;
    if(r.error)return lpg_observed_return(r,&state); // Failed partial state is retained, not guessed away.
    r.error=lmp_gpu_read32(&state,LMP_GPU_ID,&r.gpu_id);
    if(!r.error)r.error=lmp_gpu_read32(&state,LMP_GPU_STATUS,&r.gpu_status);
    if(r.error){state.failed=true;r.teardown_error=LMP_EBUSY;return lpg_observed_return(r,&state);}
    if(r.gpu_status&(1u<<7)){r.error=LMP_ENOTSUP;r.teardown_error=LMP_EBUSY;return lpg_observed_return(r,&state);}
    if((r.gpu_id&0xf00f0000u)!=0x70020000u)r.error=LMP_ENOTSUP;
    if(!r.error && c->features_requested) {
        r.error=lpg_features_capture(&state,lpg_feature_platform_read,&r.features);
        if(r.error){r.teardown_error=LMP_EBUSY;return lpg_observed_return(r,&state);}
        r.features_valid=true;
        r.private_dma_candidate=lpg_features_private_dma_candidate(&r.features);
    }
    uint16_t swcid;
    if(!r.error)r.error=lpg_pmic_read(c,0xa,&swcid);
    if(!r.error && swcid!=r.pmic.raw[0])r.error=LMP_EIO;
    if(r.error && r.error!=LMP_ENOTSUP){r.teardown_error=LMP_EBUSY;return lpg_observed_return(r,&state);}
    if(c->dma_requested) {
        if(r.error || !r.private_dma_candidate){r.error=LMP_ENOTSUP;}
        else {
            r.dma_attempted=true;
            r.error=c->dma_run(c->dma_cookie,&state,&r.features,&r.dma_safe_to_teardown);
            r.teardown_state=(state.initialized?1u:0u)|(state.started?2u:0u)|
                (state.failed?4u:0u)|(state.teardown_safe?8u:0u)|(state.gpu_ready?16u:0u);
            if(!r.dma_safe_to_teardown) {
                r.dma_halt_required=true;state.failed=true;r.teardown_error=LMP_EBUSY;
                if(!r.error)r.error=LMP_EIO;
                return lpg_observed_return(r,&state);
            }
        }
    }
    r.teardown_state=(state.initialized?1u:0u)|(state.started?2u:0u)|
        (state.failed?4u:0u)|(state.teardown_safe?8u:0u)|(state.gpu_ready?16u:0u);
    r.teardown_error=lmp_teardown(&state);
    if(!r.teardown_error)r.teardown_error=lmp_status(&state,&status);
    r.domains_restored=!r.teardown_error && !((status.pwr_status|status.pwr_status_2nd)&LMP_MFG_DOMAIN_MASK);
    if(!r.error && !r.domains_restored)r.error=r.teardown_error?r.teardown_error:LMP_EIO;
    return r;
}
#endif
