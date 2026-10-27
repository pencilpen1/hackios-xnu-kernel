/* Actual firmware adapter and shared core. Every hardware operation is RAM ONLY. */
#define main core_fixture_entry
#include "core_test.c"
#undef main
#include "lancelot_gpu_dma.h"
struct firmware {
    struct fake gpu;
    void *arena;
    unsigned checks,releases,cache_calls,fail_check,fail_cache,trace_calls;
    bool bad_pa,fail_allocate,fail_release,bus_blocked;
};
static int fw_allocate(void *cookie,void **cpu,uint64_t *pa_out,unsigned pages) {
    struct firmware*m=cookie;assert(pages==LFD_PAGES);
    if(m->fail_allocate)return LM_ENOMEM;
    assert(!posix_memalign(&m->arena,4096,LFD_PAGES*4096u));
    *cpu=m->arena;*pa_out=m->bad_pa?0x70000000u:0x42000000u;
    for(unsigned i=0;i<LFD_PAGES;i++)m->gpu.owned[i]=(struct lm_page){(char*)m->arena+i*4096u,*pa_out+i*4096u};
    return 0;
}
static int fw_release(void *cookie,void *cpu,uint64_t pa_value,unsigned pages) {
    struct firmware*m=cookie;assert(cpu==m->arena&&pages==LFD_PAGES&&pa_value);
    m->releases++;if(m->fail_release)return LM_EIO;
    free(m->arena);m->arena=NULL;
    for(unsigned i=0;i<LFD_PAGES;i++)m->gpu.owned[i]=(struct lm_page){0};
    return 0;
}
static int fw_check(void *cookie,void *cpu,uint64_t pa_value) {
    struct firmware*m=cookie;m->checks++;assert(cpu&&pa_value);
    return m->checks==m->fail_check?LM_EFAULT:0;
}
static int fw_cache(void *cookie,void *cpu,size_t n,bool after) {
    struct firmware*m=cookie;assert(cpu&&n==4096);(void)after;m->cache_calls++;
    return m->cache_calls==m->fail_cache?LM_EIO:0;
}
static void fw_barrier(void *cookie){(void)cookie;}
static void fw_trace(void *cookie,bool read,uint32_t off,uint32_t value,int error) {
    struct firmware*m=cookie;(void)read;(void)off;(void)value;(void)error;
    m->trace_calls++;m->gpu.time+=UINT64_C(100000000); /* slow screen transport */
}
static int fw_read(void *cookie,uint64_t address,uint32_t *out) {
    struct firmware*m=cookie;
    if(address>=lpg_base[3]&&address<lpg_base[3]+0x4000) {
        *out=rr(&m->gpu,(uint32_t)(address-lpg_base[3]));return m->gpu.ioerr?LM_EIO:0;
    }
    if(address==lpg_base[0]+LMP_SPM_PWR_STATUS||address==lpg_base[0]+LMP_SPM_PWR_STATUS_2ND)
        *out=m->bus_blocked?0:LMP_MFG_DOMAIN_MASK;
    else {
        assert(address==lpg_base[2]+LMP_MFG_CG_CON || address==lpg_base[1]+LMP_INFRA_PROTECTEN ||
            address==lpg_base[1]+LMP_INFRA_PROTECTEN_STA1);*out=0;
    }
    return 0;
}
static int fw_write(void *cookie,uint64_t address,uint32_t value) {
    struct firmware*m=cookie;assert(address>=lpg_base[3]&&address<lpg_base[3]+0x4000);
    wr(&m->gpu,(uint32_t)(address-lpg_base[3]),value);return m->gpu.ioerr?LM_EIO:0;
}
static uint64_t fw_now(void *cookie){return now(&((struct firmware*)cookie)->gpu);}
static void fw_delay(void *cookie,uint32_t us){delay(&((struct firmware*)cookie)->gpu,us);}
static void fw_init(struct firmware*m,struct lpg_context*c,struct lfd_memory_ops*mem) {
    memset(m,0,sizeof(*m));
    struct lm_core*unused=bare_device(&m->gpu);assert(!lm_core_destroy(unused));
    m->gpu.reg[0]=0x74021000;m->gpu.reg[0x100/4]=3;m->gpu.emulate_value=true;m->gpu.zero_thread_priority=true;
    *c=(struct lpg_context){.cookie=m,.read=fw_read,.write=fw_write,.now_ns=fw_now,.delay_us=fw_delay};
    *mem=(struct lfd_memory_ops){.cookie=m,.allocate=fw_allocate,.release=fw_release,
        .check_page=fw_check,.cache=fw_cache,.barrier=fw_barrier,.trace=fw_trace};
}
static struct lpg_features features(void) {
    struct lpg_features f={0};f.word[0]=0x74021000;f.word[5]=0x2823;
    f.word[6]=255;f.word[7]=7;f.word[18]=0x4fe;f.word[33]=3;f.word[37]=1;return f;
}
int main(int argc,char **argv) {
    (void)lpg_run;
    assert(lpm_requested("lancelot_pmic_probe=0")==0);
    struct firmware m;struct lpg_context c;struct lfd_memory_ops mem;struct lfd_context f;
    struct lpg_features props=features();struct lmp_state state={.gpu_ready=true};bool safe=false;
    if(argc>1&&!strcmp(argv[1],"quarantine")) {
        for(unsigned fault=0;fault<3;fault++) {
            fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
            if(fault==0){m.gpu.fault=3;m.gpu.reset_fail_after_submit=true;}
            else if(fault==1)m.gpu.fail_write=0x18e0;
            else m.fail_cache=1;
            assert(lfd_run(&f,&state,&props,&safe)!=0);
            assert(!safe&&f.allocated&&!m.releases&&f.page_mask&&f.metadata_active);
        }
        puts("Firmware DMA reset/posted-launch/cache uncertainty retains actual private arena; RAM ONLY");return 0;
    }
    fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
    assert(!lfd_run(&f,&state,&props,&safe)&&safe&&!f.allocated&&m.releases==1);
    assert(f.peak_pages==6&&f.cleans==6&&f.invalidates==2&&f.transaction.guards_valid);
    assert(f.transaction.observed_value==LM_PRIVATE_VALUE&&!f.page_mask&&!f.metadata_active);
    assert(!m.trace_calls&&!lpg_expired(&c)&&f.trace_count);
    lfd_flush_trace(&f);assert(m.trace_calls==f.trace_count&&lpg_expired(&c));
    /* Overflow must stay bounded and retain the final timeout, without output. */
    unsigned calls=m.trace_calls;
    for(unsigned i=0;i<2*LFD_TRACE_MAX;i++)lfd_record_trace(&f,true,0x20,i,LM_ETIMEDOUT);
    assert(f.trace_count==LFD_TRACE_MAX&&f.trace_dropped&&m.trace_calls==calls);
    assert(f.trace[LFD_TRACE_MAX-1].value==2*LFD_TRACE_MAX-1&&f.trace[LFD_TRACE_MAX-1].error==LM_ETIMEDOUT);
    /* Fragment clear through the real firmware adapter: pixels, not WRITE_VALUE. */
    fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
    f.clear_mode=true;m.gpu.emulate_clear=true;
    assert(!lfd_run(&f,&state,&props,&safe)&&safe&&!f.allocated&&m.releases==1);
    assert(f.transaction.clear_test&&f.transaction.guards_valid);
    assert(f.transaction.observed_value==LM_CLEAR_VALUE&&f.peak_pages==9);
    assert(f.transaction.descriptor_status==0x00010013&&!f.page_mask&&!f.metadata_active);
    fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
    f.clear_mode=true;m.gpu.emulate_clear=true;m.gpu.corrupt_pixel=true;
    assert(lfd_run(&f,&state,&props,&safe)==LM_EIO&&safe&&!f.allocated&&m.releases==1);
    assert(!f.transaction.guards_valid&&f.transaction.bad_offset==4u&&
        f.transaction.clear_actual==(LM_CLEAR_VALUE^1u));
    const uint32_t bind_regs[]={0x2400,0x2404,0x2408,0x240c,0x2430,0x2434};
    for(unsigned i=0;i<sizeof(bind_regs)/sizeof(bind_regs[0]);i++) {
        fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
        m.gpu.corrupt_bind_reg=bind_regs[i];
        assert(lfd_run(&f,&state,&props,&safe)==LM_EIO&&safe&&!f.allocated&&m.releases==1);
        struct lm_job_result*r=&f.transaction.job_result;
        assert(!f.transaction.submitted&&!r->sequence&&r->reset_completed&&!r->quarantined);
        assert(r->failed_stage==LM_JOB_BIND&&r->mismatch_reg==(bind_regs[i]&~7u));
        assert((r->expected^r->observed)==(UINT64_C(1)<<((bind_regs[i]&4u)?32:0)));
        struct lpg_result handoff={.error=LM_EIO,.teardown_error=LM_EBUSY,
            .dma_attempted=true,.dma_safe_to_teardown=safe};
        lpg_dma_finish_handoff(&handoff,!f.allocated&&!f.release_error&&!f.io_error);
        assert(lpg_dma_readonly_resume_safe(&handoff)&&!lpg_dma_handoff_safe(&handoff));
    }
    fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
    m.fail_release=true;assert(lfd_run(&f,&state,&props,&safe)==LM_EIO&&safe&&f.allocated);
    struct lpg_result handoff={.dma_attempted=true,.dma_safe_to_teardown=safe};
    lpg_dma_finish_handoff(&handoff,!f.allocated&&!f.release_error&&!f.io_error);
    assert(handoff.dma_halt_required&&!lpg_dma_readonly_resume_safe(&handoff));
    m.fail_release=false;assert(!fw_release(&m,m.arena,f.pa,LFD_PAGES));
    for(unsigned test=0;test<10;test++) {
        fw_init(&m,&c,&mem);f=(struct lfd_context){0};
        if(test<8)m.fail_check=test+1;else if(test==8)m.bad_pa=true;else m.fail_allocate=true;
        assert(lfd_prepare(&f,&c,&mem)!=0&&!f.allocated&&!m.gpu.writes);
    }
    fw_init(&m,&c,&mem);f=(struct lfd_context){0};assert(!lfd_prepare(&f,&c,&mem));
    assert(!lfd_write_allowed(&f,0x50,0xb16u));assert(!lfd_write_allowed(&f,0x2440,0));
    assert(!lfd_write_allowed(&f,0x2400,0x70000000));assert(!lfd_write_allowed(&f,0x18c0,0x70000000));
    assert(LM_JOB_NEXT_CONFIG==0x3300&&!lfd_write_allowed(&f,0x18d8,0x83300));
    m.bus_blocked=true;assert(lfd_read(&f,0x14)==0&&f.io_error&&!m.gpu.reads);
    assert(!fw_release(&m,m.arena,f.pa,LFD_PAGES));
    puts("Actual firmware DMA/clear adapter: private walk/offset128, real fragment clear writes 64x64 RGBA8 pixels, lease/WB/cache/domain/PA/AS/write whitelist guards PASS; RAM ONLY");
    return 0;
}
