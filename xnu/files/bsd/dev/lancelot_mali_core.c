/* Mali-G52 synchronous JM engine. See lancelot_mali_core.h for contracts.
 * Every timeout after hardware is given a root/job requires proved reset before
 * backing may be released. A failed reset poisons this device and quarantines VM.
 * MMIO offsets and AArch64 table format checked against supplied kbase r25p0.
 */
#include "lancelot_mali_core.h"
#ifdef LM_HOST_TEST
#include <string.h>
#elif defined(LM_FIRMWARE_CORE)
/* The freestanding loader provides memcpy/memset before including this file. */
#else
#include <libkern/libkern.h>
#endif
#define GPU_ID 0x000
#define GPU_IRQ_RAW 0x020
#define GPU_IRQ_CLR 0x024
#define GPU_IRQ_MASK 0x028
#define GPU_COMMAND 0x030
#define GPU_STATUS 0x034
#define GPU_FAULT 0x03c
#define GPU_FAULT_LO 0x040
#define GPU_FAULT_HI 0x044
#define JOB_RAW 0x1000
#define JOB_CLR 0x1004
#define JOB_MASK 0x1008
#define JOB_STATE 0x1010 /* current-active/next-busy snapshot on JOB_CLR */
#define JOB_STATE_MASK 0x00070007u
#define MMU_RAW 0x2000
#define MMU_CLR 0x2004
#define MMU_MASK 0x2008
#define AS_BASE 0x2400
#define SHADER_CONFIG 0xF04
#define TILER_CONFIG 0xF08
#define L2_MMU_CONFIG 0xF0C
#define JM_CONFIG 0xF00
#define COHERENCY_ENABLE 0x304
#define AS_CMD (AS_BASE + 0x18)
#define AS_STATUS (AS_BASE + 0x28)
#define JS_BASE(n) (0x1800u + (n) * 0x80u)
#define PA_MASK UINT64_C(0x000000fffffff000)
#define PTE_INVALID UINT64_C(2)
#define PTE_TABLE (UINT64_C(3) | (UINT64_C(1) << 10))
#define LM_MAX_TABLE_PAGES 1024u
#define LM_WAIT_NS UINT64_C(100000000)
struct lm_table { struct lm_page mem; struct lm_table *next; };
struct lm_mapping {
    uint64_t va; uint32_t pages, flags;
    struct lm_page *mem;
    struct lm_mapping *next;
};
struct lm_vm {
    struct lm_core *dev;
    struct lm_table *tables, *root;
    struct lm_mapping *maps;
    uint32_t mapped_pages, table_pages;
    bool quarantined, busy;
};
struct lm_core {
    struct lm_ops io;
    uint32_t id, js_present, as_present, props[80];
    uint64_t shaders, tilers, l2;
    struct lm_vm *bound;
    uint64_t sequence;
    uint32_t vm_count;
    bool ready, poisoned, terminal_offline, reset_initialized, recovery_only;
};
static void phase(struct lm_core*d,uint32_t p) {if(d->io.phase)d->io.phase(d->io.cookie,p);}
static int io_error(struct lm_core *d) { return d->io.io_error ? d->io.io_error(d->io.cookie) : 0; }
static uint32_t rr(struct lm_core *d, uint32_t r) { return d->io.read32(d->io.cookie, r); }
static void ww(struct lm_core *d, uint32_t r, uint32_t v) {
    d->io.write32(d->io.cookie, r, v); d->io.barrier(d->io.cookie);
}
static uint64_t rr64(struct lm_core *d, uint32_t r) {
    return (uint64_t)rr(d,r) | (uint64_t)rr(d,r+4)<<32;
}
static void ww64(struct lm_core *d, uint32_t r, uint64_t v) { ww(d,r,(uint32_t)v); ww(d,r+4,(uint32_t)(v>>32)); }
static int poll_bits(struct lm_core *d,uint32_t reg,uint32_t mask,uint32_t want,uint64_t ns) {
    uint64_t start=d->io.now_ns(d->io.cookie);
    /* Iteration cap also prevents hangs if the timer ceases advancing. */
    for(uint32_t loops=0;loops<100000;loops++) {
        if(io_error(d))return LM_EIO;
        if((rr(d,reg)&mask)==want) return io_error(d)?LM_EIO:LM_OK;
        if(d->io.now_ns(d->io.cookie)-start>=ns) break;
        d->io.delay_us(d->io.cookie,10);
    }
    return LM_ETIMEDOUT;
}
static bool runtime_fault(struct lm_core *d) {
    if(io_error(d))return true;
    uint32_t gpu=rr(d,GPU_IRQ_RAW),mmu=rr(d,MMU_RAW),job=rr(d,JOB_RAW);
    return io_error(d)||(gpu&0x81u)||(mmu&0x10001u)||(job&0x00070000u);
}
static int as_cmd(struct lm_core *d,uint32_t cmd) {
    int e=poll_bits(d,AS_STATUS,1,0,LM_WAIT_NS);
    if(e) return e;
    if(runtime_fault(d))return LM_EIO;
    ww(d,AS_CMD,cmd);
    e=poll_bits(d,AS_STATUS,1,0,LM_WAIT_NS);
    return e ? e : (runtime_fault(d)?LM_EIO:LM_OK);
}
static int caches(struct lm_core *d) {
    ww(d,GPU_IRQ_CLR,1u<<17); ww(d,GPU_COMMAND,8);
    int e=poll_bits(d,GPU_IRQ_RAW,1u<<17,1u<<17,LM_WAIT_NS);
    if(!e) ww(d,GPU_IRQ_CLR,1u<<17);
    return e ? e : (runtime_fault(d)?LM_EIO:LM_OK);
}
static int reset_raw(struct lm_core *d) {
    /* Caller must have platform power. Soft reset drains external bus interfaces. */
    ww(d,GPU_IRQ_MASK,0); ww(d,JOB_MASK,0); ww(d,MMU_MASK,0);
    ww(d,GPU_IRQ_CLR,0xffffffffu); ww(d,GPU_COMMAND,1);
    int e=poll_bits(d,GPU_IRQ_RAW,1u<<8,1u<<8,LM_WAIT_NS);
    if(!e) {
        ww(d,GPU_IRQ_CLR,0xffffffffu); ww(d,JOB_CLR,0xffffffffu); ww(d,MMU_CLR,0xffffffffu);
        if(rr(d,JOB_STATE)&JOB_STATE_MASK)e=LM_EIO;
        for(unsigned i=0;i<3;i++)
            if((rr(d,JS_BASE(i)+0x60)&7)!=0 || (rr(d,JS_BASE(i)+0x24)&0xff)==8) e=LM_EIO;
    }
    if(io_error(d))e=LM_EIO;
    if(e) { d->ready=false; d->poisoned=true; }
    d->bound=NULL;
    return e;
}
static int reset(struct lm_core*d) {
    if(io_error(d)) {
        if(!d->io.recover_io||d->io.recover_io(d->io.cookie)) {
            d->ready=false;d->poisoned=true;return LM_EIO;
        }
        d->recovery_only=true;
    }
    int e=reset_raw(d);
    if(e&&io_error(d)&&!d->recovery_only&&d->io.recover_io&&
        !d->io.recover_io(d->io.cookie)) {
        d->recovery_only=true;e=reset_raw(d);
    }
    if(!e){d->poisoned=false;d->reset_initialized=true;}
    return e;
}
static int core_power(struct lm_core *d,uint32_t present,uint32_t power,uint32_t ready) {
    uint64_t mask=rr64(d,present);
    ww64(d,power,mask);
    int e=poll_bits(d,ready,(uint32_t)mask,(uint32_t)mask,LM_WAIT_NS);
    return e ? e : poll_bits(d,ready+4,(uint32_t)(mask>>32),(uint32_t)(mask>>32),LM_WAIT_NS);
}
static int core_power_off(struct lm_core *d,uint32_t present,uint32_t power,uint32_t ready) {
    uint64_t mask=rr64(d,present);
    if(!mask)return LM_OK;
    ww64(d,power,mask);
    int e=poll_bits(d,ready,(uint32_t)mask,0,LM_WAIT_NS);
    return e ? e : poll_bits(d,ready+4,(uint32_t)(mask>>32),0,LM_WAIT_NS);
}
struct lm_core *lm_core_create(const struct lm_ops *io) {
    if(!io||!io->read32||!io->write32||!io->now_ns||!io->delay_us||!io->alloc||!io->free||
       !io->page_alloc||!io->page_free||!io->for_device||!io->for_cpu||!io->barrier) return NULL;
    struct lm_core *d=io->alloc(io->cookie,sizeof(*d));
    if(!d) return NULL;
    memset(d,0,sizeof(*d)); d->io=*io; return d;
}
bool lm_core_ready(const struct lm_core *d) {
    return d&&d->ready&&!d->poisoned&&!d->terminal_offline&&
        (!d->io.io_error||!d->io.io_error(d->io.cookie));
}
static int g52_configure(struct lm_core *d) {
    /* kbase_pm_driver.c:1735-1896, tGOx feature table, l2_mmu_config.c:70-72.
     * IDVS group default=0xf, TLS hashing enabled, 3-bit AXI limit AID32=0.
     * Coherency NONE=31: all CPU<->GPU transfers use explicit PoC maintenance. */
    if(rr(d,GPU_STATUS)&(1u<<7))return LM_ENOTSUP;
    if(io_error(d))return LM_EIO;
    uint32_t sc=rr(d,0xf04)|(1u<<17),tiler=rr(d,0xf08);
    uint32_t mmu=rr(d,L2_MMU_CONFIG)&~((7u<<12)|(7u<<15));
    uint32_t jm=(rr(d,0xf00)&~(0x3fu<<16))|(0xfu<<16);
    ww(d,0xf04,sc);ww(d,0xf08,tiler);ww(d,0xf0c,mmu);ww(d,0xf00,jm);ww(d,COHERENCY_ENABLE,31);
    if(io_error(d))return LM_EIO;
    if(rr(d,GPU_STATUS)&(1u<<7))return LM_EIO;
    if(rr(d,0xf04)!=sc||rr(d,0xf08)!=tiler||rr(d,0xf0c)!=mmu||rr(d,0xf00)!=jm||rr(d,COHERENCY_ENABLE)!=31||io_error(d))return LM_EIO;
    return LM_OK;
}
int lm_core_start(struct lm_core *d) {
    if(!d||d->poisoned||d->terminal_offline) return LM_ENODEV;
    if(d->ready) return LM_EBUSY;
    phase(d,LM_PHASE_ID);
    uint32_t id=rr(d,GPU_ID),product=id&UINT32_C(0xf00f0000);
    uint32_t revision=id&UINT32_C(0x0000ffff);
    if(io_error(d))return LM_EIO;
    /* GPU_ID2_PRODUCT_MODEL is arch_major[31:28] | product_major[19:16].
     * Hardware errata are known only for TGOx r0p0 and r1p0 in this source. */
    if(product!=UINT32_C(0x70020000)||
       (revision!=UINT32_C(0x00000000)&&revision!=UINT32_C(0x00001000)))return LM_ENOTSUP;
    uint32_t status=rr(d,GPU_STATUS);
    if(io_error(d))return LM_EIO;
    if(status&(1u<<7))return LM_ENOTSUP;
    d->id=id;
    phase(d,LM_PHASE_RESET);
    int e=reset(d); if(e) return e;
    if(d->recovery_only){d->terminal_offline=true;return LM_ETIMEDOUT;}
    d->reset_initialized=true;
    phase(d,LM_PHASE_CONFIG);e=g52_configure(d);
    if(!e){phase(d,LM_PHASE_L2_ON);e=core_power(d,0x120,0x1a0,0x160);}
    if(!e){phase(d,LM_PHASE_TILER_ON);e=core_power(d,0x110,0x190,0x150);}
    if(!e){phase(d,LM_PHASE_SHADER_ON);e=core_power(d,0x100,0x180,0x140);}
    if(e)goto start_fail;
    phase(d,LM_PHASE_FEATURES);
    d->shaders=rr64(d,0x100); d->tilers=rr64(d,0x110); d->l2=rr64(d,0x120);
    d->js_present=rr(d,0x01c); d->as_present=rr(d,0x018);
    if(io_error(d)||!d->shaders||!(d->as_present&1)||!(d->js_present&3)) {e=io_error(d)?LM_EIO:LM_ENOTSUP;goto start_fail;}
    /* Read-only hardware property cache; exact query numbers from panfrost_drm.h. */
    static const uint32_t regs[]={0x004,0x008,0x00c,0x010,0x014,0x0ac,0x0a0,0x0a4,0x0a8,0x300};
    for(unsigned i=0;i<10;i++) d->props[8+i]=rr(d,regs[i]);
    for(unsigned i=0;i<4;i++) d->props[18+i]=rr(d,0xb0+4*i);
    for(unsigned i=0;i<16;i++) d->props[22+i]=rr(d,0xc0+4*i);
    d->props[39]=rr(d,0x310);
    if(io_error(d)){e=LM_EIO;goto start_fail;}
    /* No frequency claim: platform DVFS not measured. AFBC is explicitly
     * reported unsupported (Panfrost UAPI defines zero as AFBC available). */
    d->ready=true; return LM_OK;
start_fail:
    phase(d,LM_PHASE_RESET);
    if(reset(d)!=LM_OK)d->poisoned=true;
    d->ready=false;d->terminal_offline=true;
    return e;
}
int lm_core_get_param(struct lm_core *d,uint32_t p,uint64_t *v) {
    if(!v||!lm_core_ready(d)) return LM_ENODEV;
    switch(p) {
    case 0: *v=d->id>>16; break;
    case 1: *v=d->id&0xffffu; break;
    case 2: *v=d->shaders; break;
    case 3: *v=d->tilers; break;
    case 4: *v=d->l2; break;
    case 5: *v=rr64(d,0xe00); break;
    case 6: *v=d->as_present; break;
    case 7: *v=d->js_present; break;
    case 38: *v=1; break; /* G52, one coherent core group */
    case 40: *v=1; break; /* driver-filtered AFBC unavailable: Mesa pan_props treats 0 as available */
    default: if((p>=8&&p<=37)||p==39) *v=d->props[p]; else return LM_EINVAL;
    }
    if(io_error(d))return LM_EIO;
    return LM_OK;
}
static struct lm_table *table_new(struct lm_vm *v) {
    struct lm_core *d=v->dev;
    if(v->table_pages>=LM_MAX_TABLE_PAGES) return NULL;
    struct lm_table *t=d->io.alloc(d->io.cookie,sizeof(*t));
    if(!t) return NULL;
    memset(t,0,sizeof(*t));
    if(d->io.page_alloc(d->io.cookie,&t->mem)||!t->mem.cpu||!t->mem.pa||
       (t->mem.pa&~PA_MASK)||(t->mem.pa&4095)) {
        if(t->mem.cpu) d->io.page_free(d->io.cookie,&t->mem);
        d->io.free(d->io.cookie,t,sizeof(*t)); return NULL;
    }
    for(unsigned i=0;i<512;i++) ((uint64_t*)t->mem.cpu)[i]=PTE_INVALID;
    t->next=v->tables; v->tables=t; v->table_pages++;
    return t;
}
static struct lm_table *table_find(struct lm_vm *v,uint64_t pa) {
    for(struct lm_table *t=v->tables;t;t=t->next) if(t->mem.pa==pa) return t;
    return NULL;
}
static uint64_t *walk(struct lm_vm *v,uint64_t va,bool make) {
    struct lm_table *t=v->root;
    for(unsigned level=0;level<3;level++) {
        uint64_t *e=&((uint64_t*)t->mem.cpu)[(va>>(39-9*level))&511];
        struct lm_table *n;
        if((*e&3)!=3) {
            if(!make || !(n=table_new(v))) return NULL;
            *e=n->mem.pa|PTE_TABLE;
        } else if(!(n=table_find(v,*e&PA_MASK))) return NULL;
        t=n;
    }
    return &((uint64_t*)t->mem.cpu)[(va>>12)&511];
}
/* Mutations are allowed only after AS0 has been detached (or a reset has
 * proved DMA stopped). Prune bottom-up and invalidate the parent before freeing
 * the table page. Keep the root for the lifetime of the client VM. This also
 * reclaims a partially built hierarchy when a map fails before its first leaf. */
static void prune_empty_path(struct lm_vm *v,uint64_t va) {
    struct lm_table *path[4]={v->root,NULL,NULL,NULL};
    uint64_t *parent[3]={NULL,NULL,NULL};
    unsigned depth=0;
    for(;depth<3;depth++) {
        uint64_t *e=&((uint64_t*)path[depth]->mem.cpu)[(va>>(39-9*depth))&511];
        if((*e&3)!=3)break;
        struct lm_table *child=table_find(v,*e&PA_MASK);
        if(!child)break;
        parent[depth]=e;path[depth+1]=child;
    }
    while(depth) {
        struct lm_table *t=path[depth];bool empty=true;
        for(unsigned i=0;i<512;i++)if(((uint64_t*)t->mem.cpu)[i]!=PTE_INVALID){empty=false;break;}
        if(!empty)break;
        *parent[depth-1]=PTE_INVALID;
        struct lm_table **link=&v->tables;
        while(*link&&*link!=t)link=&(*link)->next;
        if(!*link)break; /* Defensive: never free an unowned table. */
        *link=t->next;v->table_pages--;
        v->dev->io.page_free(v->dev->io.cookie,&t->mem);
        v->dev->io.free(v->dev->io.cookie,t,sizeof(*t));
        depth--;
    }
}
static bool range_valid(uint64_t va,uint32_t n) {
    return n&&n<=LM_MAX_MAP_PAGES&&!(va&4095)&&va>=LM_VA_MIN&&
           va<LM_VA_END&&((uint64_t)n<<12)<=LM_VA_END-va;
}
struct lm_vm *lm_core_vm_new(struct lm_core *d) {
    if(!lm_core_ready(d)||d->vm_count>=16) return NULL;
    struct lm_vm *v=d->io.alloc(d->io.cookie,sizeof(*v));
    if(!v) return NULL;
    memset(v,0,sizeof(*v)); v->dev=d;
    if(!(v->root=table_new(v))) {d->io.free(d->io.cookie,v,sizeof(*v));return NULL;}
    d->vm_count++; return v;
}
bool lm_core_vm_safe_to_free(const struct lm_vm *v) { return v&&!v->quarantined&&!v->busy; }
int lm_core_vm_map(struct lm_vm *v,uint64_t va,const struct lm_page *mem,uint32_t n,uint32_t flags) {
    if(!v||!mem||!range_valid(va,n)||(flags&~(LM_MAP_WRITE|LM_MAP_EXEC))) return LM_EINVAL;
    if(!lm_core_vm_safe_to_free(v)||v->dev->bound==v) return LM_EBUSY;
    if(n>LM_MAX_VM_PAGES-v->mapped_pages) return LM_ENOMEM;
    struct lm_core *d=v->dev;
    for(uint32_t i=0;i<n;i++) {
        if(!mem[i].cpu||!mem[i].pa||(mem[i].pa&~PA_MASK)||(mem[i].pa&4095)) return LM_EINVAL;
        uint64_t *e=walk(v,va+((uint64_t)i<<12),false);
        if(e&&(*e&3)==3) return LM_EBUSY;
    }
    struct lm_mapping *m=d->io.alloc(d->io.cookie,sizeof(*m));
    if(!m) return LM_ENOMEM;
    memset(m,0,sizeof(*m));
    m->mem=d->io.alloc(d->io.cookie,n*sizeof(*mem));
    if(!m->mem) {d->io.free(d->io.cookie,m,sizeof(*m));return LM_ENOMEM;}
    memcpy(m->mem,mem,n*sizeof(*mem)); m->va=va;m->pages=n;m->flags=flags;
    uint32_t i=0;
    for(;i<n;i++) {
        uint64_t *e=walk(v,va+((uint64_t)i<<12),true);
        if(!e) break;
        /* attr0 = writeback; stage1 access + explicit NX for data BO. */
        *e=mem[i].pa|3|(UINT64_C(1)<<10)|((flags&LM_MAP_WRITE)?(UINT64_C(1)<<6):(UINT64_C(3)<<6))|
           ((flags&LM_MAP_EXEC)?0:(UINT64_C(1)<<54));
    }
    if(i!=n) {
        uint32_t built=i;
        while(i) {i--;uint64_t *e=walk(v,va+((uint64_t)i<<12),false);if(e)*e=PTE_INVALID;}
        /* Include the failed leaf's path: walk(make) may have allocated some
         * parents before exhausting the page quota/allocator. */
        for(uint32_t j=0;j<=built;j++)prune_empty_path(v,va+((uint64_t)j<<12));
        d->io.free(d->io.cookie,m->mem,n*sizeof(*mem));d->io.free(d->io.cookie,m,sizeof(*m));return LM_ENOMEM;
    }
    m->next=v->maps;v->maps=m;v->mapped_pages+=n;return LM_OK;
}
int lm_core_vm_unmap(struct lm_vm *v,uint64_t va,uint32_t n) {
    if(!v||!range_valid(va,n)) return LM_EINVAL;
    if(!lm_core_vm_safe_to_free(v)||v->dev->bound==v) return LM_EBUSY;
    struct lm_mapping **link=&v->maps;
    while(*link&&((*link)->va!=va||(*link)->pages!=n)) link=&(*link)->next;
    if(!*link) return LM_EINVAL;
    struct lm_mapping *m=*link;
    for(uint32_t i=0;i<n;i++) {uint64_t *e=walk(v,va+((uint64_t)i<<12),false);if(e)*e=PTE_INVALID;}
    for(uint32_t i=0;i<n;i++)prune_empty_path(v,va+((uint64_t)i<<12));
    *link=m->next;v->mapped_pages-=n;
    struct lm_core *d=v->dev;
    d->io.free(d->io.cookie,m->mem,n*sizeof(*m->mem));d->io.free(d->io.cookie,m,sizeof(*m));
    return LM_OK;
}
uint64_t lm_core_vm_lookup(struct lm_vm *v,uint64_t va,uint32_t *flags) {
    if(!v||va<LM_VA_MIN||va>=LM_VA_END) return 0;
    uint64_t *e=walk(v,va,false);
    if(!e||(*e&3)!=3) return 0;
    if(flags) *flags=(((*e>>6)&3)==1?LM_MAP_WRITE:0)|((*e&(UINT64_C(1)<<54))?0:LM_MAP_EXEC);
    return (*e&PA_MASK)|(va&4095);
}
int lm_core_vm_destroy(struct lm_vm *v) {
    if(!v) return LM_EINVAL;
    if(!lm_core_vm_safe_to_free(v)||v->dev->bound==v) return LM_EBUSY;
    struct lm_core *d=v->dev;
    while(v->maps) {int e=lm_core_vm_unmap(v,v->maps->va,v->maps->pages);if(e)return e;}
    while(v->tables) {struct lm_table *t=v->tables;v->tables=t->next;
        d->io.page_free(d->io.cookie,&t->mem);d->io.free(d->io.cookie,t,sizeof(*t));}
    d->vm_count--;d->io.free(d->io.cookie,v,sizeof(*v));return LM_OK;
}
static int detach(struct lm_core *d) {
    /* Full VA invalidate before giving AS0 to another client. */
    ww64(d,AS_BASE+0x10,31); /* lock 4 GiB at address zero (size_log2 - 1) */
    int e=as_cmd(d,2);if(e)return e;
    if((e=as_cmd(d,5)))return e; /* FLUSH_MEM */
    ww64(d,AS_BASE+0x30,1);ww64(d,AS_BASE,0);
    if((e=as_cmd(d,1)))return e;
    /* An idle AS command bit does not prove the posted root/config writes
     * reached hardware. Keep backing unless disabled-AS readback agrees. */
    if(rr64(d,AS_BASE)!=0 || (rr(d,AS_BASE+0x30)&0xfu)!=1u || io_error(d))return LM_EIO;
    d->bound=NULL;return LM_OK;
}
static bool readback(struct lm_core *d,struct lm_job_result *r,uint32_t reg,uint64_t want,bool wide) {
    uint64_t got=wide?rr64(d,reg):rr(d,reg);
    if(got==want&&!io_error(d))return true;
    if(!r->mismatch_reg){r->mismatch_reg=reg;r->expected=want;r->observed=got;}
    return false;
}
static bool job_idle(struct lm_core*d,struct lm_job_result*r) {
    r->job_state=rr(d,JOB_STATE);
    if(!(r->job_state&JOB_STATE_MASK)&&!io_error(d))return true;
    if(!r->mismatch_reg){r->mismatch_reg=JOB_STATE;r->expected=0;r->observed=r->job_state;}
    return false;
}
static int bind_vm(struct lm_vm *v,struct lm_job_result *r) {
    struct lm_core *d=v->dev;
    for(struct lm_table *t=v->tables;t;t=t->next) d->io.for_device(d->io.cookie,t->mem.cpu,4096);
    for(struct lm_mapping *m=v->maps;m;m=m->next)
        for(uint32_t i=0;i<m->pages;i++) d->io.for_device(d->io.cookie,m->mem[i].cpu,4096);
    d->io.barrier(d->io.cookie);
    /* Unmapped between jobs, then update AARCH64_4K with WB page-table walks. */
    ww64(d,AS_BASE+0x30,6|(UINT64_C(2)<<24)|(UINT64_C(1)<<30));
    ww64(d,AS_BASE,v->root->mem.pa);
    ww64(d,AS_BASE+8,UINT64_C(0x4c8d888d8f88));
    d->bound=v;
    int e=as_cmd(d,1);if(e)return e;
    if(!readback(d,r,AS_BASE,v->root->mem.pa,true) ||
       !readback(d,r,AS_BASE+8,UINT64_C(0x4c8d888d8f88),true) ||
       !readback(d,r,AS_BASE+0x30,6|(UINT64_C(2)<<24)|(UINT64_C(1)<<30),true) ||
       io_error(d))return LM_EIO;
    return LM_OK;
}
int lm_core_submit_wait(struct lm_vm *v,uint64_t jc,uint32_t requirements,uint64_t timeout_ns,struct lm_job_result *r) {
    if(!v||!r||!jc||(jc&63)||requirements&~1u||!timeout_ns||timeout_ns>UINT64_C(10000000000))return LM_EINVAL;
    memset(r,0,sizeof(*r));
    struct lm_core *d=v->dev;
    if(!lm_core_ready(d)||!lm_core_vm_safe_to_free(v))return LM_ENODEV;
    if(d->bound)return LM_EBUSY;
    /* Job descriptors are data, executable shaders elsewhere have explicit EXEC. */
    uint32_t jc_flags=0,jc_end_flags=0;
    if(!lm_core_vm_lookup(v,jc,&jc_flags)||!lm_core_vm_lookup(v,jc+63,&jc_end_flags)||
       !(jc_flags&LM_MAP_WRITE)||!(jc_end_flags&LM_MAP_WRITE))return LM_EFAULT;
    uint32_t slot=(requirements&1)?0:1;
    if(!(d->js_present&(1u<<slot)))return LM_ENOTSUP;
    uint32_t slot_state=rr(d,JS_BASE(slot)+0x60);
    if(io_error(d))return LM_EIO;
    if(slot_state&7)return LM_EBUSY;
    v->busy=true;
    uint64_t started=d->io.now_ns(d->io.cookie);
    phase(d,LM_PHASE_BIND);r->failed_stage=LM_JOB_BIND;
    int e=bind_vm(v,r);
    if(!e) {
        r->failed_stage=LM_JOB_IRQ_CLEAR;
        ww(d,JOB_CLR,(1u<<slot)|(1u<<(16+slot)));ww(d,MMU_CLR,0x10001);
        ww(d,GPU_IRQ_CLR,0x81);
        if(runtime_fault(d) || (rr(d,JOB_RAW)&((1u<<slot)|(1u<<(16+slot)))) || !job_idle(d,r))e=LM_EIO;
    }
    if(!e) {
        phase(d,LM_PHASE_NEXT);r->failed_stage=LM_JOB_NEXT;
        ww64(d,JS_BASE(slot)+0x40,jc);ww64(d,JS_BASE(slot)+0x50,d->shaders);
        ww(d,JS_BASE(slot)+0x58,LM_JOB_NEXT_CONFIG);
        if(!readback(d,r,JS_BASE(slot)+0x40,jc,true) || !readback(d,r,JS_BASE(slot)+0x50,d->shaders,true) ||
           !readback(d,r,JS_BASE(slot)+0x58,LM_JOB_NEXT_CONFIG,false) || io_error(d))e=LM_EIO;
    }
    if(!e) {
        phase(d,LM_PHASE_WAIT);r->failed_stage=LM_JOB_WAIT;
        d->io.barrier(d->io.cookie);ww(d,JS_BASE(slot)+0x60,1);
        r->sequence=++d->sequence;
        uint32_t loops=0;
        for(;;) {
            r->job_irq=rr(d,JOB_RAW);
            r->mmu_fault=rr(d,MMU_RAW)&0x10001;
            r->gpu_fault=rr(d,GPU_IRQ_RAW)&0x81;
            if(io_error(d)||r->mmu_fault||r->gpu_fault||(r->job_irq&(1u<<(16+slot)))) {e=LM_EIO;break;}
            if(r->job_irq&(1u<<slot))break;
            if(d->io.now_ns(d->io.cookie)-started>=timeout_ns||++loops>=1000000) {e=LM_ETIMEDOUT;break;}
            d->io.delay_us(d->io.cookie,10);
        }
        r->job_status=rr(d,JS_BASE(slot)+0x24);
        if(!e&&(io_error(d)||(r->job_status&0xff)>1 ||
                 (rr(d,JS_BASE(slot)+0x60)&7u)))e=LM_EIO;
        if(!e) {
            /* NEXT can be empty while CURRENT still runs. Refresh the
             * hardware snapshot, then prove both idle before cache/detach. */
            ww(d,JOB_CLR,(1u<<slot)|(1u<<(16+slot)));
            if(!job_idle(d,r)||runtime_fault(d))e=LM_EIO;
        }
    }
    if(!e){phase(d,LM_PHASE_CACHE);r->failed_stage=LM_JOB_CACHE;e=caches(d);}
    if(!e){phase(d,LM_PHASE_DETACH);r->failed_stage=LM_JOB_DETACH;e=detach(d);}
    if(!e) {
        r->failed_stage=LM_JOB_ACK;
        ww(d,JOB_CLR,(1u<<slot)|(1u<<(16+slot)));
        if(runtime_fault(d)||!job_idle(d,r))e=LM_EIO;
    }
    if(e) {
        r->mmu_fault=rr(d,MMU_RAW)&0x10001u;r->gpu_fault=rr(d,GPU_IRQ_RAW)&0x81u;
        if(r->mmu_fault) {r->fault_address=rr64(d,AS_BASE+0x20);r->mmu_fault=rr(d,AS_BASE+0x1c);}
        else if(r->gpu_fault) {r->fault_address=rr64(d,GPU_FAULT_LO);r->gpu_fault=rr(d,GPU_FAULT);}
        phase(d,LM_PHASE_RESET);r->reset_completed=reset(d)==LM_OK;
        if(!r->reset_completed) {v->quarantined=true;r->quarantined=true;}
        /* Successful reset proves DMA stopped, but internal cores were reset.
         * Keep driver offline for remainder of boot; do not silently reuse context. */
        d->ready=false;d->terminal_offline=true;
    }
    if(!v->quarantined) {
        for(struct lm_mapping *m=v->maps;m;m=m->next) if(m->flags&LM_MAP_WRITE)
            for(uint32_t i=0;i<m->pages;i++)d->io.for_cpu(d->io.cookie,m->mem[i].cpu,4096);
        d->io.barrier(d->io.cookie);
    }
    r->elapsed_ns=d->io.now_ns(d->io.cookie)-started;
    if(!e)r->failed_stage=LM_JOB_NONE;
    v->busy=false;return e;
}
int lm_core_shutdown(struct lm_core *d) {
    if(!d||d->vm_count||d->bound)return LM_EBUSY;
    if(!d->reset_initialized)return LM_OK;
    if(d->poisoned)return LM_EIO;
    /* No outstanding VM or jobs remain at this point. Drain caches before
     * powering down present cores; the subsequent reset proves bus/job idle. */
    /* A prior fault path has already completed reset and cache-drain proof;
     * do not repeat a cache command against an intentionally faulted fixture. */
    phase(d,LM_PHASE_OFF);
    int e=d->terminal_offline ? LM_OK : caches(d);
    if(!e)e=core_power_off(d,0x100,0x1c0,0x140);
    if(!e)e=core_power_off(d,0x110,0x1d0,0x150);
    if(!e)e=core_power_off(d,0x120,0x1e0,0x160);
    if(e)goto shutdown_failure;
    phase(d,LM_PHASE_FINAL_RESET);e=reset(d);if(e)return e;
    /* Reset must not leave an internal core READY after our PWROFF proof.
     * Verify the final state before freeing the engine/closing outer MFG. */
    if(rr64(d,0x140)||rr64(d,0x150)||rr64(d,0x160)||io_error(d)) {
        e=LM_EIO;goto shutdown_failure;
    }
    d->ready=false;d->terminal_offline=true;return LM_OK;
shutdown_failure:
    d->poisoned=true;d->ready=false;
    /* Renew only a provider-proved diagnostic deadline, once. A physical
     * timeout or transport failure without that proof stays poisoned. */
    if(!d->recovery_only&&io_error(d)&&d->io.recover_io&&!reset(d)) {
        d->terminal_offline=true;return lm_core_shutdown(d);
    }
    return e;
}
int lm_core_destroy(struct lm_core *d) {
    if(!d)return LM_EINVAL;
    if(d->vm_count||d->bound)return LM_EBUSY;
    if(d->poisoned||io_error(d)) {
        if(!io_error(d)||!d->io.recover_io)return LM_EBUSY;
        phase(d,LM_PHASE_RESET);int e=reset(d);if(e)return e;
        d->terminal_offline=true;
    }
    int e=lm_core_shutdown(d);if(e)return e;
    struct lm_ops io=d->io;io.free(io.cookie,d,sizeof(*d));return LM_OK;
}
