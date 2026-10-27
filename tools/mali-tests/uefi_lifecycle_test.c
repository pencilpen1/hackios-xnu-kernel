/* Full platform -> adapter/core -> recovery -> arena release -> handoff.
 * All registers, time and memory are RAM models, never hardware proof. */
#define main core_fixture_entry
#include "core_test.c"
#undef main
#define main power_fixture_entry
#include "uefi_power_id_test.c"
#undef main
#include "lancelot_gpu_dma.h"
struct integration {
    struct model power;
    struct fake gpu;
    struct lfd_context dma;
    void *arena;
    unsigned test,releases,release_attempts,launches;
    bool injected,fail_release;
};
static int combined_read(void*cookie,uint64_t address,uint32_t*out) {
    struct integration*m=cookie;
    if(address<lpg_base[3]||address>=lpg_base[3]+0x4000)return read_hw(&m->power,address,out);
    uint32_t off=(uint32_t)(address-lpg_base[3]);*out=rr(&m->gpu,off);
    bool inject=(m->test==1&&off==0x304&&m->gpu.writes>=13)||
        ((m->test==2||m->test==5||m->test==6||m->test==10)&&m->gpu.submitted&&off==0x1000)||
        (m->test==3&&m->dma.phase==LM_PHASE_OFF&&off==0x20)||
        ((m->test==4||m->test==7)&&off==0x304&&m->gpu.writes>=13)||
        (m->test==8&&m->dma.phase==LM_PHASE_ID)||
        (m->test==9&&m->dma.phase==LM_PHASE_RESET&&off==0x20)||
        (m->test==12&&m->dma.phase==LM_PHASE_FINAL_RESET&&off==0x140);
    if(inject&&!m->injected) {
        m->injected=true;
        if(m->test==4)return LM_EIO;
        m->power.ns=UINT64_C(2000000000);
        if(m->test==7)return LM_ETIMEDOUT; /* transport timeout, not software budget */
    }
    if(m->test==10&&m->dma.recovering&&off==0x34)return LM_EIO;
    if(m->test==6&&m->dma.recovering)m->power.ns=UINT64_C(3000000000);
    return 0;
}
static int combined_write(void*cookie,uint64_t address,uint32_t value) {
    struct integration*m=cookie;
    if(address<lpg_base[3]||address>=lpg_base[3]+0x4000)return write_hw(&m->power,address,value);
    uint32_t off=(uint32_t)(address-lpg_base[3]);
    if(off==0x18e0&&value==1){assert(!m->dma.recovering);m->launches++;}
    wr(&m->gpu,off,value);
    if(m->test==11&&m->dma.phase==LM_PHASE_FINAL_RESET&&off==0x30&&value==1)
        m->gpu.reg[0x140/4]=3;
    return 0;
}
static uint64_t combined_now(void*cookie){return ((struct integration*)cookie)->power.ns;}
static void combined_delay(void*cookie,uint32_t us){((struct integration*)cookie)->power.ns+=(uint64_t)us*1000;}
static int combined_allocate(void*cookie,void**cpu,uint64_t*pa,unsigned pages) {
    struct integration*m=cookie;assert(pages==LFD_PAGES);
    assert(!posix_memalign(&m->arena,4096,pages*4096));*cpu=m->arena;*pa=0x42000000;
    for(unsigned i=0;i<LFD_PAGES;i++)m->gpu.owned[i]=(struct lm_page){(char*)m->arena+i*4096,*pa+i*4096};
    return 0;
}
static int combined_release(void*cookie,void*cpu,uint64_t pa,unsigned pages) {
    struct integration*m=cookie;assert(cpu==m->arena&&pa==0x42000000&&pages==LFD_PAGES);
    assert(!m->gpu.run);m->release_attempts++;if(m->fail_release)return LM_EIO;
    m->releases++;free(cpu);m->arena=NULL;return 0;
}
static int combined_check(void*cookie,void*cpu,uint64_t pa){(void)cookie;assert(cpu&&pa);return 0;}
static int combined_cache(void*cookie,void*cpu,size_t n,bool after){(void)cookie;(void)after;assert(cpu&&n==4096);return 0;}
static void combined_barrier(void*cookie){(void)cookie;}
static void one(unsigned test) {
    struct integration m={.test=test};efi_mtk_gpu_map map;struct lpg_context c;
    init(&m.power,&map,&c);
    struct lm_core*unused=bare_device(&m.gpu);assert(!lm_core_destroy(unused));
    for(unsigned i=0;i<1024;i++)m.gpu.reg[i]=m.power.reg[3][i];
    m.gpu.emulate_value=true;m.gpu.zero_thread_priority=true;
    if(test==5||test==14)m.gpu.reset_fail_after_submit=true;
    c.cookie=&m;c.read=combined_read;c.write=combined_write;c.now_ns=combined_now;c.delay_us=combined_delay;
    c.features_requested=c.dma_requested=true;c.dma_cookie=&m.dma;c.dma_run=lfd_run;
    struct lfd_memory_ops mem={.cookie=&m,.allocate=combined_allocate,.release=combined_release,
        .check_page=combined_check,.cache=combined_cache,.barrier=combined_barrier};
    assert(!lfd_prepare(&m.dma,&c,&mem));
    if(test>=13&&test<=16)m.dma.transaction.mmu_fault_test=true;
    if(test==15)m.fail_release=true;
    if(test==16)((uint32_t*)(m.arena+LFD_GUARD_PAGE*4096u))[0]^=1;
    struct lpg_result r=lpg_run(&c);
    lpg_dma_finish_handoff(&r,!m.dma.allocated&&!m.dma.release_error&&!m.dma.io_error);
    if(test==13) {
        assert(r.error==LM_EIO&&r.domains_restored&&lpg_dma_readonly_resume_safe(&r));
        assert(m.dma.transaction.expected_fault&&m.dma.transaction.guards_valid&&m.releases==1&&m.release_attempts==1&&m.launches==1);
        assert(m.dma.transaction.job_result.fault_address==LM_PRIVATE_OUTPUT_VA+4096+128);
    } else if(test==14) {
        assert(r.error==LM_EIO&&r.dma_halt_required&&!lpg_dma_readonly_resume_safe(&r));
        assert(m.dma.allocated&&!m.releases&&!m.release_attempts&&m.launches==1);
        assert(!m.dma.transaction.cleaned&&!m.dma.transaction.expected_fault);
        assert(m.dma.transaction.job_result.quarantined&&!m.dma.transaction.job_result.reset_completed);
        assert(m.dma.transaction.job_result.fault_address==LM_PRIVATE_OUTPUT_VA+4096+128);
    } else if(test==15||test==16) {
        assert(r.error==LM_EIO&&r.domains_restored&&r.dma_safe_to_teardown&&r.dma_halt_required);
        assert(!lpg_dma_readonly_resume_safe(&r)&&m.dma.allocated&&!m.releases&&m.launches==1);
        assert(m.dma.transaction.cleaned&&!m.dma.transaction.expected_fault);
        assert(m.release_attempts==(test==15));
        assert(m.dma.release_error==(test==15?LM_EIO:0));
    } else if(test==0) {
        assert(!r.error&&r.domains_restored&&lpg_dma_handoff_safe(&r));
        assert(m.launches==1&&m.dma.transaction.guards_valid&&!m.dma.first_io_error);
    } else if(test<=3||test==8||test==9||test==12) {
        assert(r.error&&r.domains_restored&&lpg_dma_readonly_resume_safe(&r)&&!lpg_dma_handoff_safe(&r));
        assert(m.dma.first_io_error==LM_ETIMEDOUT&&m.dma.recovering&&m.dma.recovery_attempted);
        assert(!m.dma.io_error&&!m.dma.allocated&&m.releases==1&&!m.gpu.run);
        assert(m.launches==((test==1||test==8||test==9)?0:1));
        if(test==1)assert(!m.dma.peak_pages&&m.dma.first_io_phase==LM_PHASE_CONFIG&&m.dma.first_io_reg==0x304);
        if(test==2)assert(m.dma.transaction.job_result.reset_completed);
        if(test==3)assert(m.dma.transaction.guards_valid);
        assert(m.power.ns<UINT64_C(3000000000));
    } else {
        assert(r.error&&!r.dma_safe_to_teardown&&r.dma_halt_required&&!lpg_dma_readonly_resume_safe(&r));
        assert(m.dma.allocated&&!m.releases&&!m.dma.transaction.cleaned);
        assert((r.teardown_state&31u)==31u);
        if(test==11)assert(!m.dma.io_error&&m.dma.transaction.cleanup_error==LM_EIO&&!m.gpu.run);
        else if(test==4||test==7)assert(m.dma.io_error==(test==4?LM_EIO:LM_ETIMEDOUT)&&!m.dma.recovering&&c.transport_error);
        else assert(m.dma.transaction.job_result.quarantined);
        /* Intentional quarantine survives process exit; leak check disabled
         * only for this explicitly selected child, as in other core tests. */
    }
    printf("Full lifecycle case=%u result=%d first-io=%d phase=%u safe=%u halt=%u releases=%u launches=%u elapsed=%llu\n",
        test,r.error,m.dma.first_io_error,m.dma.first_io_phase,r.dma_safe_to_teardown,
        r.dma_halt_required,m.releases,m.launches,(unsigned long long)m.power.ns);
}
int main(int argc,char**argv) {
    (void)lfd_flush_trace;
    if(argc>1&&!strcmp(argv[1],"quarantine")){one(4);one(5);one(6);one(7);one(10);one(11);one(14);one(15);one(16);return 0;}
    for(unsigned test=0;test<4;test++)one(test);
    one(8);one(9);one(12);one(13);
    puts("Actual full GPU lifecycle: initial/config/job/final-cleanup deadline recovery is stop-only, failed test retained, proved release resumes XNU read-only; RAM ONLY");
    return 0;
}
