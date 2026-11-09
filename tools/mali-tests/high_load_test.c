#include "dvfs_model.c"
#include "../../xnu/files/bsd/dev/lancelot_mali_trace.h"
static unsigned rated_pairs, rated_lower_caps, rated_faults;

static struct lmp_dvfs_ops rated_ops(struct model *m)
{
    struct lmp_dvfs_ops ops=dvfs_ops(m);
    ops.control_policy=LMP_DVFS_NATIVE_AP;
    ops.missing_provider_max_hz=950000000u;
    return ops;
}
static void rated_transitions(void)
{
    for(unsigned ready=0;ready<8;ready++)
        for(unsigned from=0;from<30;from++)
            for(unsigned to=0;to<30;to++) {
                struct model m;struct lmp_state s;struct lgd_opp target;
                setup(&m,&s,from);optional_mask(&m,ready);
                m.absent_error=ready==7?0:LMP_ENODEV;
                struct lmp_dvfs_ops ops=rated_ops(&m);
                assert(!lmp_dvfs_attach(&s,&ops));
                assert(!lmp_dvfs_target(&s,to));
                assert(lgd_get_opp(to,&target));
                assert(s.dvfs.current_opp==to&&s.dvfs.effective_max_hz==950000000u);
                assert(s.source_hz<=target.khz*1000u&&target.khz*1000u-s.source_hz<1000u);
                assert(m.uv[0]==target.vgpu_uv&&m.uv[1]==target.vsram_uv);
                assert(s.dvfs.provider_valid_mask==ready&&s.dvfs.provider_missing_mask==(7u^ready));
                assert(s.dvfs.meter_valid&&m.parent==1&&!m.revoked);
                analog_safe(&m);rated_pairs++;
            }
    assert(rated_pairs==7200);
}
static void rated_protection(void)
{
    const uint32_t lower[]={450000000u,484000000u,690000000u};
    for(unsigned ready=1;ready<8;ready++)
        for(unsigned provider=0;provider<3;provider++)
            if(ready&(1u<<provider)) {
                struct model m;struct lmp_state s;
                setup(&m,&s,29);optional_mask(&m,ready);
                m.provider_cap[provider]=lower[provider];
                struct lmp_dvfs_ops ops=rated_ops(&m);
                assert(!lmp_dvfs_attach(&s,&ops)&&!lmp_dvfs_target(&s,0));
                assert(s.source_hz<=lower[provider]&&s.dvfs.effective_max_hz==lower[provider]);
                analog_safe(&m);rated_lower_caps++;
            }
    for(unsigned kind=0;kind<8;kind++) {
        struct model m;struct lmp_state s;
        setup(&m,&s,29);optional_mask(&m,1);
        struct lmp_dvfs_ops ops=rated_ops(&m);
        assert(!lmp_dvfs_attach(&s,&ops));
        unsigned pll=m.pll_writes,rails=m.rail_writes,mux=m.mux_writes;
        if(kind==0)optional_mask(&m,0);
        if(kind==1)m.provider_fault_mask=1;
        if(kind==2)m.caps_stale=true;
        if(kind==3)m.caps_future=true;
        if(kind==4)m.provider_cap[0]=0;
        if(kind==5)m.caps=false;
        if(kind==6)m.foreign=true;
        if(kind==7)m.unknown=true;
        assert(lmp_dvfs_target(&s,0)!=0);
        assert(m.pll_writes==pll&&m.rail_writes==rails&&m.mux_writes==mux);
        rights_revoked(&m,&s);rated_faults++;
    }
    struct model m;struct lmp_state s;
    setup(&m,&s,29);struct lmp_dvfs_ops ops=rated_ops(&m);
    ops.missing_provider_max_hz=951000000u;
    assert(lmp_dvfs_attach(&s,&ops)==LMP_EINVAL&&!m.writes);
}
static void trace_policy(void)
{
    struct lm_trace_gate gate={0};unsigned emitted=0;
    for(unsigned i=0;i<2000000;i++) {
        uint64_t end=(uint64_t)i*1000+100000000u;
        emitted+=lm_trace_job(&gate,2,end-50000000u,end-20000000u,end,false);
    }
    assert(emitted<=6);
    assert(!lm_trace_job(&gate,0,1,2,3,true));
    assert(lm_trace_job(&gate,2,1,2,3,true));
    assert(lm_trace_job(NULL,1,1,2,3,false));
    assert(!lm_trace_job(&gate,2,4,3,5,false));
    gate=(struct lm_trace_gate){.startup_details=4,.last_detail_ns=UINT64_MAX};
    assert(lm_trace_job(&gate,2,0,20000000,30000000,false));
}
int main(void)
{
    assert(!inherited_dvfs_model_main());
    rated_transitions();rated_protection();trace_policy();
    printf("PASS rated950: %u real-controller modeled OPP transitions, %u lower-cap cases, %u protected faults; bounded mode2 timing trace\n",
           rated_pairs,rated_lower_caps,rated_faults);
    return 0;
}
