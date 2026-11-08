/* Same core as XNU, fake MMIO only. Tests never access phone or real MMIO. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_core.h"
#include "lancelot_mali_private_dma.h"
#include "lancelot_gpu_shaders.h"
struct fake {
    uint32_t reg[4096]; uint64_t time, pa;
    unsigned allocs,pages,clean,invalidate,reads,writes;
    unsigned fail_page_after; int fault,reset_fail,ioerr; bool run;
    uint32_t fail_read,fail_write; bool fail_after_done,submitted;
    bool drop_bind_root,drop_detach_config,drop_next_head,stale_done,done_busy;
    bool emulate_value,corrupt_guard,reset_fail_after_submit;
    bool emulate_clear,emulate_triangle,corrupt_pixel,corrupt_pixel2;
    unsigned jobs_run;
    uint32_t corrupt_bind_reg;
    unsigned active_slot;
    bool done_current_busy;
    bool zero_thread_priority,drop_next_config;
    uint64_t fault_va_xor;uint32_t fault_status;
    unsigned event,last_clean,last_barrier,bind_event,irq_event,trace_event;
    unsigned cache_event,cache_event_at_next,next_event,launch_event;
    unsigned cache_commands,cache_before_next,next_writes,launches,pre_job_calls;
    unsigned cache_timeout_at,cache_fault_at,cache_ioerr_at;
    uint64_t traced_jc;uint32_t traced_words[4];
    bool record_as;unsigned as_count,as_fail_at;uint32_t as_ops[16];
    bool check_gpu_share;unsigned expected_gpu_share;
    struct lm_page owned[2048];
};
static void *ma(void *o,size_t n) {struct fake*f=o;void*p=calloc(1,n);if(p)f->allocs++;return p;}
static void mf(void *o,void*p,size_t n) {(void)n;assert(p);((struct fake*)o)->allocs--;free(p);}
static int pa(void *o,struct lm_page *p) {
    struct fake*f=o;
    if(f->fail_page_after && f->pages>=f->fail_page_after)return -1;
    if(posix_memalign(&p->cpu,4096,4096))return -1;
    memset(p->cpu,0,4096);f->pa+=0x3000;p->pa=f->pa;f->pages++;
    for(unsigned i=0;i<2048;i++)if(!f->owned[i].cpu){f->owned[i]=*p;return 0;}
    /* Sparse stress can temporarily exceed this emulator's small tracking
     * array only with emulate_value off; that test does not walk GPU tables. */
    assert(!f->emulate_value);return 0;
}
static void pf(void *o,struct lm_page *p) {
    struct fake*f=o;assert(p->cpu);
    for(unsigned i=0;i<2048;i++)if(f->owned[i].cpu==p->cpu)f->owned[i]=(struct lm_page){0};
    free(p->cpu);f->pages--;p->cpu=NULL;
}
static void *physical(struct fake*f,uint64_t address) {
    for(unsigned i=0;i<2048;i++)if(f->owned[i].cpu&&address>=f->owned[i].pa&&address-f->owned[i].pa<4096)
        return (char*)f->owned[i].cpu+(address-f->owned[i].pa);
    return NULL;
}
static void *gpu_address(struct fake*f,uint64_t va) {
    uint64_t root=(uint64_t)f->reg[0x2400/4]|(uint64_t)f->reg[0x2404/4]<<32;
    unsigned t0sz=(f->reg[0x2430/4]>>6)&63u;
    assert(t0sz==0 || t0sz==20);
    unsigned levels=t0sz==20?3u:4u;
    for(unsigned level=0;level<levels;level++) {
        uint64_t *table=physical(f,root);assert(table);
        uint64_t e=table[(va>>(12+9*(levels-1-level)))&511];if((e&3)!=3)return NULL;
        if(f->check_gpu_share) {
            assert(((e>>8)&3)==(level==levels-1?f->expected_gpu_share:0));
            if(level==levels-1) {
                assert(((e>>2)&7)==0); /* the bound MEMATTR[0] policy */
                assert(((e>>6)&3)==1 && (e&(UINT64_C(1)<<54)));
            }
        }
        root=e&UINT64_C(0x000000fffffff000);
        if(level==levels-1)return physical(f,root+(va&4095));
    }
    return NULL;
}
static uint64_t jq(const uint32_t*j,unsigned w){return (uint64_t)j[w]|(uint64_t)j[w+1]<<32;}
static uint64_t tri_abuf_ptr(const uint32_t*b) {
    uint64_t q=(uint64_t)(b[0]>>6)|((uint64_t)(b[1]&0xffffff)<<26);
    return q<<6;
}
/* Emulate the two Bifrost submissions the driver must emit: JS1 carries the
 * VERTEX->TILER chain, JS0 carries the FRAGMENT job. This is not a shader
 * interpreter: it proves the driver packed the exact descriptors the Mesa
 * reference produced, then models their observable writes. */
static void fake_triangle(struct fake*f,uint64_t jc) {
    uint32_t *j0=gpu_address(f,jc);assert(j0);
    assert(j0[4]&1u); /* 64-bit job header */
    unsigned type0=(j0[4]>>1)&0x7fu;
    if(type0==5) {
        assert(f->jobs_run==0);
        uint32_t *vj=j0;
        uint64_t tiler_va=jq(vj,6);
        uint32_t *tj=gpu_address(f,tiler_va);assert(tj);
        assert(((tj[4]>>1)&0x7fu)==7u&&jq(tj,6)==0);
        assert(((vj[4]>>16)&0xffffu)==1u&&(vj[5]&0xffffu)==0u);
        assert(((tj[4]>>16)&0xffffu)==2u&&(tj[5]&0xffffu)==1u);
        /* Vertex job draw: vertex RSD, input attributes and output varyings. */
        uint32_t *vd=vj+16;
        uint32_t *vrsd=gpu_address(f,jq(vd,14));assert(vrsd);
        assert(jq(vrsd,0)==LM_TRI_SHADER_PAGE_VA);
        assert(vrsd[3]==0x00020001u&&vrsd[4]==0x00002000u&&vrsd[12]==0x00006000u);
        assert(jq(vd,4)==LM_PRIVATE_JOB_VA+LM_TRI_POS_BUF);
        assert(jq(vd,24)==LM_PRIVATE_JOB_VA+LM_TRI_VIEWPORT);
        assert(jq(vd,6)==LM_TRI_POLY_VA&&jq(vd,28)==LM_TRI_POLY_VA);
        uint32_t *attr=gpu_address(f,jq(vd,18));assert(attr);
        assert(((attr[0]>>10)&0x3fffffu)==(16u|(175u<<12)));
        assert(attr[0]&(1u<<9)); /* input attribute offset is enabled */
        assert((attr[0]&0x1ffu)==0u);
        uint32_t *abuf=gpu_address(f,jq(vd,16));assert(abuf);
        assert(tri_abuf_ptr(abuf)==LM_PRIVATE_JOB_VA+LM_TRI_VERTEX_DATA);
        assert(abuf[2]==8u&&abuf[3]==LM_PAGE_SIZE-LM_TRI_VERTEX_DATA);
        assert(abuf[4]==1u&&abuf[5]==0u&&abuf[6]==0u&&abuf[7]==0u);
        uint32_t *vin=gpu_address(f,tri_abuf_ptr(abuf));assert(vin);
        /* Screen-space triangle (0,0),(128,0),(0,128); see LM topology note. */
        static const uint32_t want_in[6]={0x00000000u,0x00000000u,0x43000000u,
            0x00000000u,0x00000000u,0x43000000u};
        for(unsigned i=0;i<6;i++)assert(vin[i]==want_in[i]);
        uint32_t *vary=gpu_address(f,jq(vd,22));assert(vary);
        assert((vary[0]&0x1ffu)==1u&&((vary[0]>>10)&0x3fffffu)==(94u<<12));
        assert(!(vary[0]&(1u<<9))); /* v7 varying offset is disabled */
        assert((vary[2]&0x1ffu)==2u&&((vary[2]>>10)&0x3fffffu)==(199u<<12));
        assert(!(vary[2]&(1u<<9)));
        uint32_t *vbuf=gpu_address(f,jq(vd,20));assert(vbuf);
        assert(vbuf[0]==0u&&vbuf[1]==0u&&vbuf[2]==0u&&vbuf[3]==0u);
        assert(tri_abuf_ptr(vbuf+4)==LM_PRIVATE_JOB_VA+LM_TRI_POS_BUF);
        assert(vbuf[6]==16u&&vbuf[7]==48u);
        assert(tri_abuf_ptr(vbuf+8)==LM_PRIVATE_JOB_VA+LM_TRI_PSIZ_BUF);
        assert(vbuf[10]==2u&&vbuf[11]==6u);
        /* Tiler job: primitive, tiler context/heap and fragment draw. */
        uint32_t *tp=tj+10;
        assert((tp[0]&0xffu)==8u); /* Triangles */
        assert(tp[3]==2u);         /* index_count - 1 */
        assert(jq(tj,18)==LM_PRIVATE_JOB_VA+LM_TRI_TILER_CTX);
        uint32_t *ctx=gpu_address(f,jq(tj,18));assert(ctx);
        assert(jq(ctx,0)==0); /* Bifrost polygon list is heap-resident */
        assert(ctx[2]==6u&&ctx[3]==0x003f003fu);
        uint64_t heap_desc=jq(ctx,6);
        uint32_t *hd=gpu_address(f,heap_desc);assert(hd);
        assert(hd[1]==LM_TRI_HEAP_BYTES);
        assert((jq(hd,2))==LM_TRI_HEAP_VA&&(jq(hd,6))==LM_TRI_HEAP_VA+LM_TRI_HEAP_BYTES);
        assert(jq(tj,16)==LM_PRIVATE_JOB_VA+LM_TRI_PSIZ_BUF);
        uint32_t *td=tj+32;
        assert(jq(td,16)==LM_PRIVATE_JOB_VA+LM_TRI_VATTR_BUF);
        assert(jq(td,18)==LM_PRIVATE_JOB_VA+LM_TRI_VATTR);
        assert(jq(td,20)==LM_PRIVATE_JOB_VA+LM_TRI_VARY_BUF);
        assert(jq(td,22)==LM_PRIVATE_JOB_VA+LM_TRI_VARY_ATTR);
        assert(jq(td,6)==LM_TRI_POLY_VA&&jq(td,28)==LM_TRI_POLY_VA);
        uint32_t *frsd=gpu_address(f,jq(td,14));assert(frsd);
        assert(jq(frsd,0)==LM_TRI_SHADER_PAGE_VA+0x100u);
        assert(frsd[4]==0x41582200u&&frsd[8]==0x37d8ffffu&&
               frsd[9]==0x40000000u&&frsd[12]==0x00000080u);
        uint32_t *blend=(uint32_t *)((uint8_t *)frsd+64);
        assert(blend[0]==0x00000a00u&&blend[1]==0xf0122122u&&
               blend[2]==0x00000019u&&blend[3]==0x010ed000u);
        uint32_t *vp=gpu_address(f,jq(td,24));assert(vp);
        assert(vp[2]==0x42800000u&&vp[7]==0x003f003fu);
        assert(jq(td,4)==LM_PRIVATE_JOB_VA+LM_TRI_POS_BUF);
        uint32_t *pos=gpu_address(f,jq(td,4));assert(pos);
        static const uint32_t want_pos[12]={0x00000000u,0x00000000u,0u,0x3f800000u,
            0x43000000u,0x00000000u,0u,0x3f800000u,
            0x00000000u,0x43000000u,0u,0x3f800000u};
        for(unsigned i=0;i<12;i++)pos[i]=want_pos[i];
        uint16_t *psiz=gpu_address(f,jq(tj,16));assert(psiz);
        for(unsigned i=0;i<3;i++)psiz[i]=0x3c00u;
        f->jobs_run=2;
        return;
    }
    /* Fragment submission on JS0: one job, its own chain index 1. */
    assert(type0==9u&&f->jobs_run==2);
    assert(((j0[4]>>16)&0xffffu)==1u&&(j0[5]&0xffffu)==0u&&jq(j0,6)==0);
    uint32_t *fbd=gpu_address(f,jq(j0,10));assert(fbd);
    assert(fbd[8]==0x003f003fu&&fbd[10]==0x003f003fu);
    assert(fbd[11]==0x01001080u&&fbd[12]==0x00010000u);
    assert(jq(fbd,14)==LM_PRIVATE_JOB_VA+LM_TRI_TILER_CTX);
    uint32_t *rt=fbd+32;
    assert((rt[0]>>26)==1u&&rt[1]==0x06888299u);
    assert(jq(rt,8)==LM_PRIVATE_OUTPUT_VA&&rt[10]==256u);
    uint64_t out_va=jq(rt,8);
    for(unsigned i=0;i<LM_TRI_WIDTH*LM_TRI_HEIGHT;i++) {
        uint32_t *p=gpu_address(f,out_va+4u*i);assert(p);
        *p=LM_TRI_VALUE;
        if(f->corrupt_pixel&&i==1)*p^=1u;
    }
    f->jobs_run=3;
}
static void clean(void*o,void*p,size_t n) {assert(p&&n==4096);struct fake*f=o;f->clean++;f->last_clean=++f->event;}
static void inval(void*o,void*p,size_t n) {assert(p&&n==4096);((struct fake*)o)->invalidate++;}
static void barrier(void*o) {struct fake*f=o;f->last_barrier=++f->event;}
static void pre_job(void*o,uint64_t jc,const uint32_t words[4]) {
    struct fake*f=o;
    assert(f->bind_event>f->last_clean&&f->irq_event>f->bind_event&&f->last_barrier>f->irq_event);
    f->trace_event=++f->event;f->pre_job_calls++;f->traced_jc=jc;
    memcpy(f->traced_words,words,sizeof(f->traced_words));
}
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
    if(r==0x2418&&f->record_as) {
        assert(f->as_count<16);f->as_ops[f->as_count++]=x;
        if(f->as_count==f->as_fail_at)f->reg[0x2428/4]=1;
    }
    if(r==0x2418&&x==1&&(f->reg[0x2400/4]||f->reg[0x2404/4]))f->bind_event=++f->event;
    if(r==0x24&&x==0x81)f->irq_event=++f->event;
    if(r==0x18c0||r==0x1840) {
        f->next_writes++;f->next_event=++f->event;
        f->cache_before_next=f->cache_commands;f->cache_event_at_next=f->cache_event;
    }
    if(r==0x24)f->reg[0x20/4]&=~x;
    if(r==0x1004&&!f->stale_done)f->reg[0x1000/4]&=~x;
    if(r==0x1004)f->reg[0x1010/4]=f->run?(1u<<f->active_slot):0;
    if(r==0x2004)f->reg[0x2000/4]&=~x;
    if(r==0x30 && x==1 && !f->reset_fail && !(f->reset_fail_after_submit&&f->submitted)) {
        f->reg[0x20/4]|=1u<<8;f->reg[0x1860/4]=f->reg[0x18e0/4]=0;f->run=false;
    }
    if(r==0x30 && x==8) {
        f->cache_commands++;f->cache_event=++f->event;
        if(f->cache_commands!=f->cache_timeout_at)f->reg[0x20/4]|=1u<<17;
        if(f->fault==4||f->cache_commands==f->cache_fault_at)f->reg[0x2000/4]|=1;
        if(f->cache_commands==f->cache_ioerr_at)f->ioerr=1;
    }
    if(r==0x2418 && x==5 && f->fault==5)f->reg[0x20/4]|=1;
    if(r==0x1a0||r==0x1a4||r==0x190||r==0x194||r==0x180||r==0x184)f->reg[(r-0x40)/4]=x;
    if(r==0x1c0||r==0x1c4||r==0x1d0||r==0x1d4||r==0x1e0||r==0x1e4)f->reg[(r-0x80)/4]&=~x;
    if((r==0x1860||r==0x18e0)&&x==1) {
        f->launches++;f->launch_event=++f->event;
        unsigned slot=r==0x1860?0:1;f->active_slot=slot;f->run=true;f->submitted=true;
        if(f->fault==1)f->reg[0x2000/4]|=1;
        else if(f->fault==2)f->reg[0x1000/4]|=1u<<(16+slot);
        else if(f->fault==3) {} /* timeout */
        else {
            if(f->emulate_clear) {
                uint64_t jc=(uint64_t)f->reg[(r-0x20)/4]|(uint64_t)f->reg[(r-0x1c)/4]<<32;
                for(unsigned depth=0;jc;depth++) {
                    assert(depth<4); /* bounded job chain */
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
                            if(f->corrupt_pixel&&depth==0&&py==0&&px==1)*p^=1u;
                            if(f->corrupt_pixel2&&depth==1&&py==0&&px==1)*p^=1u;
                        }
                    f->jobs_run++;
                    jc=(uint64_t)job[6]|(uint64_t)job[7]<<32;
                }
                if(f->fail_write&&r==f->fail_write&&(!f->fail_after_done||f->submitted))f->ioerr=1;
            } else if(f->emulate_triangle) {
                uint64_t jc=(uint64_t)f->reg[(r-0x20)/4]|(uint64_t)f->reg[(r-0x1c)/4]<<32;
                fake_triangle(f,jc);
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
static struct lm_core *bare_device_mapping_policy(struct fake *f,bool preflush,bool dump,unsigned asflush,bool shared) {
    memset(f,0,sizeof(*f));f->pa=0x100000000ull;
    f->reg[0]=0x72121000;f->reg[0x100/4]=5;f->reg[0x110/4]=1;f->reg[0x120/4]=1;
    f->reg[0x18/4]=1;f->reg[0x1c/4]=3;f->reg[0x1a0/4]=768;
    struct lm_ops ops={.cookie=f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,.alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,.barrier=barrier,.io_error=ioerr,
        .pre_job_flush=preflush,.pre_job=dump?pre_job:NULL,.pre_as_flush=asflush,.gpu_shareable=shared};
    struct lm_core*d=lm_core_create(&ops);assert(d);return d;
}
static struct lm_core *bare_device_policy(struct fake *f,bool preflush,bool dump,unsigned asflush) {
    return bare_device_mapping_policy(f,preflush,dump,asflush,false);
}
static struct lm_core *bare_device_options(struct fake *f,bool preflush,bool dump) {return bare_device_policy(f,preflush,dump,0);}
static struct lm_core *bare_device(struct fake *f) {return bare_device_options(f,false,false);}
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
static void preflush_ordering(void) {
    for(unsigned flush=0;flush<2;flush++)for(unsigned dump=0;dump<2;dump++) {
        struct fake f;struct lm_core*d=bare_device_options(&f,flush,dump);assert(!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);struct lm_page p[2];
        assert(!pa(&f,p)&&!pa(&f,p+1));assert(p[1].pa!=p[0].pa+4096);
        assert(!lm_core_vm_map(v,LM_VA_MIN,p,2,LM_MAP_WRITE));
        /* Alternate GPU pages, including the final aligned header in page 1.
         * Changing every word proves the probe reads the current allocation. */
        for(unsigned i=0;i<34;i++) {
            unsigned page=i&1,offset=page?4032:64;
            uint64_t jc=LM_VA_MIN+4096*page+offset;
            uint32_t words[4]={0x12340000+i,0x56780000+i,0x9abc0000+i,0xdef00000+i};
            memcpy((char*)p[page].cpu+offset,words,sizeof(words));
            struct lm_job_result r;
            assert(!lm_core_submit_wait(v,jc,i&1,1000000,&r));
            assert(r.sequence==i+1&&r.failed_stage==LM_JOB_NONE);
            assert(f.cache_commands==(i+1)*(1+flush));
            assert(f.cache_before_next==i*(1+flush)+flush);
            assert(f.last_clean<f.bind_event&&f.bind_event<f.irq_event);
            assert(f.irq_event<f.next_event&&f.next_event<f.launch_event&&f.launch_event<f.cache_event);
            if(flush)assert(f.irq_event<f.cache_event_at_next&&f.cache_event_at_next<f.next_event);
            assert(f.next_writes==i+1&&f.launches==i+1);
            assert(f.pre_job_calls==(dump?(i<32?i+1:32):0));
            if(dump&&i<32) {
                assert(f.traced_jc==jc&&!memcmp(f.traced_words,words,sizeof(words)));
                assert(f.irq_event<f.trace_event&&f.trace_event<f.next_event);
                if(flush)assert(f.trace_event<f.cache_event_at_next);
            }
        }
        assert(!lm_core_vm_destroy(v));pf(&f,p);pf(&f,p+1);destroy(&f,d);
    }
    puts("preflush: A/B x dump on/off, CPU-clean -> bind -> IRQ -> probe -> pre-cache -> NEXT -> launch -> post-cache; 32-snapshot limit and sparse-page offsets PASS");
}
static void preflush_failures(void) {
    for(unsigned fault=0;fault<6;fault++) {
        struct fake f;struct lm_core*d=bare_device_options(&f,true,true);assert(!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
        assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));
        if(fault<4) {
            if(fault&1)f.cache_fault_at=1+fault/2;
            else f.cache_timeout_at=1+fault/2;
        } else if(fault==4)f.drop_bind_root=true;
        else {f.stale_done=true;f.reg[0x1000/4]=2;}
        struct lm_job_result r;
        int e=lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r);
        assert(e==(fault<4&&!(fault&1)?LM_ETIMEDOUT:LM_EIO));
        assert(r.failed_stage==(fault<4?LM_JOB_CACHE:fault==4?LM_JOB_BIND:LM_JOB_IRQ_CLEAR));
        assert(f.launches==(fault==2||fault==3?1u:0u)&&f.next_writes==f.launches);
        assert(f.cache_commands==(fault<4?1+fault/2:0));
        assert(f.pre_job_calls==(fault<4?1u:0u));
        assert(r.reset_completed&&!r.quarantined&&!lm_core_ready(d));
        assert(lm_core_vm_safe_to_free(v));assert(!lm_core_vm_destroy(v));pf(&f,&p);destroy(&f,d);
    }
    puts("preflush failures: pre-cache timeout/fault prevent NEXT and launch; post-cache retained; failed bind/IRQ skip both; reset-before-release PASS");
}
static void preflush_quarantine(void) {
    for(unsigned transport=0;transport<2;transport++) {
        struct fake f;struct lm_core*d=bare_device_options(&f,true,true);assert(!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
        assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));
        if(transport)f.cache_ioerr_at=1;
        else {f.cache_timeout_at=1;f.reset_fail=1;}
        struct lm_job_result r;
        assert(lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r)<0);
        assert(r.failed_stage==LM_JOB_CACHE&&!f.next_writes&&!f.launches);
        assert(r.quarantined&&!r.reset_completed&&!lm_core_vm_safe_to_free(v));
        assert(lm_core_vm_destroy(v)==LM_EBUSY&&lm_core_destroy(d)==LM_EBUSY);
    }
    puts("preflush quarantine: failed reset or cache-command transport retains backing without launch PASS");
}
static void preasflush(void) {
    for(unsigned mode=0;mode<3;mode++) {
        struct fake f;struct lm_core*d=bare_device_policy(&f,true,true,mode);assert(!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
        assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));
        f.record_as=true;struct lm_job_result r;
        assert(!lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r));
        const uint32_t legacy[]={1,2,5,1};
        const uint32_t flushed[]={1,2,mode==1?5u:4u,1,2,5,1};
        assert(f.as_count==(mode?7u:4u));
        assert(!memcmp(f.as_ops,mode?flushed:legacy,f.as_count*sizeof(uint32_t)));
        assert(f.launches==1&&f.pre_job_calls==1&&f.cache_commands==2);
        assert(f.reg[0x2410/4]==31&&f.reg[0x2414/4]==0);
        assert(!lm_core_vm_destroy(v));pf(&f,&p);destroy(&f,d);
    }
    for(unsigned mode=1;mode<3;mode++)for(unsigned fail=2;fail<=4;fail++) {
        struct fake f;struct lm_core*d=bare_device_policy(&f,true,true,mode);assert(!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);struct lm_page p;assert(!pa(&f,&p));
        assert(!lm_core_vm_map(v,LM_VA_MIN,&p,1,LM_MAP_WRITE));
        f.record_as=true;f.as_fail_at=fail;struct lm_job_result r;
        assert(lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r)==LM_ETIMEDOUT);
        assert(r.failed_stage==LM_JOB_AS_FLUSH&&r.reset_completed&&!r.quarantined);
        assert(!f.next_writes&&!f.launches&&!f.pre_job_calls&&!f.cache_commands);
        assert(!lm_core_vm_destroy(v));pf(&f,&p);destroy(&f,d);
    }
    puts("preasflush: exclusive off/MEM/PT sequences after UPDATE; full-VA lock; lock/flush/re-UPDATE timeouts prevent launch and recover PASS");
}
static void gpu_sharing(void) {
    for(unsigned shared=0;shared<2;shared++) {
        struct fake f;struct lm_core*d=bare_device_mapping_policy(&f,true,true,0,shared!=0);
        assert(!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);assert(v);
        struct lm_page pages[2];assert(!pa(&f,pages)&&!pa(&f,pages+1));
        assert(!lm_core_vm_map(v,LM_VA_MIN,pages,2,LM_MAP_WRITE));
        f.emulate_value=true;f.check_gpu_share=true;f.expected_gpu_share=shared?3:0;
        for(unsigned step=0;step<3;step++) {
            uint32_t value=0x10203040u+step*0x01010101u;
            lm_v7_dma_prepare(pages[0].cpu,pages[1].cpu,LM_VA_MIN+4096,value);
            struct lm_job_result r;
            assert(!lm_core_submit_wait(v,LM_VA_MIN,0,1000000,&r));
            uint32_t bad=0;bool job=false;
            assert(lm_v7_dma_validate(pages[0].cpu,pages[1].cpu,value,&bad,&job));
            assert((f.reg[0x2408/4]&0xffu)==0x88u && f.reg[0x304/4]==31);
        }
        assert(f.launches==3 && f.invalidate==6);
        assert(!lm_core_vm_destroy(v));pf(&f,pages);pf(&f,pages+1);destroy(&f,d);
    }
    puts("GPU mapping policy: legacy/inner-shared leaf walks, unchanged attr0/RW/NX, nonshared tables, explicit CPU maintenance, repeated DMA and teardown PASS; no hardware");
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
static void private_clear_chain(void) {
    struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
    f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_clear=true;
    struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
        .barrier=barrier,.io_error=ioerr};
    struct lm_private_dma s={.clear_test=true,.clear_chain=true,
        .clear_value=LM_CLEAR_VALUE,.clear_value2=LM_CLEAR_VALUE2};
    assert(lm_private_dma_run(&s,&io)==LM_OK);
    assert(s.submitted&&s.guards_valid&&s.cleaned&&!s.backing_retained);
    assert(s.observed_value==LM_CLEAR_VALUE&&f.jobs_run==2);
    assert(s.descriptor_status==0x00010013);
    assert(!s.bad_offset&&!s.bad_job&&!f.pages&&!f.allocs&&!f.run);
    puts("Private chained clear: two chained FRAGMENT jobs write two RGBA8 surfaces, job.next and release proved; RAM ONLY");
}
static void private_clear_chain_mismatch(void) {
    struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
    f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_clear=true;f.corrupt_pixel2=true;
    struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
        .barrier=barrier,.io_error=ioerr};
    struct lm_private_dma s={.clear_test=true,.clear_chain=true,
        .clear_value=LM_CLEAR_VALUE,.clear_value2=LM_CLEAR_VALUE2};
    assert(lm_private_dma_run(&s,&io)==LM_EIO);
    assert(!s.guards_valid&&s.bad_offset==LM_CLEAR_BYTES+4u);
    assert(s.clear_actual==(LM_CLEAR_VALUE2^1u));
    assert(s.job_result.expected==LM_CLEAR_VALUE2&&f.jobs_run==2);
    assert(s.cleaned&&!s.backing_retained&&!f.pages&&!f.allocs);
    puts("Private chained clear negative: a wrong pixel in the second surface is rejected with its exact offset/value; RAM ONLY");
}
static void private_triangle(void) {
    struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
    f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_triangle=true;
    struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
        .barrier=barrier,.io_error=ioerr};
    struct lm_private_dma s={.triangle_test=true,
        .tri_vertex_shader=lmg_tri_vertex_shader,
        .tri_vertex_shader_bytes=LMG_TRI_VERTEX_SHADER_BYTES,
        .tri_fragment_shader=lmg_tri_fragment_shader,
        .tri_fragment_shader_bytes=LMG_TRI_FRAGMENT_SHADER_BYTES};
    assert(lm_private_dma_run(&s,&io)==LM_OK);
    assert(s.submitted&&s.guards_valid&&s.cleaned&&!s.backing_retained);
    assert(s.observed_value==LM_TRI_VALUE&&f.jobs_run==3);
    assert(s.tri_tiler_result.sequence==1&&!s.tri_tiler_result.quarantined);
    assert(s.job_result.sequence==2&&!s.job_result.quarantined);
    assert(!s.tri_bad_stage&&!s.bad_offset&&!f.pages&&!f.allocs&&!f.run);
    puts("Private triangle: dependent VERTEX->TILER->FRAGMENT jobs, real vertex/varying descriptors, RG32F input, D24 FBD, shader colour and release proved; RAM ONLY");
}
static void private_triangle_mismatch(void) {
    struct fake f;struct lm_core*unused=bare_device(&f);assert(!lm_core_destroy(unused));
    f.reg[0]=0x74021000;f.reg[0x100/4]=3;f.emulate_triangle=true;f.corrupt_pixel=true;
    struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
        .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
        .barrier=barrier,.io_error=ioerr};
    struct lm_private_dma s={.triangle_test=true,
        .tri_vertex_shader=lmg_tri_vertex_shader,
        .tri_vertex_shader_bytes=LMG_TRI_VERTEX_SHADER_BYTES,
        .tri_fragment_shader=lmg_tri_fragment_shader,
        .tri_fragment_shader_bytes=LMG_TRI_FRAGMENT_SHADER_BYTES};
    assert(lm_private_dma_run(&s,&io)==LM_EIO);
    assert(!s.guards_valid&&s.tri_bad_stage==0&&s.tri_bad_word==4u);
    assert(s.tri_actual==(LM_TRI_VALUE^1u));
    assert(s.job_result.mismatch_reg==4u&&s.job_result.expected==LM_TRI_VALUE);
    assert(s.cleaned&&!s.backing_retained&&!f.pages&&!f.allocs);
    puts("Private triangle negative: one wrong shader pixel is rejected with its exact offset/value while cleanup still succeeds; RAM ONLY");
}
static void descriptor_fault_snapshot(void) {
    for(unsigned scenario=0;scenario<6;scenario++) {
        struct fake f;struct lm_core *unused=bare_device(&f);assert(!lm_core_destroy(unused));
        f.reg[0]=0x74021000;
        struct lm_ops io={.cookie=&f,.read32=rr,.write32=wr,.now_ns=now,.delay_us=delay,
            .alloc=ma,.free=mf,.page_alloc=pa,.page_free=pf,.for_device=clean,.for_cpu=inval,
            .barrier=barrier,.io_error=ioerr,.capture_descriptors=scenario!=5,.submit_scope=true};
        struct lm_core*d=lm_core_create(&io);assert(d&&!lm_core_start(d));
        struct lm_vm*v=lm_core_vm_new(d);struct lm_page pages[3];
        for(unsigned i=0;i<3;i++)assert(!pa(&f,&pages[i]));
        for(unsigned i=0;i<3;i++)assert(!lm_core_vm_map(v,LM_VA_MIN+i*4096,&pages[i],1,LM_MAP_WRITE));
        uint32_t *job=(uint32_t *)((char *)pages[0].cpu+4032);
        for(unsigned i=0;i<16;i++)job[i]=0x10000+i;
        job[4]=0x10013; /* G52 fragment, first job, 64-bit addresses. */
        uint64_t fbd=scenario==1?0:scenario==2?LM_VA_MIN+16384:LM_VA_MIN+4096;
        job[10]=(uint32_t)fbd|1;job[11]=0;
        uint32_t *params=pages[1].cpu;
        for(unsigned i=0;i<16;i++)params[i]=0x20000+i;
        params[14]=scenario==4?0:(uint32_t)(LM_VA_MIN+8192);params[15]=0;
        uint32_t *tiler=pages[2].cpu,*rt=(uint32_t *)((char *)pages[1].cpu+128);
        for(unsigned i=0;i<16;i++){tiler[i]=0x30000+i;rt[i]=0x40000+i;}
        struct lm_range ranges[3]={{LM_VA_MIN,1,0},{LM_VA_MIN+4096,1,LM_RANGE_READONLY},{LM_VA_MIN+8192,1,0}};
        struct lm_job_result warm;
        assert(!lm_core_submit_ranges(v,LM_VA_MIN+4032,1,1000000,&warm,ranges,scenario==3?1:3));
        /* The scope's PA alias index is lazy. Warm it before measuring the
         * diagnostic path so its allocation is not blamed on the snapshot. */
        unsigned allocations=f.allocs,page_count=f.pages;
        f.fault=1;f.reg[0x241c/4]=0xb002c2;f.reg[0x2420/4]=0;
        struct lm_job_result r;
        assert(lm_core_submit_ranges(v,LM_VA_MIN+4032,1,1000000,&r,ranges,scenario==3?1:3)==LM_EIO);
        assert(r.reset_completed&&!r.quarantined&&!lm_core_ready(d));
        const unsigned expected[]={15,1,1,1,11,0};
        assert(r.descriptors.mask==expected[scenario]);
        if(scenario!=5)assert(!memcmp(r.descriptors.words[0],job,64));
        if(r.descriptors.mask&2u)assert(!memcmp(r.descriptors.words[1],params,64));
        if(r.descriptors.mask&4u)assert(!memcmp(r.descriptors.words[2],tiler,64));
        if(r.descriptors.mask&8u)assert(!memcmp(r.descriptors.words[3],rt,64));
        assert(f.allocs==allocations&&f.pages==page_count);
        assert(!lm_core_vm_destroy(v));for(unsigned i=0;i<3;i++)pf(&f,&pages[i]);destroy(&f,d);
    }
    puts("Descriptor fault snapshots: real core, last-page job header, valid FBD/tiler/RT, NULL/unmapped/hidden pointers, disabled control, reset retention, zero capture allocations PASS");
}
int main(int argc,char**argv) {
    if(argc>1 && !strcmp(argv[1],"descriptor-fault")){descriptor_fault_snapshot();return 0;}
    if(argc>1 && !strcmp(argv[1],"quarantine")){quarantine();preflush_quarantine();return 0;}
    if(argc>1 && !strcmp(argv[1],"mmio")){io_failure();return 0;}
    if(argc>1 && !strcmp(argv[1],"private-quarantine")){private_dma_quarantine();return 0;}
    if(argc>1 && !strcmp(argv[1],"private-clear")){private_clear();private_clear_mismatch();private_clear_chain();private_clear_chain_mismatch();return 0;}
    if(argc>1 && !strcmp(argv[1],"private-triangle")){private_triangle();private_triangle_mismatch();return 0;}
    gpu_sharing();preasflush();preflush_ordering();preflush_failures();identity();mapping();isolation();oom();reclaim();failures();posted_write_proofs();private_dma_transaction();private_mmu_fault();private_clear();private_clear_mismatch();private_clear_chain();private_clear_chain_mismatch();private_triangle();private_triangle_mismatch();return 0;
}
