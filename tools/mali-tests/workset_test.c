/* Same production core, fake MMIO/owned RAM only. No hardware performance claim. */
#define main core_fixture_entry
#include "core_test.c"
#undef main
struct workset_fake {
    struct fake gpu;
    struct lm_vm *vm;
    struct lm_page data[8];
    unsigned count,clean_seen[8],cpu_seen[8],hidden_checks;
    uint64_t hidden_va;
    bool expect_hidden,fail_hash;
    uint64_t parent_va,mixed_va;
    unsigned parent_level;
};
static unsigned missing_level(struct fake *f,uint64_t va) {
    uint64_t root=(uint64_t)f->reg[0x2400/4]|(uint64_t)f->reg[0x2404/4]<<32;
    for(unsigned level=0;level<4;level++) {
        uint64_t *table=physical(f,root);assert(table);
        uint64_t e=table[(va>>(39-9*level))&511];
        if((e&3)!=3)return level;
        root=e&UINT64_C(0x000000fffffff000);
    }
    return 4;
}
static void *work_alloc(void *cookie,size_t bytes) {
    struct workset_fake *f=cookie;
    if(f->fail_hash)return NULL;
    return ma(cookie,bytes);
}
static void work_sync(void *cookie,void *p,size_t bytes,bool cpu) {
    struct workset_fake *f=cookie;
    assert(bytes&&!(bytes%4096)&&bytes<=64*4096);
    for(unsigned at=0;at<bytes/4096;at++) {
        void *page=(char*)p+at*4096;
        for(unsigned i=0;i<f->count;i++)if(f->data[i].cpu==page) {
            if(cpu)f->cpu_seen[i]++;
            else f->clean_seen[i]++;
        }
    }
    f->gpu.time+=bytes/4096*1000;
    barrier(cookie);
}
static void work_clean(void *o,void *p,size_t n) {work_sync(o,p,n,false);}
static void work_cpu(void *o,void *p,size_t n) {work_sync(o,p,n,true);}
static void work_write(void *cookie,uint32_t reg,uint32_t value) {
    struct workset_fake *f=cookie;
    f->gpu.time+=100;
    if((reg==0x1860||reg==0x18e0)&&value==1&&f->hidden_va) {
        assert((gpu_address(&f->gpu,f->hidden_va)==NULL)==f->expect_hidden);
        assert((lm_core_vm_lookup(f->vm,f->hidden_va,NULL)==0)==f->expect_hidden);
        f->hidden_checks++;
    }
    if((reg==0x1860||reg==0x18e0)&&value==1&&f->parent_va) {
        assert(missing_level(&f->gpu,f->parent_va)==f->parent_level);
        assert(missing_level(&f->gpu,f->mixed_va)==3);
    }
    wr(cookie,reg,value);
}
static struct lm_core *work_device(struct workset_fake *f,bool scoped) {
    memset(f,0,sizeof(*f));
    struct lm_core *initial=bare_device(&f->gpu);assert(!lm_core_destroy(initial));
    struct lm_ops ops={.cookie=f,.read32=rr,.write32=work_write,.now_ns=now,.delay_us=delay,
        .alloc=work_alloc,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=work_clean,
        .for_cpu=work_cpu,.barrier=barrier,.io_error=ioerr,.pre_job_flush=true,
        .gpu_shareable=true,.cache_batch=true,.profile_jobs=true,.submit_scope=scoped};
    struct lm_core *d=lm_core_create(&ops);assert(d&&!lm_core_start(d));
    f->vm=lm_core_vm_new(d);assert(f->vm);return d;
}
static void add_pages(struct workset_fake *f,unsigned n) {
    for(unsigned i=0;i<n;i++)assert(!pa(&f->gpu,&f->data[f->count++]));
}
static void cleanup(struct workset_fake *f,struct lm_core *d) {
    assert(!lm_core_vm_destroy(f->vm));
    for(unsigned i=0;i<f->count;i++)pf(&f->gpu,&f->data[i]);
    destroy(&f->gpu,d);
}
static void reset_counts(struct workset_fake *f) {
    memset(f->clean_seen,0,sizeof(f->clean_seen));memset(f->cpu_seen,0,sizeof(f->cpu_seen));
}
static void selected(void) {
    for(unsigned scope=0;scope<2;scope++) {
        struct workset_fake f;struct lm_core *d=work_device(&f,scope!=0);add_pages(&f,4);
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,1,LM_MAP_WRITE));
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN+4096,f.data+1,1,LM_MAP_WRITE));
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN+8192,f.data+2,1,0));
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN+12288,f.data+3,1,LM_MAP_EXEC));
        struct lm_range ranges[3]={{.va=LM_VA_MIN,.pages=1},{.va=LM_VA_MIN+8192,.pages=1},{.va=LM_VA_MIN,.pages=1}};
        f.hidden_va=LM_VA_MIN+4096;f.expect_hidden=scope!=0;
        for(unsigned step=0;step<3;step++) {
            reset_counts(&f);struct lm_job_result r;
            assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,step&1,1000000,&r,ranges,3));
            assert(r.sequence==step+1&&r.profile.scope_mode==scope);
            assert(r.profile.vm_pages==4&&r.profile.mapped_pages==(scope?2u:4u));
            assert(r.profile.cpu_pages==(scope?1u:2u));
            for(unsigned i=0;i<4;i++) {
                assert(f.clean_seen[i]==(!scope||i==0||i==2?1u:0u));
                assert(f.cpu_seen[i]==(i==0||(!scope&&i==1)?1u:0u));
                uint32_t flags=0;assert(lm_core_vm_lookup(f.vm,LM_VA_MIN+i*4096,&flags)==f.data[i].pa);
                assert(flags==(i<2?LM_MAP_WRITE:i==3?LM_MAP_EXEC:0));
            }
        }
        assert(f.hidden_checks==3);cleanup(&f,d);
    }
    puts("workset: off/on, exact BO deduplication, selected-only clean/RW invalidate, inactive PTE invisibility at launch, restored permissions, repeated submissions PASS");
}
static void invalid_ranges(void) {
    struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,2);
    assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,2,LM_MAP_WRITE));
    struct lm_range valid={.va=LM_VA_MIN,.pages=2};
    for(unsigned test=0;test<7;test++) {
        struct lm_range range=valid;uint64_t jc=LM_VA_MIN;uint32_t count=1;
        if(test==0)range.pages=1;
        if(test==1)range.va+=4096;
        if(test==2)range.pages=0;
        if(test==3)count=0;
        if(test==4)jc+=1;
        if(test==5)count=LM_MAX_SUBMIT_RANGES+1;
        if(test==6)jc+=8192;
        struct lm_job_result r;memset(&r,0xff,sizeof(r));reset_counts(&f);
        assert(lm_core_submit_ranges(f.vm,jc,0,1000000,&r,&range,count)==LM_EINVAL);
        assert(!r.sequence&&!f.clean_seen[0]&&!f.clean_seen[1]&&!f.gpu.launches);
        assert(lm_core_vm_lookup(f.vm,LM_VA_MIN+4096,NULL)==f.data[1].pa);
    }
    cleanup(&f,d);
    puts("workset: partial/unknown/zero/oversized lists and absent/unaligned JC reject before publication, result cleared, PTEs unchanged PASS");
}
static void aliases_and_oom(void) {
    for(unsigned test=0;test<3;test++) {
        struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,2);
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,1,LM_MAP_WRITE));
        struct lm_page alias=f.data[1];if(test==0)alias.pa=f.data[0].pa;
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN+4096,&alias,1,LM_MAP_WRITE));
        struct lm_range range={.va=LM_VA_MIN,.pages=1};f.hidden_va=LM_VA_MIN+4096;f.expect_hidden=test==2;
        if(test==1)f.fail_hash=true;
        struct lm_job_result r;
        assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,&range,1));
        assert(r.profile.scope_mode==(test==0?2u:test==1?3u:1u));
        assert(r.profile.mapped_pages==(test==2?1u:2u));
        assert(f.clean_seen[0]==1&&f.cpu_seen[0]==1);
        assert(f.clean_seen[1]==(test==2?0u:1u)&&f.cpu_seen[1]==(test==2?0u:1u));
        f.fail_hash=false;
        if(test==2) {
            /* A new alias invalidates the cached no-alias determination. */
            assert(!lm_core_vm_unmap(f.vm,LM_VA_MIN+4096,1));alias.pa=f.data[0].pa;
            assert(!lm_core_vm_map(f.vm,LM_VA_MIN+4096,&alias,1,LM_MAP_WRITE));
            reset_counts(&f);f.expect_hidden=false;
            assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,&range,1));
            assert(r.profile.scope_mode==2&&f.clean_seen[1]==1);
        }
        cleanup(&f,d);
    }
    puts("workset: physical aliases (different CPU VAs) and hash OOM retain full-VM visibility/maintenance; map/unmap invalidate alias cache PASS");
}
static void leaf_boundaries(void) {
    struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,1);
    struct lm_page *many=calloc(1025,sizeof(*many));assert(many);
    for(unsigned i=0;i<1025;i++)assert(!pa(&f.gpu,&many[i]));
    uint64_t other=LM_VA_MIN+0x1ff000; /* Start at the LAST entry of a leaf. */
    assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,1,LM_MAP_WRITE));
    assert(!lm_core_vm_map(f.vm,other,many,1025,LM_MAP_WRITE|LM_MAP_EXEC));
    struct lm_range range={.va=LM_VA_MIN,.pages=1};
    unsigned offsets[]={0,1,511,512,513,1024};
    for(unsigned step=0;step<sizeof(offsets)/sizeof(offsets[0]);step++) {
        f.hidden_va=other+(uint64_t)offsets[step]*4096;f.expect_hidden=true;
        struct lm_job_result r;assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,&range,1));
        assert(r.profile.vm_pages==1026&&r.profile.mapped_pages==1&&r.profile.cpu_pages==1);
        for(unsigned i=0;i<1025;i++) {
            uint32_t flags=0;assert(lm_core_vm_lookup(f.vm,other+(uint64_t)i*4096,&flags)==many[i].pa);
            assert(flags==(LM_MAP_WRITE|LM_MAP_EXEC));
        }
    }
    assert(!lm_core_vm_destroy(f.vm));
    for(unsigned i=0;i<1025;i++)pf(&f.gpu,&many[i]);
    free(many);pf(&f.gpu,f.data);destroy(&f.gpu,d);
    puts("workset: 1025-page inactive BO crossing leaf tables at final/first entries, sparse PA, all RW/EXEC PTEs restored, guards/teardown PASS");
}
static void faults(bool quarantine) {
    for(unsigned test=0;test<3;test++) {
        struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,2);
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,1,LM_MAP_WRITE));
        uint64_t other=LM_VA_MIN+(test==2?0x200000:4096);
        assert(!lm_core_vm_map(f.vm,other,f.data+1,1,LM_MAP_WRITE));
        struct lm_range range={.va=LM_VA_MIN,.pages=1};f.hidden_va=other;f.expect_hidden=true;
        if(test==0)f.gpu.cache_timeout_at=1;
        if(test==1)f.gpu.cache_timeout_at=2;
        if(test==2) {
            lm_v7_dma_prepare(f.data[0].cpu,f.data[1].cpu,other,0x12345678);
            f.gpu.emulate_value=true; /* Missing BO reference must fault, not DMA. */
        }
        if(quarantine)f.gpu.reset_fail_after_submit=true;
        if(quarantine&&test==0)f.gpu.reset_fail=1;
        struct lm_job_result r;int e=lm_core_submit_ranges(f.vm,LM_VA_MIN,0,1000000,&r,&range,1);
        assert(e<0&&!lm_core_ready(d)&&!f.cpu_seen[1]);
        if(test==2) {
            assert(r.fault_vm.valid&&r.fault_vm.scoped&&r.fault_vm.mapped);
            assert(!r.fault_vm.selected&&!r.fault_vm.visible);
            assert(r.fault_vm.va==other&&r.fault_vm.pages==1&&r.fault_vm.page_pa==f.data[1].pa);
            /* Whole-leaf parent masking preserves the leaf data, but the real
             * walk is blocked. The snapshot must not use restored visibility. */
            assert((r.fault_vm.leaf_pte&3)==3&&r.fault_vm.leaf_pte==r.fault_vm.expected_pte);
        }
        if(quarantine) {
            assert(r.quarantined&&!r.reset_completed&&!f.cpu_seen[0]);
            assert(!lm_core_vm_lookup(f.vm,other,NULL));
            assert(lm_core_vm_destroy(f.vm)==LM_EBUSY&&lm_core_destroy(d)==LM_EBUSY);
        } else {
            assert(r.reset_completed&&!r.quarantined&&f.cpu_seen[0]==1);
            assert(lm_core_vm_lookup(f.vm,other,NULL)==f.data[1].pa);
            cleanup(&f,d);
        }
    }
    puts("workset: pre/post flush failure and missing-reference MMU fault preserve reset-before-restore; failed reset retains masked tables/backing and performs no CPU invalidate PASS");
}
static void hierarchical_masks(void) {
    struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,3);
    struct lm_page *many=calloc(1025,sizeof(*many));assert(many);
    for(unsigned i=0;i<1025;i++)assert(!pa(&f.gpu,many+i));
    uint64_t other=UINT64_C(0x3ffff000); /* Leaf AND 1GiB-parent boundary. */
    assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,1,LM_MAP_WRITE));
    assert(!lm_core_vm_map(f.vm,LM_VA_MIN+4096,f.data+1,1,LM_MAP_WRITE));
    assert(!lm_core_vm_map(f.vm,other,many,1025,LM_MAP_WRITE|LM_MAP_EXEC));
    f.mixed_va=LM_VA_MIN+4096;f.parent_va=other+4096;
    for(unsigned step=0;step<6;step++) {
        struct lm_range ranges[]={{.va=LM_VA_MIN,.pages=1},{.va=other,.pages=1025},
                                  {.va=other-4096,.pages=1}};
        /* A selected neighbor turns the first otherwise hidden leaf mixed. */
        bool neighbor=step>=2&&step<4;
        if(step==2)assert(!lm_core_vm_map(f.vm,other-4096,f.data+2,1,0));
        if(step==4)assert(!lm_core_vm_unmap(f.vm,other-4096,1));
        unsigned count=(step&1)?2:1;
        if(neighbor) {ranges[count++]=(struct lm_range){.va=other-4096,.pages=1};}
        f.parent_level=(step&1)?4:2;
        struct lm_job_result r;
        assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,step&1,10000000,&r,ranges,count));
        assert(r.profile.masked_leaf_tables==((step&1)?0u:neighbor?2u:3u));
        assert(r.profile.masked_leaf_entries==(!(step&1)&&neighbor?2u:1u));
        for(unsigned i=0;i<1025;i++) {
            uint32_t flags=0;assert(lm_core_vm_lookup(f.vm,other+(uint64_t)i*4096,&flags)==many[i].pa);
            assert(flags==(LM_MAP_WRITE|LM_MAP_EXEC));
        }
    }
    /* Fully pruned and recreated leaves must replace the cached metadata. */
    assert(!lm_core_vm_unmap(f.vm,other,1025));
    assert(!lm_core_vm_map(f.vm,other,many,1025,LM_MAP_WRITE|LM_MAP_EXEC));
    struct lm_range range={.va=LM_VA_MIN,.pages=1};struct lm_job_result r;f.parent_level=2;
    assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1));
    assert(r.profile.masked_leaf_tables==3&&r.profile.masked_leaf_entries==1);
    assert(!lm_core_vm_destroy(f.vm));
    for(unsigned i=0;i<1025;i++)pf(&f.gpu,many+i);
    free(many);for(unsigned i=0;i<f.count;i++)pf(&f.gpu,f.data+i);destroy(&f.gpu,d);
    puts("hierarchical workset: real parent-level masking across 1GiB, mixed PTE isolation, alternating selections, neighbor map/unmap, complete restored permissions and prune/remap PASS");
}
static void fault_snapshot_classes(void) {
    for(unsigned test=0;test<3;test++) {
        struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,2);
        uint64_t other=LM_VA_MIN+0x200000;
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN,f.data,1,LM_MAP_WRITE));
        assert(!lm_core_vm_map(f.vm,other,f.data+1,1,LM_MAP_WRITE));
        uint64_t fault=(test==0?LM_VA_MIN:test==1?other:other+0x200000)+32;
        f.gpu.fault=1;f.gpu.reg[0x241c/4]=0x1002c3;
        f.gpu.reg[0x2420/4]=(uint32_t)fault;f.gpu.reg[0x2424/4]=(uint32_t)(fault>>32);
        struct lm_range range={.va=LM_VA_MIN,.pages=1};struct lm_job_result r;
        assert(lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1)==LM_EIO);
        assert(r.reset_completed&&!r.quarantined&&r.fault_address==fault&&r.mmu_fault==0x1002c3);
        assert(r.fault_vm.valid&&r.fault_vm.scoped);
        assert(r.fault_vm.mapped==(test!=2)&&r.fault_vm.selected==(test==0)&&r.fault_vm.visible==(test==0));
        if(test!=2)assert(r.fault_vm.va==(test==0?LM_VA_MIN:other)&&r.fault_vm.pages==1);
        else assert(!r.fault_vm.va&&!r.fault_vm.page_pa&&!r.fault_vm.expected_pte);
        assert(lm_core_vm_lookup(f.vm,other,NULL)==f.data[1].pa); /* Restored later, not snapshotted as visible. */
        cleanup(&f,d);
    }
    puts("MMU snapshot: selected-valid, retained-but-masked and absent VA distinguished before reset/restore; no GPU-buffer reads or revived device PASS");
}
static void incremental_alias_index(void) {
    struct workset_fake f;struct lm_core *d=work_device(&f,true);add_pages(&f,6);
    uint64_t model[4];
    for(unsigned i=0;i<4;i++) {
        model[i]=f.data[i].pa;
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN+i*4096,f.data+i,1,LM_MAP_WRITE));
    }
    struct lm_range range={.va=LM_VA_MIN,.pages=1};struct lm_job_result r;
    assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1));
    assert(r.profile.scope_mode==1);
    for(unsigned step=0;step<800;step++) {
        unsigned slot=1+step%3;
        assert(!lm_core_vm_unmap(f.vm,LM_VA_MIN+slot*4096,1));
        struct lm_page page=f.data[slot];
        /* Same hash bucket at capacity8, plus cross-BO aliases to JC/others. */
        page.pa=f.data[0].pa+(uint64_t)((step*13)%7)*8*4096;
        model[slot]=page.pa;
        assert(!lm_core_vm_map(f.vm,LM_VA_MIN+slot*4096,&page,1,LM_MAP_WRITE));
        bool aliases=false;
        for(unsigned i=0;i<4;i++)for(unsigned j=i+1;j<4;j++)if(model[i]==model[j])aliases=true;
        /* Rebuilding would fail: updates must maintain exact collision chains
         * and refcounts, including deleting the first/middle/last entry. */
        f.fail_hash=true;
        assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,step&1,10000000,&r,&range,1));
        f.fail_hash=false;
        assert(r.profile.scope_mode==(aliases?2u:1u));
        assert(r.profile.clean_pages==(aliases?4u:1u)&&r.profile.cpu_pages==(aliases?4u:1u));
        for(unsigned i=0;i<4;i++)assert(lm_core_vm_lookup(f.vm,LM_VA_MIN+i*4096,NULL)==model[i]);
    }
    for(unsigned i=1;i<4;i++)assert(!lm_core_vm_unmap(f.vm,LM_VA_MIN+i*4096,1));
    struct lm_page pair[2]={f.data[4],f.data[5]};pair[1].pa=pair[0].pa;
    assert(!lm_core_vm_map(f.vm,LM_VA_MIN+0x200000,pair,2,LM_MAP_WRITE));
    f.fail_hash=true;
    assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1)&&r.profile.scope_mode==2);
    f.fail_hash=false;assert(!lm_core_vm_unmap(f.vm,LM_VA_MIN+0x200000,2));
    f.fail_hash=true;
    assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1)&&r.profile.scope_mode==1);
    f.fail_hash=false;
    /* Exceed half-full capacity: discard, then OOM must use full VM until
     * rebuilding succeeds. Rebuilding cannot retain stale alias counters. */
    for(unsigned i=1;i<5;i++)assert(!lm_core_vm_map(f.vm,LM_VA_MIN+i*4096,f.data+i,1,LM_MAP_WRITE));
    f.fail_hash=true;
    assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1));
    assert(r.profile.scope_mode==3&&r.profile.clean_pages==5&&r.profile.cpu_pages==5);
    f.fail_hash=false;
    assert(!lm_core_submit_ranges(f.vm,LM_VA_MIN,0,10000000,&r,&range,1)&&r.profile.scope_mode==1);
    cleanup(&f,d);
    puts("PA alias index: 800 colliding map/unmap generations match independent multiset oracle without reallocation; within-BO duplicates, alias removal, growth/OOM full-VM and rebuild/teardown PASS");
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"quarantine"))faults(true);
    else {selected();invalid_ranges();aliases_and_oom();leaf_boundaries();hierarchical_masks();incremental_alias_index();faults(false);fault_snapshot_classes();}
    return 0;
}
