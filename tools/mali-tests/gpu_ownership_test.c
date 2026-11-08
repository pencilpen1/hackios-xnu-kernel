/* Actual production core + eligibility policy, RAM-only ownership transition model. */
#define main core_fixture_entry
#include "core_test.c"
#undef main
#include "lancelot_mali_client_logic.h"
struct ownership_fake {
    struct fake gpu;
    struct lm_vm *vm;
    struct lm_page data[3];
    unsigned clean_seen[3],cpu_seen[3],cpu_event[3];
    bool fail_alloc;
};
static void *ownership_alloc(void *cookie,size_t bytes) {
    return ((struct ownership_fake*)cookie)->fail_alloc?NULL:ma(cookie,bytes);
}
static void ownership_sync(void *cookie,void *p,size_t bytes,bool cpu) {
    struct ownership_fake *f=cookie;
    assert(bytes&&!(bytes%4096)&&bytes<=64*4096);
    for(unsigned at=0;at<bytes/4096;at++)for(unsigned i=0;i<3;i++)if((char*)p+at*4096==f->data[i].cpu) {
        if(cpu) {assert(!f->gpu.run);f->cpu_seen[i]++;f->cpu_event[i]=++f->gpu.event;}
        else {f->clean_seen[i]++;f->gpu.last_clean=++f->gpu.event;}
    }
    f->gpu.time+=bytes/4096*1000;barrier(cookie);
}
static void ownership_clean(void *o,void *p,size_t n) {ownership_sync(o,p,n,false);}
static void ownership_cpu(void *o,void *p,size_t n) {ownership_sync(o,p,n,true);}
static struct lm_core *ownership_device(struct ownership_fake *f,bool enabled) {
    memset(f,0,sizeof(*f));struct lm_core *first=bare_device(&f->gpu);assert(!lm_core_destroy(first));
    struct lm_ops ops={.cookie=f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,.alloc=ownership_alloc,.free=mf,
        .page_alloc=pa,.page_free=pf,.for_device=ownership_clean,.for_cpu=ownership_cpu,.barrier=barrier,
        .io_error=ioerr,.pre_job_flush=true,.gpu_shareable=true,.cache_batch=true,
        .profile_jobs=true,.submit_scope=true,.gpu_only_sync=enabled};
    struct lm_core *d=lm_core_create(&ops);assert(d&&!lm_core_start(d));
    f->vm=lm_core_vm_new(d);assert(f->vm);
    for(unsigned i=0;i<3;i++) {
        assert(!pa(&f->gpu,f->data+i));
        assert(!lm_core_vm_map(f->vm,LM_VA_MIN+i*4096,f->data+i,1,LM_MAP_WRITE));
    }
    return d;
}
static void ownership_destroy(struct ownership_fake *f,struct lm_core *d) {
    assert(!lm_core_vm_destroy(f->vm));
    for(unsigned i=0;i<3;i++)pf(&f->gpu,f->data+i);
    destroy(&f->gpu,d);
}
static void policy(void) {
    for(unsigned wc=0;wc<2;wc++)for(unsigned imported=0;imported<2;imported++)
        for(unsigned exported=0;exported<2;exported++)for(unsigned job=0;job<2;job++)
            assert(lmal_client_gpu_only_eligible(wc,imported,exported,job)==(wc&&!imported&&!exported&&!job));
    puts("GPU ownership policy: only never-export-requested internal WC non-JC BO eligible; imports/WB/prior maps/job descriptors excluded PASS");
}
static void transitions(void) {
    for(unsigned enabled=0;enabled<2;enabled++) {
        struct ownership_fake f;struct lm_core *d=ownership_device(&f,enabled!=0);
        struct lm_range ranges[]={{.va=LM_VA_MIN,.pages=1},
            {.va=LM_VA_MIN+4096,.pages=1,.flags=LM_RANGE_GPU_ONLY}, {.va=LM_VA_MIN+8192,.pages=1}};
        lm_v7_dma_prepare(f.data[0].cpu,f.data[1].cpu,LM_VA_MIN+4096,0x10203040);
        f.gpu.emulate_value=true;
        for(unsigned step=0;step<3;step++) {
            ((uint32_t*)f.data[0].cpu)[12]=0x10203040+step;
            struct lm_job_result r;assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,ranges,3));
            assert(f.clean_seen[0]==step+1&&f.cpu_seen[0]==step+1);
            assert(f.clean_seen[2]==step+1&&f.cpu_seen[2]==step+1);
            assert(f.clean_seen[1]==(enabled?1u:step+1));
            assert(f.cpu_seen[1]==(enabled?0u:step+1));
            assert(r.profile.clean_pages==(enabled&&step?2u:3u)&&r.profile.cpu_pages==(enabled?2u:3u));
            assert(f.gpu.cache_commands==2*(step+1)); /* Never skip GPU pre/postflush. */
        }
        unsigned clean=f.clean_seen[1],invalid=f.cpu_seen[1];
        assert(!lm_core_vm_cpu_access(f.vm,LM_VA_MIN+4096,1));
        assert(f.clean_seen[1]==clean&&f.cpu_seen[1]==invalid+(enabled?1u:0u));
        /* This checks physical RAM after the modeled ownership handoff; the
         * fixture is not a CPU-cache emulator or hardware performance test. */
        assert(((uint32_t*)f.data[1].cpu)[32]==0x10203042);
        ranges[1].flags=0; /* Client's export descriptor now permanently exists. */
        struct lm_job_result r;assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,ranges,3));
        assert(f.clean_seen[1]==clean+1&&f.cpu_seen[1]==invalid+(enabled?2u:1u));
        assert(r.profile.clean_pages==3&&r.profile.cpu_pages==3);
        ownership_destroy(&f,d);
    }
    puts("GPU ownership: feature off/on, initial clean, repeated GPU DMA without CPU callbacks, ordinary BO maintenance, explicit invalidate-only CPU handoff, permanently visible BO full sync, GPU flushes retained PASS");
}
static void initial_cpu_and_invalid_flags(void) {
    struct ownership_fake f;struct lm_core *d=ownership_device(&f,true);
    assert(!lm_core_vm_cpu_access(f.vm,LM_VA_MIN+4096,1));
    assert(f.clean_seen[1]==1&&f.cpu_seen[1]==1); /* Initial CPU bytes clean before invalidation. */
    struct lm_range range={.va=LM_VA_MIN,.pages=1,.flags=LM_RANGE_GPU_ONLY};struct lm_job_result r;
    assert(lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,&range,1)==LM_EINVAL);
    range.flags=2;assert(lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,&range,1)==LM_EINVAL);
    assert(!f.gpu.launches&&!r.sequence);
    assert(lm_core_vm_cpu_access(f.vm,LM_VA_MIN+4096,2)==LM_EINVAL);
    ownership_destroy(&f,d);
    puts("GPU ownership: first CPU map synchronizes initial bytes, GPU-only JC/unknown flags and partial CPU handoff reject before launch PASS");
}
static void alias_fallback(void) {
    struct ownership_fake f;struct lm_core *d=ownership_device(&f,true);
    assert(!lm_core_vm_unmap(f.vm,LM_VA_MIN+8192,1));
    struct lm_page alias=f.data[2];alias.pa=f.data[1].pa;
    assert(!lm_core_vm_map(f.vm,LM_VA_MIN+8192,&alias,1,LM_MAP_WRITE));
    struct lm_range ranges[]={{.va=LM_VA_MIN,.pages=1},{.va=LM_VA_MIN+4096,.pages=1,.flags=LM_RANGE_GPU_ONLY}};
    for(unsigned step=0;step<2;step++) {
        struct lm_job_result r;assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,ranges,2));
        assert(r.profile.scope_mode==2&&r.profile.clean_pages==3&&r.profile.cpu_pages==3);
        assert(f.clean_seen[1]==step+1&&f.cpu_seen[1]==step+1&&f.clean_seen[2]==step+1&&f.cpu_seen[2]==step+1);
    }
    ownership_destroy(&f,d);
    puts("GPU ownership: physical aliases force full-VM CPU clean/invalidate even for GPU-only flags PASS");
}
static void oom_and_duplicates(void) {
    for(unsigned test=0;test<2;test++) {
        struct ownership_fake f;struct lm_core *d=ownership_device(&f,true);
        struct lm_range ranges[]={{.va=LM_VA_MIN,.pages=1},
            {.va=LM_VA_MIN+4096,.pages=1,.flags=LM_RANGE_GPU_ONLY}, {.va=LM_VA_MIN+4096,.pages=1}};
        if(test==0)f.fail_alloc=true;
        struct lm_job_result r;
        assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,ranges,test==0?2:3));
        assert(r.profile.scope_mode==(test==0?3u:1u));
        assert(f.clean_seen[1]==1&&f.cpu_seen[1]==1); /* OOM/full VM or CPU-visible duplicate. */
        f.fail_alloc=false;ownership_destroy(&f,d);
    }
    puts("GPU ownership: hash OOM/full-VM and CPU-visible duplicate never exempt CPU maintenance PASS");
}
static void fault_policy(bool quarantine) {
    struct ownership_fake f;struct lm_core *d=ownership_device(&f,true);
    struct lm_range ranges[]={{.va=LM_VA_MIN,.pages=1},{.va=LM_VA_MIN+4096,.pages=1,.flags=LM_RANGE_GPU_ONLY}};
    f.gpu.fault=2;if(quarantine)f.gpu.reset_fail_after_submit=true;
    struct lm_job_result r;assert(lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,ranges,2)==LM_EIO);
    assert(!f.cpu_seen[1]&&!lm_core_ready(d));
    assert(lm_core_vm_cpu_access(f.vm,LM_VA_MIN+4096,1)==LM_ENODEV);
    if(quarantine) {
        assert(r.quarantined&&!r.reset_completed&&!f.cpu_seen[0]);
        assert(lm_core_vm_destroy(f.vm)==LM_EBUSY&&lm_core_destroy(d)==LM_EBUSY);
    } else {assert(r.reset_completed&&!r.quarantined&&f.cpu_seen[0]==1);ownership_destroy(&f,d);}
    puts("GPU ownership: fault goes offline, failed reset keeps backing and denies CPU handoff; no skipped fault-safety proof PASS");
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"quarantine"))fault_policy(true);
    else {policy();transitions();initial_cpu_and_invalid_flags();alias_fallback();oom_and_duplicates();fault_policy(false);}
    return 0;
}
