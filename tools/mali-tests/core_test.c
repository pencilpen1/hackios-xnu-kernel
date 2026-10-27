/* Same core as XNU, fake MMIO only. Tests never access phone or real MMIO. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_core.h"
#include "lancelot_mali_private_dma.h"
struct fake {
    uint32_t reg[4096]; uint64_t time, pa;
    unsigned allocs,pages,clean,invalidate,reads,writes;
    unsigned fail_page_after; int fault,reset_fail,ioerr; bool run;
    uint32_t fail_read,fail_write; bool fail_after_done,submitted;
    bool drop_bind_root,drop_detach_config,drop_next_head,stale_done,done_busy;
    bool emulate_value,corrupt_guard,reset_fail_after_submit;
    bool emulate_clear,corrupt_pixel;
    uint32_t corrupt_bind_reg;
    unsigned active_slot;
    bool done_current_busy;
    bool zero_thread_priority,drop_next_config;
    uint64_t fault_va_xor;uint32_t fault_status;
    struct lm_page owned[32];
};
static void *ma(void *o,size_t n) {struct fake*f=o;void*p=calloc(1,n);if(p)f->allocs++;return p;}
static void mf(void *o,void*p,size_t n) {(void)n;assert(p);((struct fake*)o)->allocs--;free(p);}
static int pa(void *o,struct lm_page *p) {
    struct fake*f=o;
    if(f->fail_page_after && f->pages>=f->fail_page_after)return -1;
    if(posix_memalign(&p->cpu,4096,4096))return -1;
    memset(p->cpu,0,4096);f->pa+=0x3000;p->pa=f->pa;f->pages++;
    for(unsigned i=0;i<32;i++)if(!f->owned[i].cpu){f->owned[i]=*p;return 0;}
    /* Sparse stress can temporarily exceed this emulator's small tracking
     * array only with emulate_value off; that test does not walk GPU tables. */
    assert(!f->emulate_value);return 0;
}
static void pf(void *o,struct lm_page *p) {
    struct fake*f=o;assert(p->cpu);
    for(unsigned i=0;i<32;i++)if(f->owned[i].cpu==p->cpu)f->owned[i]=(struct lm_page){0};
    free(p->cpu);f->pages--;p->cpu=NULL;
}
static void *physical(struct fake*f,uint64_t address) {
    for(unsigned i=0;i<32;i++)if(f->owned[i].cpu&&address>=f->owned[i].pa&&address-f->owned[i].pa<4096)
        return (char*)f->owned[i].cpu+(address-f->owned[i].pa);
    return NULL;
}
static void *gpu_address(struct fake*f,uint64_t va) {
    uint64_t root=(uint64_t)f->reg[0x2400/4]|(uint64_t)f->reg[0x2404/4]<<32;
    for(unsigned level=0;level<4;level++) {
        uint64_t *table=physical(f,root);assert(table);
        uint64_t e=table[(va>>(39-9*level))&511];if((e&3)!=3)return NULL;
        root=e&UINT64_C(0x000000fffffff000);
        if(level==3)return physical(f,root+(va&4095));
    }
    return NULL;
}
static void clean(void*o,void*p,size_t n) {assert(p&&n==4096);((struct fake*)o)->clean++;}
static void inval(void*o,void*p,size_t n) {assert(p&&n==4096);((struct fake*)o)->invalidate++;}
static void barrier(void*o) {(void)o;}
static uint64_t now(void*o) {return ((struct fake*)o)->time;}
static void delay(void*o,uint32_t us) {((struct fake*)o)->time+=us*1000ull;}
static uint32_t rr(void*o,uint32_t reg) {
    struct fake*f=o;assert(reg<0x4000&&!(reg&3));f->reads++;
    if(f->fail_read&&reg==f->fail_read&&(!f->fail_after_done||f->submitted))f->ioerr=1;
    if(f->ioerr)return 0;
    return f->reg[reg/4];
}
static void wr(void*o,uint32_t r,uint32_t x) {
    struct fake*f=o;assert(r<0x4000&&!(r&3));f->writes++;if(f->ioerr)return;
    if((f->drop_bind_root&&r==0x2400&&x) ||
       (f->drop_detach_config&&r==0x2430&&x==1) ||
       (f->drop_next_head&&r==0x18c0&&x))return;
    f->reg[r/4]=x;
    if(r==0x18d8) {
        if(f->zero_thread_priority)f->reg[r/4]&=~UINT32_C(0x000f0000);
        if(f->drop_next_config)f->reg[r/4]^=1u<<8;
    }
    if(f->corrupt_bind_reg&&r==f->corrupt_bind_reg)f->reg[r/4]^=1;
    if(r==0x24)f->reg[0x20/4]&=~x;
    if(r==0x1004&&!f->stale_done)f->reg[0x1000/4]&=~x;
    if(r==0x1004)f->reg[0x1010/4]=f->run?(1u<<f->active_slot):0;
    if(r==0x2004)f->reg[0x2000/4]&=~x;
    if(r==0x30 && x==1 && !f->reset_fail && !(f->reset_fail_after_submit&&f->submitted)) {
        f->reg[0x20/4]|=1u<<8;f->reg[0x1860/4]=f->reg[0x18e0/4]=0;f->run=false;
    }
    if(r==0x30 && x==8){f->reg[0x20/4]|=1u<<17;if(f->fault==4)f->reg[0x2000/4]|=1;}
    if(r==0x2418 && x==5 && f->fault==5)f->reg[0x20/4]|=1;
    if(r==0x1a0||r==0x1a4||r==0x190||r==0x194||r==0x180||r==0x184)f->reg[(r-0x40)/4]=x;
    if(r==0x1c0||r==0x1c4||r==0x1d0||r==0x1d4||r==0x1e0||r==0x1e4)f->reg[(r-0x80)/4]&=~x;
    if((r==0x1860||r==0x18e0)&&x==1) {
        unsigned slot=r==0x1860?0:1;f->active_slot=slot;f->run=true;f->submitted=true;
        if(f->fault==1)f->reg[0x2000/4]|=1;
        else if(f->fault==2)f->reg[0x1000/4]|=1u<<(16+slot);
        else if(f->fault==3) {} /* timeout */
        else {
            if(f->emulate_clear) {
                uint64_t jc=(uint64_t)f->reg[(r-0x20)/4]|(uint64_t)f->reg[(r-0x1c)/4]<<32;
                uint32_t *job=gpu_address(f,jc);assert(job);
                assert((job[4]&UINT32_C(0xfe))==UINT32_C(9u<<1)); /* Fragment */
                assert(job[4]&1u); /* is_64b */
                uint64_t tag=(uint64_t)job[10]|(uint64_t)job[11]<<32;
                uint32_t *fbd=gpu_address(f,tag&~UINT64_C(0x3f));assert(fbd);
                uint32_t width=(fbd[8]&0xffffu)+1u,height=(fbd[8]>>16)+1u;
                assert((fbd[10]&0xffffu)==width-1u);
                assert(((fbd[11]>>9)&0xfu)==8u); /* 16x16 tile area 256, log2 */
                assert((fbd[11]>>24)==1u);       /* 1024-byte cbuf >> 10 */
                uint32_t *rt=fbd+32;
                assert(((rt[0]>>26)&0x3fu)==1u); /* R8G8B8A8 */
                assert(((rt[1]>>3)&0x1fu)==19u); /* writeback R8G8B8A8 */
                assert(((rt[1]>>8)&0xfu)==2u);   /* linear */
                assert(rt[1]&1u);                /* write enable */
                assert(rt[1]&(1u<<31));          /* clean pixel write */
                assert(((rt[1]>>16)&0xfffu)==0x688u); /* identity RGBA swizzle */
                assert(rt[10]==width*4u);
                uint32_t color=rt[12];
                assert(rt[13]==color&&rt[14]==color&&rt[15]==color);
                uint64_t base=(uint64_t)rt[8]|(uint64_t)rt[9]<<32;
                for(unsigned py=0;py<height;py++)
                    for(unsigned px=0;px<width;px++) {
                        uint32_t *p=gpu_address(f,base+4u*(py*width+px));assert(p);
                        *p=color;
                        if(f->corrupt_pixel&&py==0&&px==1)*p^=1u;
                    }
                if(f->fail_write&&r==f->fail_write&&(!f->fail_after_done||f->submitted))f->ioerr=1;
            } else if(f->emulate_value) {
                uint64_t jc=(uint64_t)f->reg[(r-0x20)/4]|(uint64_t)f->reg[(r-0x1c)/4]<<32;
                uint32_t *job=gpu_address(f,jc);assert(job&&job[4]==0x00010005&&job[10]==6);
                uint64_t target_va=(uint64_t)job[8]|(uint64_t)job[9]<<32;
                uint32_t *target=gpu_address(f,target_va);
                if(!target) {
                    uint64_t fault_va=target_va^f->fault_va_xor;
                    f->reg[0x2000/4]|=1;f->reg[0x241c/4]=f->fault_status?f->fault_status:0x3c3;
                    f->reg[0x2420/4]=(uint32_t)fault_va;f->reg[0x2424/4]=(uint32_t)(fault_va>>32);
                    f->reg[0x1000/4]|=1u<<(16+slot);f->reg[r/4]=0;f->run=false;
                    if(f->corrupt_guard)job[16]^=1;
                    if(f->fail_write&&r==f->fail_write&&(!f->fail_after_done||f->submitted))f->ioerr=1;
                    return;
                }
                *target=job[12];if(f->corrupt_guard)target[1]^=1;
            }
            f->reg[0x1000/4]|=1u<<slot;f->reg[(0x1824+slot*0x80)/4]=1;
            f->reg[r/4]=f->done_busy?1:0;f->run=f->done_busy||f->done_current_busy;}
    }
    if(f->fail_write&&r==f->fail_write&&(!f->fail_after_done||f->submitted))f->ioerr=1;
}
static int ioerr(void*o){return ((struct fake*)o)->ioerr?LM_EIO:LM_OK;}
static struct lm_core *bare_device(struct fake *f) {
    memset(f,0,sizeof(*f));f->pa=0x100000000ull;
    f->reg[0]=0x72121000;f->reg[0x100/4]=5;f->reg[0x110/4]=1;f->reg[0x120/4]=1;
    f->reg[0x18/4]=1;f->reg[0x1c/4]=3;f->reg[0x1a0/4]=768;
    struct lm_ops ops={.cookie=f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,.alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,.barrier=barrier,.io_error=ioerr};
    struct lm_core*d=lm_core_create(&ops);assert(d);return d;
}
static struct lm_core *device(struct fake *f) {
    struct lm_core*d=bare_device(f);assert(lm_core_start(d)==LM_OK);
    assert(f->reg[0xf04/4]&(1u<<17));assert(((f->reg[0xf00/4]>>16)&0x3f)==15);
    assert(!(f->reg[0xf0c/4]&((7u<<12)|(7u<<15)))&&f->reg[0x304/4]==31);
    uint64_t value=0;assert(!lm_core_get_param(d,40,&value)&&value==1);
    assert(!lm_core_get_param(d,0,&value)&&value==0x7212);
    return d;
}
static void destroy(struct fake*f,struct lm_core*d) {
    bool powered=f->reg[0x140/4]||f->reg[0x150/4]||f->reg[0x160/4];
    int shutdown_error=lm_core_destroy(d);
    assert(shutdown_error==0);assert(f->allocs==0&&f->pages==0);
    if(powered)assert(!(f->reg[0x140/4]&3)&&!(f->reg[0x150/4]&1)&&!(f->reg[0x160/4]&1));
}
static void identity(void) {
    const uint32_t unsupported[]={0x06000000,0x72121120,0x72121001,0x72120010};
    for(unsigned i=0;i<sizeof(unsupported)/sizeof(*unsupported);i++) {
        struct fake f;struct lm_core*d=bare_device(&f);f.reg[0]=unsupported[i];
        assert(lm_core_start(d)==LM_ENOTSUP&&!f.writes&&!lm_core_ready(d));destroy(&f,d);
    }
    struct fake f;struct lm_core*d=bare_device(&f);f.reg[0]=0x74020000;
    assert(!lm_core_start(d));destroy(&f,d);
    d=bare_device(&f);f.reg[0x34/4]=1u<<7;
    assert(lm_core_start(d)==LM_ENOTSUP&&!f.writes);destroy(&f,d);
    puts("identity: G52 product/revision full mask, unknown errata and protected-mode refusal passed");
}
static void mapping(void) {
    struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);assert(v);
    struct lm_page p[3];for(unsigned i=0;i<3;i++)assert(!pa(&f,p+i));
    assert(!lm_core_vm_map(v,LM_VA_MIN,p,3,LM_MAP_WRITE));
    for(unsigned i=0;i<3;i++){uint32_t flags=0;assert(lm_core_vm_lookup(v,LM_VA_MIN+4096*i+8,&flags)==p[i].pa+8);assert(flags==LM_MAP_WRITE);}
    assert(lm_core_vm_lookup(v,LM_VA_MIN+3*4096,NULL)==0);
    assert(lm_core_vm_map(v,LM_VA_MIN,p,1,0)==LM_EBUSY);
    assert(lm_core_vm_map(v,LM_VA_MIN+1,p,1,0)==LM_EINVAL);
    assert(lm_core_vm_map(v,LM_VA_END-4096,p,3,0)==LM_EINVAL);
    assert(lm_core_vm_map(v,LM_VA_MIN,p,UINT32_MAX,0)==LM_EINVAL);
    uint64_t saved=p[0].pa;p[0].pa|=1;assert(lm_core_vm_map(v,LM_VA_MIN+16384,p,1,0)==LM_EINVAL);p[0].pa=saved;
    assert(lm_core_vm_unmap(v,LM_VA_MIN,2)==LM_EINVAL);
    assert(!lm_core_vm_unmap(v,LM_VA_MIN,3));
    assert(!lm_core_vm_lookup(v,LM_VA_MIN,NULL));
    assert(!lm_core_vm_destroy(v));for(unsigned i=0;i<3;i++)pf(&f,p+i);destroy(&f,d);
    puts("map: noncontiguous physical pages, guard pages, overflow, alignment and exact unmap passed");
}
static void isolation(void) {
    struct fake f;struct lm_core*d=device(&f);struct lm_vm*a=lm_core_vm_new(d),*b=lm_core_vm_new(d);
    struct lm_page p[2];assert(!pa(&f,p));assert(!pa(&f,p+1));
    assert(!lm_core_vm_map(a,LM_VA_MIN,p,1,0));assert(!lm_core_vm_map(b,LM_VA_MIN,p+1,1,LM_MAP_WRITE|LM_MAP_EXEC));
    assert(lm_core_vm_lookup(a,LM_VA_MIN,NULL)!=lm_core_vm_lookup(b,LM_VA_MIN,NULL));
    uint32_t fl;lm_core_vm_lookup(a,LM_VA_MIN,&fl);assert(fl==0);lm_core_vm_lookup(b,LM_VA_MIN,&fl);assert(fl==3);
    struct lm_job_result r;assert(lm_core_submit_wait(a,LM_VA_MIN,0,10000000,&r)==LM_EFAULT);
    assert(!lm_core_vm_unmap(a,LM_VA_MIN,1));assert(!lm_core_vm_map(a,LM_VA_MIN,p,1,LM_MAP_WRITE));
    assert(!lm_core_submit_wait(a,LM_VA_MIN,0,10000000,&r));assert(r.sequence==1&&!r.quarantined);
    assert(!lm_core_submit_wait(b,LM_VA_MIN,1,10000000,&r));assert(r.sequence==2);
    assert(f.reg[0x2400/4]==0&&f.reg[0x2430/4]==1); /* AS unmapped between clients */
    assert(f.clean>=8&&f.invalidate==2);
    assert(!lm_core_vm_destroy(a));assert(!lm_core_vm_destroy(b));pf(&f,p);pf(&f,p+1);destroy(&f,d);
    puts("isolation: same VA distinct PA, RO/NX, AS detach, fake completion protocol and cache callbacks passed");
}
static void oom(void) {
    struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
    f.fail_page_after=f.pages+1; /* partial table hierarchy alloc fails */
    unsigned baseline=f.pages;
    assert(lm_core_vm_map(v,LM_VA_MIN,&p,1,0)==LM_ENOMEM);
    assert(lm_core_vm_lookup(v,LM_VA_MIN,NULL)==0);
    assert(f.pages==baseline); /* empty partial parents must not consume quota */
    f.fail_page_after=0;assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,0));
    assert(!lm_core_vm_destroy(v));pf(&f,&p);destroy(&f,d);
    puts("allocation failure: no partial leaf mapping and cleanup passed");
}
static void reclaim(void) {
    struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);
    struct lm_page p[2];assert(!pa(&f,p));assert(!pa(&f,p+1));
    unsigned baseline=f.pages;
    /* More sparse leaf regions than the table quota: no historical allocation
     * may accumulate across BO release, including 1-GiB parent boundaries. */
    for(unsigned i=0;i<1500;i++) {
        uint64_t va=LM_VA_MIN+((uint64_t)i<<21);
        assert(!lm_core_vm_map(v,va,p,1,LM_MAP_WRITE));
        assert(f.pages==baseline+3);
        assert(!lm_core_vm_unmap(v,va,1));
        assert(f.pages==baseline);
    }
    /* First leaf installed, allocation fails at the next 2-MiB region: rollback
     * removes both leaves and every newly-created empty parent. */
    f.fail_page_after=baseline+3;
    assert(lm_core_vm_map(v,LM_VA_MIN+0x1ff000,p,2,LM_MAP_WRITE)==LM_ENOMEM);
    assert(f.pages==baseline&&!lm_core_vm_lookup(v,LM_VA_MIN+0x1ff000,NULL));
    f.fail_page_after=0;
    assert(!lm_core_vm_map(v,LM_VA_MIN,p,2,LM_MAP_WRITE));
    assert(!lm_core_vm_unmap(v,LM_VA_MIN,2)&&f.pages==baseline);
    assert(!lm_core_vm_destroy(v));pf(&f,p);pf(&f,p+1);destroy(&f,d);
    puts("reclamation: 1500 sparse map/unmap cycles and cross-region partial rollback keep only VM root passed");
}
static void failures(void) {
    for(int fault=1;fault<=5;fault++) {
        struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
        assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));f.fault=fault;struct lm_job_result r;
        assert(lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r)<0);assert(r.reset_completed&&!r.quarantined);
        assert(!lm_core_ready(d));assert(lm_core_vm_safe_to_free(v));assert(!lm_core_vm_destroy(v));pf(&f,&p);destroy(&f,d);
    }
    puts("fault: MMU, JOB, timeout, post-DONE cache and AS flush faults all stop device and prove reset before release passed");
}
static void io_failure(void) {
    for(unsigned where=0;where<3;where++) {
        struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
        assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));
        if(where==0){f.fault=3;f.fail_write=0x18e0;}
        else if(where==1){f.fail_read=0x18a4;f.fail_after_done=true;}
        else {f.fail_write=0x1004;f.fail_after_done=true;}
        struct lm_job_result r;
        assert(lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r)==LM_EIO);
        assert(!lm_core_ready(d));
        assert(r.quarantined&&!r.reset_completed&&!lm_core_vm_safe_to_free(v));
        assert(lm_core_vm_destroy(v)==LM_EBUSY&&lm_core_destroy(d)==LM_EBUSY);
    }
    puts("MMIO transport fault: launch, DONE-status and final ACK failures never falsely prove reset or free backing passed");
}
static void quarantine(void) {
    struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
    assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));f.fault=3;f.reset_fail=1;struct lm_job_result r;
    unsigned before=f.allocs,pages=f.pages;
    assert(lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r)<0);assert(r.quarantined&&!r.reset_completed);
    assert(!lm_core_vm_safe_to_free(v));assert(lm_core_vm_destroy(v)==LM_EBUSY);assert(lm_core_destroy(d)==LM_EBUSY);
    assert(f.allocs==before&&f.pages==pages); /* leak intentionally to model quarantine */
    /* This fixture runs in a child selected by argument; leak checker disabled only
     * for the intentional persistent DMA quarantine process, never for other tests. */
    puts("quarantine: reset timeout retains all VM/backing; no use-after-free path passed");
}
static void posted_write_proofs(void) {
    for(unsigned fault=0;fault<7;fault++) {
        struct fake f;struct lm_core*d=device(&f);struct lm_vm*v=lm_core_vm_new(d);
        struct lm_page p;assert(!pa(&f,&p));assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));
        if(fault==0)f.drop_bind_root=true;
        else if(fault==1)f.drop_detach_config=true;
        else if(fault==2)f.drop_next_head=true;
        else if(fault==3){f.stale_done=true;f.reg[0x1000/4]=2;}
        else if(fault==4)f.done_busy=true;
        else if(fault==5)f.done_current_busy=true;
        else f.drop_next_config=true;
        struct lm_job_result r;
        assert(lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r)==LM_EIO);
        assert(r.reset_completed&&!r.quarantined&&!lm_core_ready(d)&&!f.run);
        if(fault==0||fault==2||fault==3)assert(!f.submitted);
        if(fault==0)assert(r.failed_stage==LM_JOB_BIND&&r.mismatch_reg==0x2400&&
            r.observed==(r.expected&UINT64_C(0xffffffff00000000))&&(r.expected&UINT64_C(0xffffffff)));
        if(fault==2)assert(r.failed_stage==LM_JOB_NEXT&&r.mismatch_reg==0x18c0&&r.observed==0&&r.expected==LM_VA_MIN);
        if(fault==3)assert(r.failed_stage==LM_JOB_IRQ_CLEAR&&!r.mismatch_reg);
        if(fault==5)assert(r.failed_stage==LM_JOB_WAIT&&r.mismatch_reg==0x1010&&(r.job_state&2));
        if(fault==6)assert(!f.submitted&&r.failed_stage==LM_JOB_NEXT&&r.mismatch_reg==0x18d8&&r.expected==0x3300&&r.observed==0x3200);
        assert(!lm_core_vm_destroy(v));pf(&f,&p);destroy(&f,d);
    }
    puts("posted-write proof: dropped AS root/unmap/next-head, stale DONE and DONE-while-busy fail; reset proved before release passed");
}
static void private_dma_transaction(void) {
    for(unsigned test=0;test<3;test++) {
        struct fake f;struct lm_core*unused=bare_device(&f);
        assert(!lm_core_destroy(unused));
        f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_value=true;f.zero_thread_priority=true;
        if(test==1)f.corrupt_guard=true;
        else if(test==2)f.fault=3;
        struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
            .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
            .barrier=barrier,.io_error=ioerr};
        struct lm_private_dma state={0};int e=lm_private_dma_run(&state,&io);
        if(test==0)assert(!e&&state.guards_valid&&state.submitted);
        else if(test==1)assert(e==LM_EIO&&!state.guards_valid&&!state.bad_job&&state.bad_offset==132);
        else assert(e==LM_ETIMEDOUT&&state.job_result.reset_completed);
        assert(state.cleaned&&!state.backing_retained&&!f.pages&&!f.allocs);
        assert(lm_private_dma_run(&state,&io)==LM_EINVAL);
    }
    puts("private DMA orchestration: actual 4-level private page walk/WRITE_VALUE offset128, guard damage and timeout-reset cleanup passed; RAM ONLY");
}
static void private_dma_quarantine(void) {
    for(unsigned test=0;test<4;test++) {
        struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
        f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_value=true;f.fault=test<2?3:0;
        if(test==0||test==2)f.reset_fail_after_submit=true;else f.fail_write=0x18e0;
        struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
            .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
            .barrier=barrier,.io_error=ioerr};
        struct lm_private_dma state={.mmu_fault_test=test>=2};assert(lm_private_dma_run(&state,&io)!=0);
        assert(state.backing_retained&&!state.cleaned&&state.core&&state.vm);
        assert(state.job_page.cpu&&state.output_page.cpu&&state.job_result.quarantined);
        assert(!state.job_result.reset_completed&&!state.expected_fault&&f.pages==6);
        if(test==2) {
            assert(state.job_result.fault_address==state.expected_fault_va);
            assert(state.job_result.mmu_fault==0x3c3);
        } else if(test==3) {
            /* The unmapped-leaf branch honors launch transport failure; after
             * that sticky failure no fault address/status read can be trusted. */
            assert(f.ioerr&&!state.job_result.fault_address&&!state.job_result.mmu_fault);
        }
        unsigned pages=f.pages,allocs=f.allocs;
        assert(lm_private_dma_cleanup(&state)==LM_EBUSY&&f.pages==pages&&f.allocs==allocs);
    }
    puts("private DMA quarantine: timeout and controlled-MMU-fault reset/launch failures retain both data pages, four table pages and metadata; RAM ONLY");
}
static void private_mmu_fault(void) {
    for(unsigned test=0;test<5;test++) {
        struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
        f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_value=true;f.zero_thread_priority=true;
        if(test==1)f.fault_va_xor=0x1000;
        else if(test==2)f.fault_status=0x3cb; /* permission, not translation */
        else if(test==3)f.fault_status=0x2c3; /* read, not WRITE_VALUE */
        else if(test==4)f.corrupt_guard=true;
        struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
            .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
            .barrier=barrier,.io_error=ioerr};
        struct lm_private_dma s={.mmu_fault_test=true};
        assert(lm_private_dma_run(&s,&io)==LM_EIO&&s.submitted&&s.job_result.failed_stage==LM_JOB_WAIT);
        assert(s.job_result.reset_completed&&!s.job_result.quarantined&&s.cleaned&&!s.backing_retained);
        assert(!f.pages&&!f.allocs&&!f.run&&s.observed_value==~LM_PRIVATE_VALUE);
        assert(s.job_result.mmu_fault==(test==2?0x3cb:test==3?0x2c3:0x3c3));
        assert(s.job_result.fault_address==(s.expected_fault_va^(test==1?0x1000:0)));
        if(test==0)assert(s.expected_fault&&s.guards_valid);
        else {
            assert(!s.expected_fault);
            if(test==4)assert(!s.guards_valid&&s.bad_job&&s.bad_offset==64);
            else assert(s.guards_valid);
        }
    }
    puts("Private MMU negative test: unmapped WRITE fault succeeds only with exact VA/type/access, reset, guards and release; all mismatches rejected; RAM ONLY");
}
static void private_clear(void) {
    struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
    f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_clear=true;
    struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
        .barrier=barrier,.io_error=ioerr};
    struct lm_private_dma s={.clear_test=true,.clear_value=LM_CLEAR_VALUE};
    assert(lm_private_dma_run(&s,&io)==LM_OK);
    assert(s.submitted&&s.guards_valid&&s.cleaned&&!s.backing_retained);
    assert(s.observed_value==LM_CLEAR_VALUE&&s.descriptor_status==0x00010013);
    assert(s.job_result.sequence==1&&!s.job_result.quarantined);
    assert(!s.bad_offset&&!s.bad_job&&!f.pages&&!f.allocs&&!f.run);
    puts("Private fragment clear: full 64x64 RGBA8 surface written by a real FRAGMENT job, descriptor/page guards and release proved; RAM ONLY");
}
static void private_clear_mismatch(void) {
    struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
    f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_clear=true;f.corrupt_pixel=true;
    struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
        .barrier=barrier,.io_error=ioerr};
    struct lm_private_dma s={.clear_test=true,.clear_value=LM_CLEAR_VALUE};
    assert(lm_private_dma_run(&s,&io)==LM_EIO);
    assert(!s.guards_valid&&s.bad_offset==4u&&s.clear_actual==(LM_CLEAR_VALUE^1u));
    assert(s.job_result.mismatch_reg==4u&&s.job_result.expected==LM_CLEAR_VALUE);
    assert(s.cleaned&&!s.backing_retained&&!f.pages&&!f.allocs);
    puts("Private clear negative: one wrong pixel is rejected with its exact offset/value while cleanup still succeeds; RAM ONLY");
}
int main(int argc,char**argv) {
    if(argc>1 && !strcmp(argv[1],"quarantine")){quarantine();return 0;}
    if(argc>1 && !strcmp(argv[1],"mmio")){io_failure();return 0;}
    if(argc>1 && !strcmp(argv[1],"private-quarantine")){private_dma_quarantine();return 0;}
    if(argc>1 && !strcmp(argv[1],"private-clear")){private_clear();private_clear_mismatch();return 0;}
    identity();mapping();isolation();oom();reclaim();failures();posted_write_proofs();private_dma_transaction();private_mmu_fault();private_clear();private_clear_mismatch();return 0;
}
