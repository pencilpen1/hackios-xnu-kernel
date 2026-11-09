#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_platform.h"

/* Offline register/fault model ONLY. The actual production controller is
 * linked separately; these callbacks must never be used by XNU. */
struct model {
    uint32_t reg[LMP_BANK_COUNT][4096], parent;
    uint32_t uv[2], top_other, dbg, misc;
    uint64_t now, pll_written;
    unsigned calls, fail_at, hit, reads, writes, rail_writes, pll_writes, mux_writes;
    unsigned revoked, guards, meters;
    bool fail_after, locked, foreign, unknown, busy, detached, caps;
    bool drop_park, drop_return, drop_pll, stuck_meter, freeze_time;
    bool drop_meter_start, drop_meter_select, bad_pll_delay, meter_first_read;
    bool skew_meter, drift_rails, drift_main, caps_stale, caps_incomplete;
    bool limit_fault;
    bool caps_future;
    uint32_t provider_mask, provider_started_mask, provider_fault_mask;
    uint32_t provider_cap[3], reduce_cap_after;
    int absent_error, aggregate_error;
    uint32_t cap, cap_lost_after;
};
static unsigned pairs, attach_faults, up_faults, down_faults, ambiguous_writes;
static unsigned negative_cases, governor_cases;
static unsigned fallback_pairs, optional_combinations, optional_error_cases;
static unsigned load_policy_cases, live_lower_cap_cases, runtime_provider_cases;
static unsigned provider_io_failstops, missing_provider_io_faults;
static int fault(struct model *m) {
    m->calls++;
    if (m->fail_at && m->calls == m->fail_at) { m->hit++; return LMP_EIO; }
    return 0;
}
static uint32_t actual_hz(struct model *m) {
    uint32_t hz;
    assert(m->parent == 1 || m->parent == 2);
    assert(lgd_pll_hz(m->reg[LMP_BANK_APMIXED][(m->parent == 1 ? LMP_APMIXED_MFGPLL_CON1 : LMP_APMIXED_MAINPLL_CON0 + 4)/4], m->parent == 1 ? 1 : 3, &hz));
    return hz;
}
static void analog_safe(struct model *m) {
    struct lgd_opp floor;
    assert(lgd_rail_pair_valid(m->uv[0], m->uv[1]));
    assert(lgd_floor_opp(actual_hz(m), &floor));
    assert(m->uv[0] >= floor.vgpu_uv);
}
static uint64_t now_ns(void *p) { struct model *m=p; if (!m->freeze_time) m->now+=1000; return m->now; }
static void delay_us(void *p, uint32_t us) { struct model *m=p; if (!m->freeze_time && !(m->bad_pll_delay&&us==20)) m->now+=(uint64_t)us*1000; }
static int rd(void *p, enum lmp_bank b, uint32_t o, uint32_t *v) {
    struct model *m=p; assert(b < LMP_BANK_COUNT && !(o&3u)); m->reads++;
    if (fault(m)) return LMP_EIO;
    *v=m->reg[b][o/4];
    if(b==LMP_BANK_TOP && o==LMP_TOP_CLK26CALI_0 && m->meter_first_read) {
        *v=0x1010u;m->meter_first_read=false;
    }
    if (m->drift_main && b==LMP_BANK_APMIXED && o==LMP_APMIXED_MAINPLL_CON0+4) *v ^= m->reads & 1u;
    return 0;
}
static int wr(void *p, enum lmp_bank b, uint32_t o, uint32_t v) {
    struct model *m=p; int e=fault(m); m->writes++;
    if (e && !m->fail_after) return e;
    assert(m->locked && !m->foreign && !m->unknown && !m->busy && m->detached);
    if (b==LMP_BANK_TOP) {
        if (o==LMP_TOP_CLK_CFG_1_CLR) { assert(v==3u); m->reg[b][LMP_TOP_CLK_CFG_1/4]&=~v; m->mux_writes++; }
        else if (o==LMP_TOP_CLK_CFG_1_SET) { assert(v==1u || v==2u); m->reg[b][LMP_TOP_CLK_CFG_1/4]|=v; m->mux_writes++; }
        else if (o==LMP_TOP_CLK_CFG_UPDATE) {
            assert(v==LMP_TOP_MFG_UPDATE); uint32_t parent=m->reg[b][LMP_TOP_CLK_CFG_1/4]&3u;
            if (parent==1) assert(m->now-m->pll_written>=20000u);
            if (!((parent==2 && m->drop_park) || (parent==1 && m->drop_return))) m->parent=parent;
            m->reg[b][o/4]=0; m->mux_writes++; analog_safe(m);
        } else if (o==LMP_TOP_CLK26CALI_0) {
            if(v==0x1010u && m->drop_meter_start)return e;
            m->reg[b][o/4]=v;
            if (v==0x1010u && !m->stuck_meter) {
                assert((m->reg[b][LMP_TOP_CLK_DBG_CFG/4] & 0x3f03u)==0x501u);
                assert((m->reg[b][LMP_TOP_CLK_MISC_CFG_0/4] & 0xff000000u)==0);
                uint32_t hz=actual_hz(m);
                if (m->skew_meter) hz=26000000u;
                m->reg[b][LMP_TOP_CLK26CALI_1/4]=(uint32_t)((uint64_t)hz*1024/26000000);
                m->reg[b][o/4]=0x1000u; m->meters++;
                m->meter_first_read=true;
                if (!m->freeze_time) m->now+=40000u;
            }
        } else { assert(o==LMP_TOP_CLK_DBG_CFG || o==LMP_TOP_CLK_MISC_CFG_0); if(!(o==LMP_TOP_CLK_DBG_CFG&&m->drop_meter_select))m->reg[b][o/4]=v; }
        assert((m->reg[b][LMP_TOP_CLK_CFG_1/4]&~3u)==m->top_other);
    } else {
        assert(b==LMP_BANK_APMIXED && o==LMP_APMIXED_MFGPLL_CON1);
        assert(m->parent==2u); /* A shadow mux echo cannot satisfy this. */
        if (!m->drop_pll) m->reg[b][o/4]=v;
        m->pll_written=m->now; m->pll_writes++; analog_safe(m);
    }
    return e;
}
static int enabled(void *p, enum lmp_rail r, bool *v) { struct model *m=p; if(fault(m))return LMP_EIO; assert(r<2); *v=true; return 0; }
static int voltage(void *p, enum lmp_rail r, uint32_t *v) { struct model *m=p; if(fault(m))return LMP_EIO; assert(r<2); *v=m->uv[r]; return 0; }
static int read_rails(void *p, struct lmp_gpu_rails *v) {
    struct model *m=p; if(fault(m))return LMP_EIO;
    *v=(struct lmp_gpu_rails){.uv={m->uv[0],m->uv[1]},.enabled={true,true}};
    if(m->drift_rails)v->uv[0]+=6250u;
    return 0;
}
static int set_voltage(void *p, enum lmp_rail r, uint32_t v) {
    struct model *m=p; int e=fault(m); if(e&&!m->fail_after)return e;
    assert(r<2 && m->locked && !m->busy && m->detached);
    assert((v>m->uv[r]?v-m->uv[r]:m->uv[r]-v)==6250u);
    m->uv[r]=v; m->rail_writes++; analog_safe(m);
    delay_us(m,240); /* Real provider additionally verifies DA under WACS2. */
    return e;
}
static int guard(void *p) {
    struct model *m=p; m->guards++; if(fault(m))return LMP_EIO;
    if(!m->locked || m->busy || !m->detached)return LMP_EBUSY;
    return m->foreign || m->unknown ? LMP_EIO : 0;
}
static void revoke(void *p, int e) { struct model *m=p; assert(e && m->locked); m->revoked++; }
static int limits(void *p, struct lmp_dvfs_limits *l) {
    struct model *m=p;
    /* Lifecycle is reported even when the real read fails. No missing sensor
     * value is populated, and zero VALID caps are explicit shutdown requests. */
    *l=(struct lmp_dvfs_limits){.started_mask=m->provider_started_mask};
    if(fault(m)){m->limit_fault=true;return LMP_EIO;}
    if(!m->caps || (m->cap_lost_after && m->rail_writes+m->mux_writes>=m->cap_lost_after))return LMP_ENODEV;
    if(m->reduce_cap_after && m->rail_writes+m->mux_writes>=m->reduce_cap_after)m->cap=484000000u;
    l->valid_mask=(m->caps_incomplete?(m->provider_mask&3u):m->provider_mask)&~m->provider_fault_mask;
    l->fault_mask=m->provider_fault_mask;
    uint32_t cap[3];
    for(unsigned i=0;i<3;i++)cap[i]=m->provider_cap[i]<m->cap?m->provider_cap[i]:m->cap;
    l->thermal_max_hz=l->valid_mask&1u?cap[0]:0;
    l->battery_max_hz=l->valid_mask&2u?cap[1]:0;
    l->power_max_hz=l->valid_mask&4u?cap[2]:0;
    l->sampled_ns=m->caps_future?m->now+1000000u:m->now;
    l->valid_until_ns=m->caps_stale?m->now:m->now+100000000u;
    if(m->aggregate_error)return m->aggregate_error;
    return m->provider_mask!=7u?m->absent_error:0;
}
static struct lmp_dvfs_ops dvfs_ops(struct model *m) { return (struct lmp_dvfs_ops){.cookie=m,.guard=guard,.revoke=revoke,.limits=limits}; }
static void setup(struct model *m, struct lmp_state *s, unsigned index) {
    memset(m,0,sizeof(*m)); struct lgd_opp o; uint32_t w;
    assert(lgd_get_opp(index,&o) && lgd_pll_word(o.khz,&w));
    m->uv[0]=o.vgpu_uv; m->uv[1]=o.vsram_uv; m->parent=1;
    m->locked=m->detached=m->caps=true; m->cap=950000000u;
    m->provider_mask=m->provider_started_mask=7u;
    for(unsigned i=0;i<3;i++)m->provider_cap[i]=950000000u;
    m->top_other=0x01010200u; m->dbg=0x23a7c2f0u; m->misc=0x9abc1234u;
    m->reg[LMP_BANK_TOP][LMP_TOP_CLK_CFG_1/4]=m->top_other|1u;
    m->reg[LMP_BANK_TOP][LMP_TOP_CLK_DBG_CFG/4]=m->dbg;
    m->reg[LMP_BANK_TOP][LMP_TOP_CLK_MISC_CFG_0/4]=m->misc;
    m->reg[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON0/4]=0x1e1;
    m->reg[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON1/4]=w & ~LGD_PLL_CHANGE;
    m->reg[LMP_BANK_APMIXED][LMP_APMIXED_MFGPLL_CON3/4]=1;
    m->reg[LMP_BANK_APMIXED][LMP_APMIXED_MAINPLL_CON0/4]=0x1e1;
    m->reg[LMP_BANK_APMIXED][(LMP_APMIXED_MAINPLL_CON0+4)/4]=(2u<<24)|(uint32_t)(UINT64_C(1092000000)*65536/26000000);
    m->reg[LMP_BANK_APMIXED][LMP_APMIXED_MAINPLL_CON3/4]=1;
    m->reg[LMP_BANK_SPM][LMP_SPM_PWR_STATUS/4]=LMP_MFG_DOMAIN_MASK;
    m->reg[LMP_BANK_SPM][LMP_SPM_PWR_STATUS_2ND/4]=LMP_MFG_DOMAIN_MASK;
    const uint32_t pwr[4]={LMP_SPM_MFG_PWR_CON,LMP_SPM_MFG_CORE0_PWR_CON,LMP_SPM_MFG_CORE1_PWR_CON,LMP_SPM_MFG_ASYNC_PWR_CON};
    for(unsigned i=0;i<4;i++)m->reg[LMP_BANK_SPM][pwr[i]/4]=LMP_PWR_RST_B|LMP_PWR_ON|LMP_PWR_ON_2ND;
    struct lmp_io_ops io={.cookie=m,.read32=rd,.write32=wr,.now_ns=now_ns,.delay_us=delay_us,
        .pmic={.cookie=m,.is_enabled=enabled,.get_voltage_uv=voltage,.set_voltage_uv=set_voltage,.read_rails=read_rails}};
    struct lmp_config cfg={.platform=LMP_PLATFORM_LANCELOT_MT6768,.mode=LMP_MODE_FULL,.timeout_ns=100000000u /* entire native transaction budget */};
    assert(lmp_init(s,&cfg,&io)==0 && lmp_start(s)==0);
    m->calls=m->reads=m->writes=0; analog_safe(m);
}
static void attach(struct model *m,struct lmp_state *s) { struct lmp_dvfs_ops o=dvfs_ops(m); assert(lmp_dvfs_attach(s,&o)==0); assert(!m->rail_writes&&!m->pll_writes&&!m->mux_writes); }
static void optional_mask(struct model *m,uint32_t ready) { m->provider_mask=m->provider_started_mask=ready; }
static void rights_revoked(struct model *m, struct lmp_state *s) {
    uint32_t v; unsigned calls=m->calls,writes=m->writes,rails=m->rail_writes;
    assert(s->failed && !s->gpu_ready && !s->teardown_safe && !s->dvfs.attached && !s->dvfs.limit_policy_valid && m->revoked==1);
    assert(lmp_gpu_read32(s,0,&v)==LMP_ENODEV);
    assert(lmp_gpu_write32(s,0x30,1)==LMP_ENODEV);
    assert(lmp_dvfs_target(s,0)==LMP_EBUSY && lmp_teardown(s)==LMP_EBUSY);
    assert(calls==m->calls && writes==m->writes && rails==m->rail_writes);
}
static void all_pairs(void) {
    struct model m; struct lmp_state s; struct lgd_opp o;
    for(unsigned a=0;a<LGD_OPP_COUNT;a++)for(unsigned b=0;b<LGD_OPP_COUNT;b++) {
        setup(&m,&s,a); attach(&m,&s); assert(lmp_dvfs_target(&s,b)==0);
        assert(lgd_get_opp(b,&o)); assert(m.uv[0]==o.vgpu_uv && m.uv[1]==o.vsram_uv);
        assert(s.dvfs.current_opp==b && s.source_hz<=o.khz*1000u && o.khz*1000u-s.source_hz<1000u);
        assert(m.parent==1 && !s.failed && s.gpu_ready && !m.revoked);
        assert(s.dvfs.meter_valid && s.dvfs.measured_hz && s.dvfs.meter_count && s.dvfs.meter_samples>=2 && s.dvfs.measured_ns);
        assert(m.reg[LMP_BANK_TOP][LMP_TOP_CLK_DBG_CFG/4]==m.dbg && m.reg[LMP_BANK_TOP][LMP_TOP_CLK_MISC_CFG_0/4]==m.misc);
        assert(m.reg[LMP_BANK_TOP][LMP_TOP_CLK26CALI_0/4]==0);
        assert(m.pll_writes==(a==b?0u:1u) && m.mux_writes==(a==b?0u:6u)); pairs++;
    }
    assert(pairs==900u);
    /* Real inherited boot pair, not canonical535MHz voltages. */
    setup(&m,&s,16);m.uv[0]=800000u;m.uv[1]=900000u;attach(&m,&s);
    assert(s.source_hz==534999909u && lmp_dvfs_target(&s,29)==0); negative_cases++;
}
static void all_fallback_pairs(void) {
    struct model m;struct lmp_state s;struct lgd_opp o;
    for(unsigned ready=0;ready<7;ready++) {
        for(unsigned from=0;from<30;from++)for(unsigned request=0;request<30;request++) {
            unsigned goal=request<4?4u:request;
            setup(&m,&s,from);optional_mask(&m,ready);attach(&m,&s);
            assert(lmp_dvfs_target(&s,request)==0 && lgd_get_opp(goal,&o));
            assert(s.dvfs.current_opp==goal && s.source_hz<=850000000u);
            assert(s.dvfs.limit_policy_valid && s.dvfs.effective_max_hz==850000000u);
            assert(s.dvfs.provider_valid_mask==ready && s.dvfs.provider_missing_mask==(7u^ready));
            assert(m.uv[0]==o.vgpu_uv && m.uv[1]==o.vsram_uv && m.parent==1);
            assert(m.pll_writes==(from==goal?0u:1u) && m.mux_writes==(from==goal?0u:6u));
            assert(s.gpu_ready && !s.failed && !m.revoked);analog_safe(&m);fallback_pairs++;
        }
        optional_combinations++;
    }
    assert(fallback_pairs==6300u && optional_combinations==7u);
}
static void faults(bool attaching,unsigned from,unsigned to,unsigned ready,unsigned *counter) {
    unsigned before=*counter;
    struct model m; struct lmp_state s; setup(&m,&s,from);
    optional_mask(&m,ready);
    if(!attaching)attach(&m,&s);
    m.calls=0;struct lmp_dvfs_ops ops=dvfs_ops(&m);
    assert((attaching?lmp_dvfs_attach(&s,&ops):lmp_dvfs_target(&s,to))==0);
    unsigned total=m.calls;assert(total>50);
    for(unsigned after=0;after<2;after++)for(unsigned n=1;n<=total;n++) {
        setup(&m,&s,from);optional_mask(&m,ready);if(!attaching)attach(&m,&s);
        m.calls=0;m.fail_at=n;m.fail_after=after;ops=dvfs_ops(&m);
        int e=attaching?lmp_dvfs_attach(&s,&ops):lmp_dvfs_target(&s,to);
        assert(m.hit==1);
        assert(e && s.failed);rights_revoked(&m,&s);
        if(m.limit_fault)provider_io_failstops++;
        (*counter)++;if(after)ambiguous_writes++;
    }
    assert(*counter==before+2u*total);
}
static void negative(void) {
    struct model m;struct lmp_state s;struct lmp_dvfs_ops o;
    for(unsigned mode=0;mode<3;mode++) {
        setup(&m,&s,16);o=dvfs_ops(&m);if(mode==0)m.locked=false;if(mode==1)m.busy=true;if(mode==2)m.detached=false;
        assert(lmp_dvfs_attach(&s,&o)==LMP_EBUSY && !m.writes && !s.failed);negative_cases++;
    }
    setup(&m,&s,16);o=dvfs_ops(&m);o.guard=NULL;assert(lmp_dvfs_attach(&s,&o)==LMP_ENOTSUP&&!m.writes);negative_cases++;
    setup(&m,&s,16);o=dvfs_ops(&m);o.revoke=NULL;assert(lmp_dvfs_attach(&s,&o)==LMP_ENOTSUP&&!m.writes);negative_cases++;
    setup(&m,&s,16);o=dvfs_ops(&m);s.ops.pmic.read_rails=NULL;assert(lmp_dvfs_attach(&s,&o)==LMP_ENOTSUP&&!m.writes);negative_cases++;
    setup(&m,&s,16);optional_mask(&m,0);o=dvfs_ops(&m);s.ops.pmic.set_voltage_uv=NULL;
    assert(lmp_dvfs_attach(&s,&o)==LMP_ENOTSUP&&!m.writes);negative_cases++;
    setup(&m,&s,16);optional_mask(&m,0);o=dvfs_ops(&m);s.config.mode=LMP_MODE_SPM_ONLY;
    assert(lmp_dvfs_attach(&s,&o)==LMP_ENOTSUP&&!m.writes);negative_cases++;
    for(unsigned mode=0;mode<4;mode++) {
        setup(&m,&s,16);attach(&m,&s);m.rail_writes=m.pll_writes=m.mux_writes=0;
        if(mode==0)m.caps=false;
        if(mode==1)m.caps_stale=true;
        if(mode==2)m.caps_incomplete=true;
        if(mode==3)s.dvfs.ops.limits=NULL;
        assert(lmp_dvfs_target(&s,0)!=0);rights_revoked(&m,&s);
        assert(!m.rail_writes&&!m.pll_writes&&!m.mux_writes);runtime_provider_cases++;
    }
    setup(&m,&s,16);optional_mask(&m,0);m.caps=false;attach(&m,&s);
    assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==4);optional_error_cases++;
    /* Normal startup absence also permits the rail increase needed to park
     * while moving between low OPPs; all physical guards still apply. */
    setup(&m,&s,29);optional_mask(&m,0);m.caps=false;attach(&m,&s);
    assert(lmp_dvfs_target(&s,28)==0 && !s.failed);optional_error_cases++;
    const unsigned hard_modes[]={0,1,2,3,4,5,6,7,9,10,11,12};
    for(unsigned optional=0;optional<2;optional++)for(unsigned at=0;at<sizeof(hard_modes)/sizeof(hard_modes[0]);at++) {
        unsigned mode=hard_modes[at];
        setup(&m,&s,mode==2?0:29);if(optional)optional_mask(&m,0);attach(&m,&s);
        if(mode==0)m.foreign=true;
        if(mode==1)m.unknown=true;
        if(mode==2)m.drop_park=true;
        if(mode==3)m.drop_return=true;
        if(mode==4)m.drop_pll=true;
        if(mode==5)m.skew_meter=true;
        if(mode==6)m.drift_rails=true;
        if(mode==7)m.drift_main=true;
        if(mode==9)m.reg[LMP_BANK_TOP][LMP_TOP_CLK_CFG_1/4]|=LMP_TOP_MFG_GATE;
        if(mode==10)m.drop_meter_start=true;
        if(mode==11)m.drop_meter_select=true;
        if(mode==12)m.bad_pll_delay=true;
        assert(lmp_dvfs_target(&s,mode==2?29:0)!=0);rights_revoked(&m,&s);
        if(mode==2)assert(!m.pll_writes);
        negative_cases++;
    }
    setup(&m,&s,29);attach(&m,&s);m.cap_lost_after=3;
    assert(lmp_dvfs_target(&s,0)!=0);rights_revoked(&m,&s);runtime_provider_cases++;
    for(unsigned freeze=0;freeze<2;freeze++) {
        setup(&m,&s,16);o=dvfs_ops(&m);m.stuck_meter=true;m.freeze_time=freeze;
        assert(lmp_dvfs_attach(&s,&o)==LMP_ETIMEDOUT);rights_revoked(&m,&s);negative_cases++;
    }
    setup(&m,&s,16);attach(&m,&s);unsigned c=m.calls;
    assert(lmp_dvfs_target(&s,30)==LMP_EINVAL && m.calls==c);negative_cases++;
    /* Readback may have coincident frequencies; don't pretend the meter then
     * identifies the parked parent. Both original and target are tested. */
    for(unsigned mode=0;mode<2;mode++) {
        setup(&m,&s,mode?16:25);uint32_t hz=(mode?950000000u:366000000u)*3u;
        m.reg[LMP_BANK_APMIXED][(LMP_APMIXED_MAINPLL_CON0+4)/4]=(uint32_t)((uint64_t)hz*16384/26000000);
        attach(&m,&s);assert(lmp_dvfs_target(&s,0)==LMP_ENOTSUP&&!s.failed);negative_cases++;
    }
    setup(&m,&s,16);attach(&m,&s);m.cap=484000000u;
    assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==18 && s.source_hz<=484000000u);negative_cases++;
    setup(&m,&s,0);attach(&m,&s);m.cap=299000000u;
    assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==29);negative_cases++;
    setup(&m,&s,16);attach(&m,&s);m.cap=298000000u;
    assert(lmp_dvfs_target(&s,16)==LMP_ENOTSUP);rights_revoked(&m,&s);negative_cases++;
}
static void optional_policy(void) {
    struct model m;struct lmp_state s;
    const int absence[3]={0,LMP_ENODEV,LMP_ENOTSUP};
    for(unsigned ready=0;ready<7;ready++)for(unsigned e=0;e<3;e++) {
        setup(&m,&s,29);optional_mask(&m,ready);m.absent_error=absence[e];attach(&m,&s);
        assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==4 && s.source_hz<=850000000u);
        assert(s.dvfs.provider_error==0 && s.dvfs.provider_fault_mask==0);
        assert(s.dvfs.provider_valid_mask==ready && s.dvfs.provider_missing_mask==(7u^ready));
        assert(lmp_dvfs_target(&s,29)==0 && s.source_hz<=299000000u && s.gpu_ready);optional_error_cases++;
    }
    setup(&m,&s,29);optional_mask(&m,0);struct lmp_dvfs_ops ops=dvfs_ops(&m);ops.limits=NULL;
    assert(lmp_dvfs_attach(&s,&ops)==0 && lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==4);
    assert(lmp_dvfs_target(&s,29)==0 && s.gpu_ready);optional_error_cases++;
    for(unsigned ready=0;ready<8;ready++) {
        struct lgd_governor g={0};uint32_t target;
        struct lgd_load_window w={.start_ns=1,.end_ns=10000001,.busy_ns=9000000,.jobs=1,.valid=true};
        setup(&m,&s,29);optional_mask(&m,ready);attach(&m,&s);
        for(unsigned n=0;n<16;n++) {
            assert(lgd_choose(&g,s.dvfs.current_opp,&w,&target) && lmp_dvfs_target(&s,target)==0);
            assert(s.source_hz<=(ready==7?950000000u:850000000u));
            w.start_ns=w.end_ns;w.end_ns+=10000000u;
        }
        assert(s.dvfs.current_opp==(ready==7?0u:4u));
        w.busy_ns=0;w.jobs=0;
        assert(lgd_choose(&g,s.dvfs.current_opp,&w,&target) && target==29 && lmp_dvfs_target(&s,target)==0);
        assert(s.source_hz<=299000000u && s.gpu_ready && !s.failed);load_policy_cases++;
    }
    /* Every valid lower provider wins independently, even when the other
     * providers never started. No dummy value is filled for missing peers. */
    const uint32_t lower[3]={485000000u,400000000u,349000000u};
    const unsigned goal[3]={18,23,26};
    for(unsigned ready=1;ready<8;ready++)for(unsigned p=0;p<3;p++)if(ready&(1u<<p)) {
        setup(&m,&s,16);optional_mask(&m,ready);m.absent_error=ready==7?0:LMP_ENODEV;
        m.provider_cap[p]=lower[p];attach(&m,&s);
        assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==goal[p] && s.source_hz<=lower[p]);
        assert(s.dvfs.effective_max_hz==lower[p] && !m.revoked);live_lower_cap_cases++;
    }
    assert(live_lower_cap_cases==12u);
    /* Providers may start later: full valid readiness lifts850 to950, but
     * subsequent disappearance of a started provider is a real fault. */
    setup(&m,&s,29);optional_mask(&m,0);attach(&m,&s);
    assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==4);
    optional_mask(&m,7);assert(lmp_dvfs_target(&s,0)==0 && s.dvfs.current_opp==0);
    assert(s.dvfs.provider_started_mask==7 && s.dvfs.effective_max_hz==950000000u);load_policy_cases++;
    optional_mask(&m,6);assert(lmp_dvfs_target(&s,29)!=0);rights_revoked(&m,&s);runtime_provider_cases++;
    /* A live cap below the parking source is still a real clock restriction:
     * startup absence of OTHER providers must not bypass it. */
    setup(&m,&s,29);optional_mask(&m,1);m.provider_cap[0]=349000000u;attach(&m,&s);
    assert(lmp_dvfs_target(&s,28)==LMP_ENOTSUP && s.dvfs.current_opp==29 && !s.failed);
    assert(!m.rail_writes && !m.pll_writes && !m.mux_writes);negative_cases++;
}
static void provider_runtime_faults(void) {
    struct model m;struct lmp_state s;
    const int errors[]={LMP_ENODEV,LMP_ENOTSUP,LMP_EIO,LMP_ETIMEDOUT};
    for(unsigned p=0;p<3;p++) {
        for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);e++) {
            setup(&m,&s,29);optional_mask(&m,1u<<p);attach(&m,&s);
            m.aggregate_error=errors[e];m.provider_fault_mask=1u<<p;
            assert(lmp_dvfs_target(&s,0)==errors[e]);rights_revoked(&m,&s);runtime_provider_cases++;
        }
        for(unsigned how=0;how<6;how++) {
            setup(&m,&s,29);optional_mask(&m,1u<<p);attach(&m,&s);
            if(how==0)optional_mask(&m,0); /* Cannot erase sticky start history. */
            if(how==1)m.provider_fault_mask=1u<<p;
            if(how==2)m.caps_stale=true;
            if(how==3)m.caps_future=true;
            if(how==4)m.provider_cap[p]=0; /* Valid overheat/low-battery/shutdown. */
            if(how==5)m.caps=false;
            assert(lmp_dvfs_target(&s,0)!=0);rights_revoked(&m,&s);runtime_provider_cases++;
        }
        /* Even the first active read can fail: lifecycle is explicitly
         * reported before I/O, so ENODEV is not misclassified as absence. */
        setup(&m,&s,29);optional_mask(&m,1u<<p);m.provider_mask=0;m.absent_error=LMP_ENODEV;
        struct lmp_dvfs_ops ops=dvfs_ops(&m);
        assert(lmp_dvfs_attach(&s,&ops)==LMP_ENODEV);rights_revoked(&m,&s);runtime_provider_cases++;
    }
    /* A newly stronger valid real restriction during a transition has priority
     * over the former demand. Do not continue into a higher PLL/job. */
    setup(&m,&s,29);attach(&m,&s);m.reduce_cap_after=3;
    assert(lmp_dvfs_target(&s,0)==LMP_ENOTSUP);rights_revoked(&m,&s);
    assert(!m.pll_writes && s.dvfs.effective_max_hz==484000000u);runtime_provider_cases++;
    setup(&m,&s,29);optional_mask(&m,0);attach(&m,&s);m.provider_mask=8;
    assert(lmp_dvfs_target(&s,0)==LMP_EINVAL);rights_revoked(&m,&s);runtime_provider_cases++;
}
static void governor(void) {
    struct lgd_governor g={0};struct lgd_load_window w={.start_ns=1,.end_ns=10000001,.busy_ns=9000000,.jobs=1,.valid=true};uint32_t target;
    assert(lgd_choose(&g,16,&w,&target)&&target==14);governor_cases++;
    for(unsigned i=0;i<3;i++) { w.start_ns=w.end_ns;w.end_ns+=10000000;w.busy_ns=1000000;assert(lgd_choose(&g,16,&w,&target)&&target==(i==2?17u:16u));governor_cases++; }
    w.start_ns=w.end_ns;w.end_ns+=10000000;w.busy_ns=0;w.jobs=0;assert(lgd_choose(&g,0,&w,&target)&&target==29);governor_cases++;
    struct lgd_governor saved=g;w.start_ns--;assert(!lgd_choose(&g,0,&w,&target)&&!memcmp(&g,&saved,sizeof(g)));governor_cases++;
    w.start_ns=g.last_end_ns;w.busy_ns=1;assert(!lgd_choose(&g,0,&w,&target));governor_cases++;
    w.busy_ns=0;w.valid=false;assert(!lgd_choose(&g,0,&w,&target));governor_cases++;
}
int inherited_dvfs_model_main(void) {
    all_pairs();all_fallback_pairs();negative();governor();optional_policy();provider_runtime_faults();
    faults(true,16,0,7,&attach_faults);faults(false,29,0,7,&up_faults);faults(false,0,29,7,&down_faults);
    faults(true,16,0,0,&missing_provider_io_faults);faults(false,29,4,0,&missing_provider_io_faults);faults(false,4,29,0,&missing_provider_io_faults);
    printf("{\"opp_pairs\":%u,\"opp_pairs_expected\":900,\"fallback_opp_pairs\":%u,\"fallback_opp_pairs_expected\":6300,\"opp_pairs_skipped\":0,\"missing_provider_combinations\":%u,\"normal_absence_cases\":%u,\"load_up_idle_down_cases\":%u,\"live_lower_cap_cases\":%u,\"runtime_provider_protection_cases\":%u,\"attach_io_faults\":%u,\"up_io_faults\":%u,\"down_io_faults\":%u,\"missing_provider_io_faults\":%u,\"provider_io_failstops\":%u,\"after_operation_faults\":%u,\"negative_cases\":%u,\"governor_cases\":%u,\"hardware_verified\":false}\n",pairs,fallback_pairs,optional_combinations,optional_error_cases,load_policy_cases,live_lower_cap_cases,runtime_provider_cases,attach_faults,up_faults,down_faults,missing_provider_io_faults,provider_io_failstops,ambiguous_writes,negative_cases,governor_cases);
    return 0;
}
