/* RAM-only cache coverage/timing model, never hardware performance evidence. */
#define main core_fixture_entry
#include "core_test.c"
#undef main

struct batch_fake {
    struct fake gpu; /* Existing fixture callbacks take this first member. */
    struct lm_page *data;
    unsigned data_pages;
    unsigned clean_ranges,invalidate_ranges,clean_pages,invalidate_pages;
    unsigned clean_seen[160],invalidate_seen[160];
    uint64_t expected_clean_ns,expected_invalidate_ns;
    bool pending;
};
static void batch_barrier(void *cookie) {
    struct batch_fake *f=cookie;
    f->pending=false;f->gpu.time+=50;barrier(cookie);
}
static void batch_sync(void *cookie,void *base,size_t bytes,bool cpu) {
    struct batch_fake *f=cookie;
    assert(base&&bytes&&!(bytes%LM_PAGE_SIZE)&&bytes<=64*LM_PAGE_SIZE);
    unsigned pages=(unsigned)(bytes/LM_PAGE_SIZE);
    for(unsigned at=0;at<pages;at++) {
        void *p=(char*)base+at*LM_PAGE_SIZE;
        bool found=false;
        for(unsigned i=0;i<f->data_pages;i++)if(f->data[i].cpu==p) {
            if(cpu)f->invalidate_seen[i]++;
            else f->clean_seen[i]++;
            found=true;break;
        }
        if(!found) {
            /* Page tables are individually owned, never output-invalidated. */
            assert(!cpu&&pages==1);
            for(unsigned i=0;i<2048;i++)if(f->gpu.owned[i].cpu==p)found=true;
            assert(found);
        }
    }
    uint64_t cost=1000+pages*100;
    f->gpu.time+=cost;f->pending=true;
    if(cpu) {
        f->invalidate_ranges++;f->invalidate_pages+=pages;
        f->expected_invalidate_ns+=cost+50;
    } else {
        f->clean_ranges++;f->clean_pages+=pages;
        f->expected_clean_ns+=cost+50;
        f->gpu.last_clean=++f->gpu.event;
    }
    batch_barrier(cookie); /* Same synchronous range contract as XNU. */
}
static void batch_clean(void *cookie,void *p,size_t bytes) {batch_sync(cookie,p,bytes,false);}
static void batch_invalidate(void *cookie,void *p,size_t bytes) {batch_sync(cookie,p,bytes,true);}
static void batch_write(void *cookie,uint32_t reg,uint32_t value) {
    struct batch_fake *f=cookie;
    assert(!f->pending); /* No root/launch/recovery before completion. */
    f->gpu.time+=100;
    if((reg==0x1860||reg==0x18e0)&&value==1)f->gpu.time+=5000;
    wr(cookie,reg,value);
}
static struct lm_core *batch_device(struct batch_fake *f,bool batch,bool profile) {
    memset(f,0,sizeof(*f));
    struct lm_core *initial=bare_device_mapping_policy(&f->gpu,true,false,0,true);
    assert(!lm_core_destroy(initial));
    struct lm_ops ops={.cookie=f,.read32=rr,.write32=batch_write,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=batch_clean,
        .for_cpu=batch_invalidate,.barrier=batch_barrier,.io_error=ioerr,
        .pre_job_flush=true,.gpu_shareable=true,.cache_batch=batch,.profile_jobs=profile};
    struct lm_core *d=lm_core_create(&ops);assert(d&&!lm_core_start(d));return d;
}
static uint64_t profile_sum(const struct lm_job_result *r) {
    return r->profile.for_device_ns+r->profile.bind_ns+r->profile.preflush_ns+
           r->profile.run_ns+r->profile.postflush_ns+r->profile.detach_ns+r->profile.for_cpu_ns;
}
static void reset_counters(struct batch_fake *f) {
    f->clean_ranges=f->invalidate_ranges=f->clean_pages=f->invalidate_pages=0;
    f->expected_clean_ns=f->expected_invalidate_ns=0;
    memset(f->clean_seen,0,sizeof(f->clean_seen));
    memset(f->invalidate_seen,0,sizeof(f->invalidate_seen));
}
static void coverage(void) {
    for(unsigned batch=0;batch<2;batch++)for(unsigned profile=0;profile<2;profile++) {
        struct batch_fake f;struct lm_core *d=batch_device(&f,batch!=0,profile!=0);
        struct lm_vm *v=lm_core_vm_new(d);assert(v);
        void *arena=NULL;assert(!posix_memalign(&arena,4096,80*4096));memset(arena,0,80*4096);
        struct lm_page p[75];
        for(unsigned i=0;i<75;i++) {
            /* RW 70-page run crosses the 64-page cap; then two pages, a
             * CPU-VA hole and two adjacent pages in DIFFERENT mappings. */
            unsigned index=i<72?i:i+1;
            p[i]=(struct lm_page){(char*)arena+index*4096,UINT64_C(0x500000000)+i*0x3000};
        }
        f.data=p;f.data_pages=75;
        assert(!lm_core_vm_map(v,LM_VA_MIN,p,70,LM_MAP_WRITE));
        assert(!lm_core_vm_map(v,LM_VA_MIN+70*4096,p+70,3,LM_MAP_WRITE));
        assert(!lm_core_vm_map(v,LM_VA_MIN+73*4096,p+73,1,LM_MAP_WRITE));
        assert(!lm_core_vm_map(v,LM_VA_MIN+74*4096,p+74,1,0)); /* Read-only. */
        for(unsigned step=0;step<3;step++) {
            reset_counters(&f);struct lm_job_result r;
            assert(!lm_core_submit_wait(v,LM_VA_MIN,step&1,UINT64_C(1000000000),&r));
            assert(r.sequence==step+1&&!f.pending);
            for(unsigned i=0;i<75;i++) {
                assert(f.clean_seen[i]==1);assert(f.invalidate_seen[i]==(i<74?1u:0u));
            }
            unsigned tables=f.gpu.pages;
            assert(f.clean_pages==75+tables&&f.invalidate_pages==74);
            assert(f.clean_ranges==(batch?6:75)+tables&&f.invalidate_ranges==(batch?5:74));
            if(profile) {
                assert(r.profile.enabled&&r.profile.mapped_pages==75&&r.profile.table_pages==tables);
                assert(r.profile.cpu_pages==74&&r.profile.device_ranges==f.clean_ranges&&r.profile.cpu_ranges==f.invalidate_ranges);
                assert(r.profile.for_device_ns==f.expected_clean_ns+50);
                assert(r.profile.for_cpu_ns==f.expected_invalidate_ns+50);
                assert(r.profile.run_ns>=5000&&r.profile.bind_ns&&r.profile.preflush_ns&&r.profile.postflush_ns&&r.profile.detach_ns);
                assert(profile_sum(&r)<=r.elapsed_ns);
            } else {
                assert(!r.profile.enabled&&!r.profile.for_device_ns&&!r.profile.for_cpu_ns&&!r.profile.device_ranges);
            }
        }
        /* Never emit a stale profile/sequence for a subsequently invalid call. */
        struct lm_job_result invalid;memset(&invalid,0xff,sizeof(invalid));
        assert(lm_core_submit_wait(v,LM_VA_MIN+1,0,1000,&invalid)==LM_EINVAL);
        assert(!invalid.sequence&&!invalid.profile.enabled&&!invalid.elapsed_ns);
        assert(!lm_core_vm_destroy(v));free(arena);destroy(&f.gpu,d);
    }
    puts("cache batch: legacy/batch x profile off/on, exact page coverage, sparse PA, CPU-VA holes, mapping boundaries, RO output exclusion, 64-page cap, repeated submissions and timing accounting PASS");
}
static void recovery(void) {
    for(unsigned batch=0;batch<2;batch++)for(unsigned fault=0;fault<5;fault++) {
        struct batch_fake f;struct lm_core *d=batch_device(&f,batch!=0,true);
        struct lm_vm *v=lm_core_vm_new(d);assert(v);
        struct lm_page p[2];assert(!pa(&f,p)&&!pa(&f,p+1));f.data=p;f.data_pages=2;
        assert(!lm_core_vm_map(v,LM_VA_MIN,p,2,LM_MAP_WRITE));
        reset_counters(&f);
        if(fault<2)f.gpu.cache_timeout_at=1;
        if(fault==1)f.gpu.reset_fail=1;
        if(fault==2)f.gpu.fault=2; /* Existing model's job fault branch. */
        if(fault==3)f.gpu.cache_timeout_at=2;
        if(fault==4)f.gpu.cache_ioerr_at=1;
        struct lm_job_result r;int e=lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r);
        assert(e<0&&!lm_core_ready(d));
        if(fault==1||fault==4) {
            assert(r.quarantined&&!r.reset_completed&&!f.invalidate_ranges&&!r.profile.for_cpu_ns);
            assert(lm_core_vm_destroy(v)==LM_EBUSY);
            assert(lm_core_destroy(d)==LM_EBUSY);
            /* Deliberate retention; runner disables leak checks for this mode
             * exactly as for the existing quarantine fixtures. */
        } else {
            assert(r.reset_completed&&!r.quarantined&&f.invalidate_pages==2&&!f.pending);
            assert(!lm_core_vm_destroy(v));pf(&f,p);pf(&f,p+1);destroy(&f.gpu,d);
        }
    }
    puts("cache batch: preflush/job faults preserve reset-before-invalidate and quarantine skips CPU access PASS");
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"recovery"))recovery();
    else coverage();
    return 0;
}
