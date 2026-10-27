/* SPDX-License-Identifier: GPL-2.0-only
 * Bounded firmware private-DMA adapter. Real allocator/cache/MMIO operations
 * are supplied by the native UEFI wrapper. No WACS2/SSPM provider or OS branch.
 * Exactly six private GPU4K pages (job/output/four tables), never whole DRAM.
 */
#ifndef XNULOADER_LANCELOT_GPU_DMA_H
#define XNULOADER_LANCELOT_GPU_DMA_H
#include "lancelot_gpu_prepare.h"
#include "lancelot_gpu_shaders.h"
#include "../xnu/files/bsd/dev/lancelot_mali_private_dma.h"
#define LFD_PAGES 32u
#define LFD_USABLE_LAST 30u
#define LFD_GUARD_PAGE 31u
#define LFD_MIN_PA UINT64_C(0x40280000)
#define LFD_MAX_PA UINT64_C(0x44000000)
#define LFD_TRACE_MAX 512u
struct lfd_trace_event {uint32_t off,value;int error;bool read;};
struct lfd_memory_ops {
    void *cookie;
    int (*allocate)(void *,void **,uint64_t *,unsigned);
    int (*release)(void *,void *,uint64_t,unsigned);
    int (*check_page)(void *,void *,uint64_t);
    int (*cache)(void *,void *,size_t,bool);
    void (*barrier)(void *);
    void (*trace)(void *,bool,uint32_t,uint32_t,int);
};
struct lfd_context {
    struct lpg_context *platform;
    struct lmp_state *state;
    struct lfd_memory_ops mem;
    struct lm_private_dma transaction;
    uint8_t *arena;
    uint64_t pa;
    size_t metadata_used;
    struct { void *pointer; size_t size; } metadata[24];
    unsigned page_mask,peak_pages,metadata_active,gpu_reads,gpu_writes,cleans,invalidates;
    int io_error,release_error;
    int first_io_error;
    uint32_t first_io_reg,first_io_phase,phase;
    bool initialized,allocated,safe_to_teardown;
    bool recovery_attempted,recovering,clear_mode,clear_chain,triangle_mode;
    struct lfd_trace_event trace[LFD_TRACE_MAX];
    unsigned trace_count,trace_dropped;
};
/* Never print or call firmware while the bounded GPU lifecycle runs. Keep the
 * first events and the latest event on overflow; the receipt retains failures. */
static void lfd_record_trace(struct lfd_context*f,bool read,uint32_t off,uint32_t value,int error) {
    if(error&&!f->first_io_error){f->first_io_error=error;f->first_io_reg=off;f->first_io_phase=f->phase;}
    unsigned i=f->trace_count;
    if(i<LFD_TRACE_MAX)f->trace_count++;
    else{i=LFD_TRACE_MAX-1;f->trace_dropped++;}
    f->trace[i]=(struct lfd_trace_event){.off=off,.value=value,.error=error,.read=read};
}
static void lfd_flush_trace(struct lfd_context*f) {
    if(!f->mem.trace)return;
    for(unsigned i=0;i<f->trace_count;i++) {
        const struct lfd_trace_event*t=&f->trace[i];
        f->mem.trace(f->mem.cookie,t->read,t->off,t->value,t->error);
    }
}
static int lfd_prepare(struct lfd_context *f,struct lpg_context *platform,
    const struct lfd_memory_ops *mem) {
    if(!f || f->initialized || !platform || !mem || !mem->allocate || !mem->release ||
       !mem->check_page || !mem->cache || !mem->barrier)return LM_EINVAL;
    f->initialized=true;f->platform=platform;f->mem=*mem;
    void *cpu=NULL;uint64_t pa=0;
    int error=mem->allocate(mem->cookie,&cpu,&pa,LFD_PAGES);
    if(error)return error;
    f->arena=cpu;f->pa=pa;f->allocated=true;
    if(!cpu || ((uintptr_t)cpu&4095u) || (pa&4095u) || pa<LFD_MIN_PA ||
       pa>LFD_MAX_PA-LFD_PAGES*4096u)error=LM_EINVAL;
    for(unsigned i=0;!error && i<LFD_PAGES;i++)
        error=mem->check_page(mem->cookie,(uint8_t*)cpu+i*4096u,pa+i*4096u);
    if(error) {
        f->release_error=mem->release(mem->cookie,cpu,pa,LFD_PAGES);
        if(!f->release_error){f->allocated=false;f->arena=NULL;}
        return error;
    }
    memset(f->arena,0,LFD_GUARD_PAGE*4096u);
    for(unsigned i=0;i<1024;i++)((uint32_t *)(f->arena+LFD_GUARD_PAGE*4096u))[i]=UINT32_C(0x61726e47)^i;
    return LM_OK;
}
static void *lfd_alloc(void *cookie,size_t n) {
    struct lfd_context*f=cookie;
    if(!n || n>4096 || f->metadata_used>4096-((n+15u)&~15u))return NULL;
    for(unsigned i=0;i<24;i++)if(!f->metadata[i].pointer) {
        void *p=f->arena+f->metadata_used;f->metadata_used+=(n+15u)&~15u;
        f->metadata[i].pointer=p;f->metadata[i].size=n;f->metadata_active++;
        memset(p,0,n);return p;
    }
    return NULL;
}
static void lfd_free(void *cookie,void *p,size_t n) {
    struct lfd_context*f=cookie;
    for(unsigned i=0;i<24;i++)if(f->metadata[i].pointer==p && f->metadata[i].size==n) {
        f->metadata[i].pointer=NULL;f->metadata_active--;return;
    }
    f->io_error=LM_EIO;
}
static int lfd_page_index(struct lfd_context*f,void *p) {
    uintptr_t v=(uintptr_t)p,a=(uintptr_t)f->arena;
    if(v<a+4096u || v>=a+(LFD_USABLE_LAST+1u)*4096u || ((v-a)&4095u))return -1;
    unsigned i=(unsigned)((v-a)/4096u);
    return (f->page_mask&(1u<<i))?(int)i:-1;
}
static int lfd_page_alloc(void *cookie,struct lm_page *p) {
    struct lfd_context*f=cookie;
    for(unsigned i=1;i<=LFD_USABLE_LAST;i++)if(!(f->page_mask&(1u<<i))) {
        f->page_mask|=1u<<i;p->cpu=f->arena+i*4096u;p->pa=f->pa+i*4096u;
        memset(p->cpu,0,4096u);
        unsigned count=0;for(unsigned j=1;j<=LFD_USABLE_LAST;j++)if(f->page_mask&(1u<<j))count++;
        if(count>f->peak_pages)f->peak_pages=count;
        return LM_OK;
    }
    return LM_ENOMEM;
}
static void lfd_page_free(void *cookie,struct lm_page *p) {
    struct lfd_context*f=cookie;int i=lfd_page_index(f,p->cpu);
    if(i<0 || p->pa!=f->pa+(unsigned)i*4096u){f->io_error=LM_EIO;return;}
    f->page_mask&=~(1u<<(unsigned)i);p->cpu=NULL;p->pa=0;
}
static void lfd_cache(struct lfd_context*f,void *p,size_t n,bool after) {
    if(f->io_error)return;
    if(lfd_page_index(f,p)<0 || n!=4096){f->io_error=LM_EIO;lfd_record_trace(f,after,UINT32_MAX,0,LM_EIO);return;}
    int e=f->mem.cache(f->mem.cookie,p,n,after);
    if(e){f->io_error=e;f->platform->transport_error=e;lfd_record_trace(f,after,UINT32_MAX,0,e);}
    if(after)f->invalidates++;else f->cleans++;
}
static void lfd_for_device(void *cookie,void *p,size_t n){lfd_cache(cookie,p,n,false);}
static void lfd_for_cpu(void *cookie,void *p,size_t n){lfd_cache(cookie,p,n,true);}
static void lfd_barrier(void *cookie){struct lfd_context*f=cookie;f->mem.barrier(f->mem.cookie);}
static int lfd_error(void *cookie){return ((struct lfd_context*)cookie)->io_error;}
static bool lfd_read_allowed(uint32_t off) {
    if(lpg_feature_read_allowed(off) || off==0x034 || off==0x020 || off==0x03c ||
        off==0x040 || off==0x044 || off==0x1000 || off==0x1010 || off==0x2000 || off==0x304 ||
       off==0xf00 || off==0xf04 || off==0xf08 || off==0xf0c)return true;
    if(off==0x140 || off==0x144 || off==0x150 || off==0x154 || off==0x160 || off==0x164)return true;
    for(unsigned i=0;i<3;i++) {
        uint32_t base=0x1800u+0x80u*i;
        if(off==base+0x00 || off==base+0x04 || off==base+0x08 || off==base+0x0c ||
           off==base+0x24 || off==base+0x40 || off==base+0x44 ||
           off==base+0x50 || off==base+0x54 || off==base+0x58 || off==base+0x60)return true;
    }
    return off==0x2400 || off==0x2404 || off==0x2408 || off==0x240c ||
        off==0x241c || off==0x2420 || off==0x2424 || off==0x2428 || off==0x2430 ||
        off==0x2434 || off==0x2438 || off==0x243c;
}
static bool lfd_write_allowed(struct lfd_context*f,uint32_t off,uint32_t value) {
    if(f->recovering) {
        /* Stop/reset/internal power-off only. No configuration, AS root or job. */
        switch(off) {
        case 0x030:if(value!=1)return false;break;
        case 0x028:case 0x1008:case 0x2008:case 0x024:case 0x1004:case 0x2004:
        case 0x1c0:case 0x1c4:case 0x1d0:case 0x1d4:case 0x1e0:case 0x1e4:break;
        default:return false;
        }
    }
    /* Job-slot writes are identical for every slot; DMA uses slot 1 and the
     * fragment clear uses slot 0. Values remain exact, never a free-form MMIO. */
    if(off>=0x1800u && off<0x1980u) {
        switch(off&0x7fu) {
        case 0x40:return value==LM_PRIVATE_JOB_VA ||
            value==LM_PRIVATE_JOB_VA+LM_CLEAR_DESC_OFFSET ||
            value==LM_PRIVATE_JOB_VA+LM_TRI_FRAG_JOB;
        case 0x44:return value==0;
        case 0x50:return value==3;
        case 0x54:return value==0;
        case 0x58:return value==LM_JOB_NEXT_CONFIG;
        case 0x60:return value==1;
        default:return false;
        }
    }
    switch(off) {
    case 0x030:return value==1 || value==8;
    case 0x028:case 0x1008:case 0x2008:return value==0;
    case 0x024:return value==0xffffffffu || value==0x81u || value==(1u<<17);
    case 0x1004:return value==0xffffffffu || value==0x20002u || value==0x10001u;
    case 0x2004:return value==0xffffffffu || value==0x10001u;
    case 0x180:return value==3;
    case 0x190:case 0x1a0:return value==1;
    case 0x184:case 0x194:case 0x1a4:return value==0;
    case 0x1c0:return value==3;
    case 0x1d0:case 0x1e0:return value==1;
    case 0x1c4:case 0x1d4:case 0x1e4:return value==0;
    case 0x304:return value==31;
    case 0xf00:case 0xf04:case 0xf08:case 0xf0c:return true; // Shared core RMW + readback.
    case 0x2400:
        return value==0 || (value>=f->pa+4096u && value<f->pa+(LFD_USABLE_LAST+1u)*4096u &&
            !(value&4095u) && (f->page_mask&(1u<<((value-f->pa)/4096u))));
    case 0x2404:case 0x240c:case 0x2414:case 0x2434:
        return value==(off==0x240c?0x4c8du:0u);
    case 0x2408:return value==0x888d8f88u;
    case 0x2410:return value==31;
    case 0x2418:return value==1 || value==2 || value==5;
    case 0x2430:return value==0x42000006u || value==1;
    default:return false;
    }
}
static uint32_t lfd_read(void *cookie,uint32_t off) {
    struct lfd_context*f=cookie;uint32_t value=0;
    if(f->io_error)return 0;
    if(!lfd_read_allowed(off)){f->io_error=LM_EINVAL;lfd_record_trace(f,true,off,0,LM_EINVAL);return 0;}
    int e=lpg_gpu_access_guard(f->platform);
    if(!e)e=lpg_transport_read(f->platform,lpg_base[3]+off,&value);
    f->gpu_reads++;
    if(!e && lpg_expired(f->platform))e=LM_ETIMEDOUT;
    if(e)f->io_error=e;
    lfd_record_trace(f,true,off,value,e);
    return value;
}
static void lfd_write(void *cookie,uint32_t off,uint32_t value) {
    struct lfd_context*f=cookie;
    if(f->io_error)return;
    if(!lfd_write_allowed(f,off,value)){f->io_error=LM_EINVAL;lfd_record_trace(f,false,off,value,LM_EINVAL);return;}
    int e=lpg_gpu_access_guard(f->platform);
    if(!e && (off==0xf00 || off==0xf04 || off==0xf08 || off==0xf0c)) {
        uint32_t old;
        e=lpg_transport_read(f->platform,lpg_base[3]+off,&old);f->gpu_reads++;
        if(!e) {
            uint32_t want=off==0xf04?old|(1u<<17):off==0xf08?old:
                off==0xf0c?old&~((7u<<12)|(7u<<15)):(old&~(0x3fu<<16))|(0xfu<<16);
            if(value!=want)e=LM_EINVAL;
        }
    }
    if(!e){e=lpg_transport_write(f->platform,lpg_base[3]+off,value);f->gpu_writes++;}
    if(!e && lpg_expired(f->platform))e=LM_ETIMEDOUT;
    if(e)f->io_error=e;
    lfd_record_trace(f,false,off,value,e);
}
static uint64_t lfd_now(void *cookie){return lpg_now(((struct lfd_context*)cookie)->platform);}
static void lfd_delay(void *cookie,uint32_t us){lpg_delay(((struct lfd_context*)cookie)->platform,us);}
static void lfd_phase(void*cookie,uint32_t phase){((struct lfd_context*)cookie)->phase=phase;}
static int lfd_recover_io(void*cookie) {
    struct lfd_context*f=cookie;
    if(f->io_error!=LM_ETIMEDOUT||f->recovery_attempted||f->platform->transport_error)return LM_EIO;
    f->recovery_attempted=true;
    int e=lpg_begin_recovery(f->platform);
    if(!e)e=lpg_gpu_access_guard(f->platform);
    if(e)return e;
    /* The first error remains in the receipt and the transaction stays failed.
     * A live MMIO guard plus a new stop-only budget is not itself reset proof. */
    f->recovering=true;f->io_error=0;return LM_OK;
}
static int lfd_run(void *cookie,struct lmp_state *state,const struct lpg_features *features,bool *safe) {
    struct lfd_context*f=cookie;
    if(!f || !safe || !f->allocated || !state || !state->gpu_ready ||
       !lpg_features_private_dma_candidate(features))return LM_ENOTSUP;
    *safe=false;f->state=state;
    f->transaction.clear_test=f->clear_mode;
    f->transaction.clear_chain=f->clear_chain;
    f->transaction.triangle_test=f->triangle_mode;
    f->transaction.clear_value=LM_CLEAR_VALUE;
    f->transaction.clear_value2=LM_CLEAR_VALUE2;
    f->transaction.tri_vertex_shader=lmg_tri_vertex_shader;
    f->transaction.tri_vertex_shader_bytes=LMG_TRI_VERTEX_SHADER_BYTES;
    f->transaction.tri_fragment_shader=lmg_tri_fragment_shader;
    f->transaction.tri_fragment_shader_bytes=LMG_TRI_FRAGMENT_SHADER_BYTES;
    struct lm_ops io={.cookie=f,.read32=lfd_read,.write32=lfd_write,.now_ns=lfd_now,.delay_us=lfd_delay,
        .alloc=lfd_alloc,.free=lfd_free,.page_alloc=lfd_page_alloc,.page_free=lfd_page_free,
        .for_device=lfd_for_device,.for_cpu=lfd_for_cpu,.barrier=lfd_barrier,.io_error=lfd_error,
        .recover_io=lfd_recover_io,.phase=lfd_phase};
    int e=lm_private_dma_run(&f->transaction,&io);
    if(f->recovery_attempted || f->recovering || f->io_error || f->first_io_error)
        f->transaction.expected_fault=false;
    f->safe_to_teardown=f->transaction.cleaned && !f->transaction.cleanup_error && !f->io_error &&
        !f->page_mask && !f->metadata_active;
    *safe=f->safe_to_teardown;
    if(!*safe)return e?e:LM_EBUSY;
    for(unsigned i=0;i<1024;i++)if(((uint32_t *)(f->arena+LFD_GUARD_PAGE*4096u))[i]!=(UINT32_C(0x61726e47)^i)) {
        f->transaction.expected_fault=false;return LM_EIO;
    }
    f->release_error=f->mem.release(f->mem.cookie,f->arena,f->pa,LFD_PAGES);
    if(f->release_error)f->transaction.expected_fault=false;
    if(!f->release_error){f->allocated=false;f->arena=NULL;}
    return e?e:f->release_error?f->release_error:f->first_io_error;
}
#endif
