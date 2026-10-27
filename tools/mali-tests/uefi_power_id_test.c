/* Actual firmware adapter and shared platform lifecycle. Hardware is RAM only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_gpu_prepare.h"

struct model {
    uint32_t reg[6][1024];uint16_t pmic[0x8000];uint64_t ns;
    unsigned reads,writes,gpu_reads,fail_write,fail_gpu_at;
    bool hung,frozen,fail_gpu,unstable_features,protect_late;
};
static struct model *active;
static int locate(uint64_t pa,unsigned *bank,unsigned *word) {
    for(unsigned i=0;i<6;i++)if(pa>=lpg_base[i]&&pa<lpg_base[i]+0x1000) {
        assert(!(pa&3));*bank=i;*word=(unsigned)((pa-lpg_base[i])/4);return 0;
    }
    return -1;
}
static int read_hw(void *cookie,uint64_t pa,uint32_t *out) {
    struct model*m=cookie;unsigned bank,word;assert(!locate(pa,&bank,&word));
    m->reads++;
    if(bank==LMP_BANK_GPU) {
        m->gpu_reads++;
        assert((m->reg[0][LMP_SPM_PWR_STATUS/4]&LMP_MFG_DOMAIN_MASK)==LMP_MFG_DOMAIN_MASK);
        assert((m->reg[0][LMP_SPM_PWR_STATUS_2ND/4]&LMP_MFG_DOMAIN_MASK)==LMP_MFG_DOMAIN_MASK);
        assert(!(m->reg[2][0]&1));
        if(m->fail_gpu || m->gpu_reads==m->fail_gpu_at)return LMP_EIO;
    }
    *out=m->reg[bank][word];
    if(bank==LMP_BANK_GPU && m->gpu_reads>2+LPG_FEATURE_WORDS+2) {
        if(m->unstable_features && word==LMP_GPU_ID/4)*out^=1;
        if(m->protect_late && word==LMP_GPU_STATUS/4)*out|=1u<<7;
    }
    return 0;
}
static int write_hw(void *cookie,uint64_t pa,uint32_t value) {
    struct model*m=cookie;unsigned bank,word;assert(!locate(pa,&bank,&word));m->writes++;
    assert(bank==LMP_BANK_SPM||bank==LMP_BANK_INFRA||bank==LMP_BANK_MFGCFG);
    unsigned off=word*4;
    if(m->hung)return 0;
    if(bank==LMP_BANK_INFRA) {
        assert(off==LMP_INFRA_PROTECTEN_SET||off==LMP_INFRA_PROTECTEN_CLR);
        if(off==LMP_INFRA_PROTECTEN_SET){m->reg[1][0x220/4]|=value;m->reg[1][0x228/4]|=value;}
        else{m->reg[1][0x220/4]&=~value;m->reg[1][0x228/4]&=~value;}
    } else if(bank==LMP_BANK_MFGCFG) {
        assert(value==1&&(off==4||off==8));
        if(off==4)m->reg[2][0]|=1;else m->reg[2][0]&=~1u;
    } else {
        uint32_t mask=off==0x338?1u<<11:off==0x34c?1u<<12:off==0x310?1u<<13:off==0x334?1u<<14:0;
        assert(mask);
        uint32_t old=m->reg[0][word];
        if(!(old&(LMP_PWR_ON|LMP_PWR_ON_2ND))&&(value&LMP_PWR_ON))
            assert(!(value&LMP_PWR_ON_2ND)); // source's two distinct power-on writes
        if((value&(LMP_PWR_ON|LMP_PWR_ON_2ND))==(LMP_PWR_ON|LMP_PWR_ON_2ND)) {
            m->reg[0][0x180/4]|=mask;m->reg[0][0x184/4]|=mask;
        } else if(!(value&(LMP_PWR_ON|LMP_PWR_ON_2ND))) {
            m->reg[0][0x180/4]&=~mask;m->reg[0][0x184/4]&=~mask;
        }
        if(off!=0x334) {
            if(value&LMP_SRAM_PDN)value|=LMP_SRAM_PDN_ACK;else value&=~LMP_SRAM_PDN_ACK;
        }
        m->reg[0][word]=value;
    }
    return m->writes==m->fail_write?LMP_EIO:0; // error after possibly applied write
}
static uint64_t now_hw(void *cookie){return ((struct model*)cookie)->ns;}
static void delay_hw(void *cookie,uint32_t us){struct model*m=cookie;if(!m->frozen)m->ns+=(uint64_t)us*1000;}
static efi_status read_pmic(uint16_t address,uint16_t *out) {
    assert(!(address&1));*out=active->pmic[address/2];return 0;
}
static void init(struct model*m,efi_mtk_gpu_map*map,struct lpg_context*c) {
    memset(m,0,sizeof(*m));memset(map,0,sizeof(*map));
    map->revision=1;map->size=sizeof(*map);
    for(unsigned i=0;i<6;i++){map->window[i].base=lpg_base[i];map->window[i].size=i==3?0x4000:0x1000;}
    static const efi_mtk_pwrap_checked p={read_pmic,NULL,NULL};
    *c=(struct lpg_context){.map=map,.pmic=&p,.cookie=m,.read=read_hw,.write=write_hw,.now_ns=now_hw,.delay_us=delay_hw};
    active=m;m->reg[0][0]=1;m->reg[0][0x338/4]=m->reg[0][0x34c/4]=m->reg[0][0x310/4]=0x1112;
    m->reg[0][0x334/4]=0x112;m->reg[2][0]=1;
    m->reg[4][0x50/4]=0x01010201;m->reg[5][0x248/4]=0x1e1;m->reg[5][0x24c/4]=0x021493b1;m->reg[5][0x254/4]=0x80000001;
    m->reg[3][0]=0x74021000;
    /* Fixture from accepted, framed Android cached GPUPROPS capture2.
     * This is RAM, not a replacement for live loader feature evidence. */
    m->reg[3][4/4]=0x07120206;m->reg[3][8/4]=2;m->reg[3][12/4]=0x209;
    m->reg[3][16/4]=1;m->reg[3][20/4]=0x2823;m->reg[3][24/4]=0xff;m->reg[3][28/4]=7;
    m->reg[3][0xa0/4]=0x300;m->reg[3][0xa4/4]=m->reg[3][0xa8/4]=0x180;
    m->reg[3][0xac/4]=0xa046000;m->reg[3][0x310/4]=0x400;
    m->reg[3][0xb0/4]=0xf7fe03fe;m->reg[3][0xb4/4]=0xc3fff7ff;m->reg[3][0xb8/4]=0xbfe1ff9f;
    /* Live loader proved 0xbc=0x10c6; the exact Android r54 binary's
     * property sampler aliases its fourth texture field to JS0=0x20e. */
    m->reg[3][0xbc/4]=0x10c6;m->reg[3][0xc0/4]=0x20e;m->reg[3][0xc4/4]=0x4fe;m->reg[3][0xc8/4]=0x7e;
    m->reg[3][0x100/4]=m->reg[3][0xe00/4]=3;m->reg[3][0x110/4]=m->reg[3][0x120/4]=1;
    m->pmic[0xa/2]=0x5820;m->pmic[0x1508/2]=1;m->pmic[0x1520/2]=0x43;m->pmic[0x1526/2]=0x30;m->pmic[0x151e/2]=0x2830;
    m->pmic[0x1bc8/2]=1;m->pmic[0x1be4/2]=3;m->pmic[0x1bf4/2]=0x40;m->pmic[0x1be2/2]=0x4060;
}
int main(void) {
    struct model m;efi_mtk_gpu_map map;struct lpg_context c;struct lpg_result r;
    assert(lpm_requested("lancelot_pmic_probe=0")==0);
    assert(lpm_requested_named("lancelot_gpu_power=1 lancelot_gpu_power=0","lancelot_gpu_power=")==-1);
    init(&m,&map,&c);r=lpg_run(&c);
    assert(!r.error&&r.started&&r.domains_restored&&r.gpu_id==0x74021000&&r.source_hz==534999909);
    assert(m.gpu_reads==2&&m.writes>0&&!m.reg[0][0x180/4]&&!m.reg[0][0x184/4]);
    assert(m.pmic[0x1508/2]==1&&m.pmic[0x1bc8/2]==1&&m.pmic[0x1526/2]==0x30&&m.pmic[0x1bf4/2]==0x40);
    unsigned successful_writes=m.writes;
    init(&m,&map,&c);map.window[3].size=0x2000;r=lpg_run(&c);assert(r.error==LMP_ENOTSUP&&!m.reads&&!m.writes);
    init(&m,&map,&c);c.pmic=NULL;r=lpg_run(&c);assert(r.error==LMP_ENOTSUP&&!m.reads&&!m.writes);
    init(&m,&map,&c);m.reg[0][0]=0;r=lpg_run(&c);assert(r.error==LMP_ENOTSUP&&!m.writes);
    init(&m,&map,&c);m.reg[0][0x180/4]=1u<<11;r=lpg_run(&c);assert(r.error==LMP_EBUSY&&!m.writes);
    init(&m,&map,&c);m.reg[0][0x338/4]|=LMP_PWR_ON;r=lpg_run(&c);assert(r.error==LMP_EBUSY&&!m.writes);
    init(&m,&map,&c);m.reg[1][0x220/4]=LMP_MFG_PROT_STEP1_MASK;r=lpg_run(&c);assert(r.error==LMP_EBUSY&&!m.writes);
    init(&m,&map,&c);m.pmic[0x1bc8/2]=m.pmic[0x1be4/2]=0;r=lpg_run(&c);assert(r.error==LMP_ENOTSUP&&!m.writes);
    init(&m,&map,&c);m.pmic[0x1be2/2]=0x4160;r=lpg_run(&c);assert(r.error==LMP_EIO&&!m.writes);
    init(&m,&map,&c);m.hung=m.frozen=true;r=lpg_run(&c);assert(r.error==LMP_ETIMEDOUT&&!m.gpu_reads&&!r.domains_restored);
    for(unsigned i=1;i<=successful_writes;i++) {
        init(&m,&map,&c);m.fail_write=i;r=lpg_run(&c);
        assert(r.error!=0&&!r.domains_restored);
    }
    init(&m,&map,&c);m.fail_gpu=true;r=lpg_run(&c);assert(r.error==LMP_EIO&&r.teardown_error==LMP_EBUSY&&!r.domains_restored);
    init(&m,&map,&c);m.reg[3][0]=0x12340000;r=lpg_run(&c);assert(r.error==LMP_ENOTSUP&&r.domains_restored);
    init(&m,&map,&c);m.reg[3][0x34/4]=1u<<7;r=lpg_run(&c);assert(r.error==LMP_ENOTSUP&&!r.domains_restored);
    init(&m,&map,&c);c.features_requested=true;r=lpg_run(&c);
    assert(!r.error&&r.domains_restored&&r.features_valid&&r.private_dma_candidate);
    assert(m.gpu_reads==2+2*(LPG_FEATURE_WORDS+2)&&m.writes==successful_writes);
    assert(r.features.word[5]==0x2823 && r.features.word[6]==0xff && r.features.word[33]==3);
    unsigned sample_reads=m.gpu_reads;
    for(unsigned i=3;i<=sample_reads;i++) {
        init(&m,&map,&c);c.features_requested=true;m.fail_gpu_at=i;r=lpg_run(&c);
        assert(r.error==LMP_EIO&&!r.features_valid&&!r.domains_restored&&r.teardown_error==LMP_EBUSY);
        for(unsigned j=0;j<LPG_FEATURE_WORDS;j++)assert(!r.features.word[j]);
    }
    init(&m,&map,&c);c.features_requested=m.unstable_features=true;r=lpg_run(&c);
    assert(r.error==LMP_EIO&&!r.features_valid&&!r.domains_restored);
    init(&m,&map,&c);c.features_requested=m.protect_late=true;r=lpg_run(&c);
    assert(r.error==LMP_ENOTSUP&&!r.features_valid&&!r.domains_restored);
    init(&m,&map,&c);c.features_requested=true;m.reg[3][0xc4/4]&=~(1u<<2);r=lpg_run(&c);
    assert(!r.error&&r.features_valid&&r.domains_restored&&!r.private_dma_candidate);
    init(&m,&map,&c);uint32_t sentinel=0x12345678;
    assert(lpg_read_bank(&c,LMP_BANK_GPU,0x014,&sentinel)==LMP_ENODEV);
    assert(!m.gpu_reads&&sentinel==0x12345678);
    assert(lpg_read_bank(&c,LMP_BANK_GPU,0x030,&sentinel)==LMP_EINVAL);
    assert(lpg_read_bank(&c,LMP_BANK_GPU,0x2400,&sentinel)==LMP_EINVAL);
    assert(lpg_write_bank(&c,LMP_BANK_GPU,0x030,1)==LMP_ENOTSUP&&!m.writes);
    assert(lpm_requested_named("lancelot_gpu_features=1 lancelot_gpu_features=0","lancelot_gpu_features=")==-1);
    struct lpg_result handoff={.domains_restored=true,.dma_attempted=true,.dma_safe_to_teardown=true};
    assert(lpg_dma_handoff_safe(&handoff));
    handoff.error=LMP_ETIMEDOUT;assert(!lpg_dma_handoff_safe(&handoff));handoff.error=0;
    handoff.domains_restored=false;assert(!lpg_dma_handoff_safe(&handoff));handoff.domains_restored=true;
    handoff.dma_attempted=false;assert(!lpg_dma_handoff_safe(&handoff));
    assert(!lpg_dma_readonly_resume_safe(&handoff));handoff.dma_attempted=true;
    assert(lpg_dma_readonly_resume_safe(&handoff));
    handoff.dma_safe_to_teardown=false;assert(!lpg_dma_readonly_resume_safe(&handoff));
    /* attempt4 screen: cleanup complete, job not submitted, outer teardown
     * failed. Exercise the same finalizer the loader calls after lpg_run. */
    handoff=(struct lpg_result){.error=LMP_EIO,.teardown_error=LMP_EBUSY,
        .dma_attempted=true,.dma_safe_to_teardown=true};
    lpg_dma_finish_handoff(&handoff,true);
    assert(!handoff.dma_halt_required&&!lpg_dma_handoff_safe(&handoff));
    assert(lpg_dma_readonly_resume_safe(&handoff));
    lpg_dma_finish_handoff(&handoff,true);assert(lpg_dma_readonly_resume_safe(&handoff));
    for(unsigned failure=0;failure<4;failure++) {
        handoff=(struct lpg_result){.dma_attempted=true,.dma_safe_to_teardown=true};
        if(failure==0)handoff.dma_safe_to_teardown=false;
        if(failure==1)handoff.dma_halt_required=true;
        if(failure==2)handoff.dma_attempted=false;
        lpg_dma_finish_handoff(&handoff,failure!=3);
        assert(handoff.dma_halt_required&&!lpg_dma_readonly_resume_safe(&handoff));
    }
    puts("Loader handoff: cleaned failed DMA/outer teardown resumes read-only; uncertain, unprepared and unreleased cases refuse handoff; RAM ONLY");
    init(&m,&map,&c);m.ns=UINT64_C(1999999999);
    assert(lpg_begin_recovery(&c)==LMP_EIO&&!c.recovery_active);
    m.ns=UINT64_C(2000000000);assert(!lpg_begin_recovery(&c)&&!lpg_expired(&c));
    assert(lpg_begin_recovery(&c)==LMP_EIO);
    m.ns=UINT64_C(2999999999);assert(!lpg_expired(&c));
    m.ns=UINT64_C(3000000000);assert(lpg_expired(&c));
    init(&m,&map,&c);m.ns=UINT64_C(2000000000);c.transport_error=LMP_ETIMEDOUT;
    assert(lpg_begin_recovery(&c)==LMP_EIO&&!c.recovery_active);
    init(&m,&map,&c);c.start=10;m.ns=9;
    assert(lpg_begin_recovery(&c)==LMP_EIO&&!c.recovery_active&&lpg_expired(&c));
    puts("Actual firmware GPU power/ID adapter: mapping/provider/rail guards, ordered domain writes, zero PMIC/GPU commands, ID-only read, owned teardown and all posted-write faults PASS; RAM ONLY");
    puts("G52 readonly features: 42 words twice, all 88 sample read faults, unstable/late protected samples never published, SET_VALUE qualification and command/AS deny guards PASS; RAM ONLY");
    return 0;
}
