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
#define PTE_GPU_INNER_SHARED (UINT64_C(3) << 8)
#define LM_MAX_TABLE_PAGES 1024u
#define LM_WAIT_NS UINT64_C(100000000)
struct lm_table {
    struct lm_page mem;
    struct lm_table *next;
    /* Leaf-only metadata. Parent lives until this leaf is pruned. */
    uint64_t *parent_entry;
    struct lm_table *parent;
    bool scope_selected, scope_hidden;
    bool dirty; /* CPU-only page-table writes; GPU never writes these pages. */
};
struct lm_mapping {
    uint64_t va; uint32_t pages, flags;
    struct lm_page *mem;
    struct lm_mapping *next;
    bool selected;
    bool gpu_only, device_initialized, readonly;
};
struct lm_vm {
    struct lm_core *dev;
    struct lm_vm *next_vm;
    struct lm_table *tables, *root;
    /* Supported VA is below 4GiB: one slot per 2MiB leaf region. This is
     * metadata only; hardware still walks the actual four-level hierarchy. */
    struct lm_table *leaves[LM_VA_END >> 21];
    struct lm_mapping *maps;
    uint32_t mapped_pages, table_pages;
    bool quarantined, busy;
    bool scope_active;
    uint32_t active_pages;
    /* Retained PA multiset, lazily built for scoped submissions. At most
     * 2*LM_MAX_VM_PAGES slots (1MiB). High bits store CPU-only refcounts. */
    uint64_t *pa_index;
    uint32_t pa_capacity, alias_keys;
};
struct lm_core {
    struct lm_ops io;
    uint32_t id, js_present, as_present, props[80];
    uint64_t shaders, tilers, l2;
    struct lm_vm *bound;
    struct lm_vm *vms;
    uint64_t sequence;
    uint32_t vm_count;
    uint32_t pre_job_dumps;
    uint32_t table_levels, transcfg;
    struct lm_chain_snapshot *chain;
    struct lm_poll_counts control_wait;
    bool ready, poisoned, terminal_offline, reset_initialized, recovery_only;
    bool external_unsafe, dvfs_as_prepared;
};
static void phase(struct lm_core*d,uint32_t p) {if(d->io.phase)d->io.phase(d->io.cookie,p);}
static uint64_t profile_now(struct lm_core*d) {return d->io.profile_jobs?d->io.now_ns(d->io.cookie):0;}
static int io_error(struct lm_core *d) { return d->io.io_error ? d->io.io_error(d->io.cookie) : 0; }
static uint32_t rr(struct lm_core *d, uint32_t r) { return d->io.read32(d->io.cookie, r); }
static void ww(struct lm_core *d, uint32_t r, uint32_t v) {
    d->io.write32(d->io.cookie, r, v); d->io.barrier(d->io.cookie);
}
static uint64_t rr64(struct lm_core *d, uint32_t r) {
    return (uint64_t)rr(d,r) | (uint64_t)rr(d,r+4)<<32;
}
static void ww64(struct lm_core *d, uint32_t r, uint64_t v) { ww(d,r,(uint32_t)v); ww(d,r+4,(uint32_t)(v>>32)); }
/* Short handshakes should not enter the scheduler for every requested10us.
 * The budget is local to one wait, capped by elapsed time and requested spin
 * time independently. After it expires, sleep50us rather than create a10us
 * timer/wakeup storm. Neither path assumes the hardware completed. */
struct lm_poll_state {uint64_t start;uint32_t budget_us,used_us;};
static int poll_pause(struct lm_core *d,struct lm_poll_state *p,uint64_t now,
                      uint64_t remaining,struct lm_poll_counts *counts) {
    bool spin=d->io.spin_us && now>=p->start && now-p->start<(uint64_t)p->budget_us*1000 && p->used_us<p->budget_us;
    uint32_t us=spin||!d->io.spin_us?10u:50u;
    if(spin&&us>p->budget_us-p->used_us)us=p->budget_us-p->used_us;
    if(spin) {
        uint64_t budget_left=(uint64_t)p->budget_us*1000-(now-p->start);
        if(budget_left<(uint64_t)us*1000)us=(uint32_t)((budget_left+999)/1000);
    }
    /* At most1us rounding at the final sub-microsecond boundary. */
    if(d->io.spin_us&&remaining<(uint64_t)us*1000)us=(uint32_t)((remaining+999)/1000);
    if(!us)return LM_ETIMEDOUT;
    uint64_t before=profile_now(d);int e=LM_OK;
    if(spin){p->used_us+=us;e=d->io.spin_us(d->io.cookie,us);}
    else d->io.delay_us(d->io.cookie,us);
    if(d->io.profile_jobs) {
        uint64_t elapsed=profile_now(d)-before;
        if(spin){counts->spins++;counts->spin_ns+=elapsed;}
        else {counts->sleeps++;counts->sleep_ns+=elapsed;}
    }
    return e;
}
static int poll_bits(struct lm_core *d,uint32_t reg,uint32_t mask,uint32_t want,uint64_t ns) {
    uint64_t start=d->io.now_ns(d->io.cookie);
    struct lm_poll_state pause={.start=start,.budget_us=50};
    /* Iteration cap also prevents hangs if the timer ceases advancing. */
    for(uint32_t loops=0;loops<100000;loops++) {
        if(io_error(d))return LM_EIO;
        if((rr(d,reg)&mask)==want) return io_error(d)?LM_EIO:LM_OK;
        uint64_t now=d->io.now_ns(d->io.cookie);
        if(now-start>=ns) break;
        int e=poll_pause(d,&pause,now,ns-(now-start),&d->control_wait);
        if(e)return e;
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
       !io->page_alloc||!io->page_free||!io->for_device||!io->for_cpu||!io->barrier||io->pre_as_flush>2||
       (io->gpu_only_sync&&!io->submit_scope)) return NULL;
    struct lm_core *d=io->alloc(io->cookie,sizeof(*d));
    if(!d) return NULL;
    memset(d,0,sizeof(*d)); d->io=*io;
    d->table_levels=4;d->transcfg=6|(UINT32_C(2)<<24)|(UINT32_C(1)<<30);
    if(io->capture_descriptors) {
        d->chain=io->alloc(io->cookie,sizeof(*d->chain));
        if(!d->chain){io->free(io->cookie,d,sizeof(*d));return NULL;}
    }
    return d;
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
    if(d->io.g52_walk_nc) {
        if((d->id>>16)!=0x7402 || d->props[12]!=0x2823) {e=LM_ENOTSUP;goto start_fail;}
        /* hakios-mt6768 redwood_mali.c TRANSCFG_WALK_NC: T0SZ=55-35.
         * The top table starts at bit 30, followed by 21 and 12. */
        d->table_levels=3;d->transcfg=6|(20u<<6)|(1u<<24);
    }
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
    t->dirty=true;
    t->next=v->tables; v->tables=t; v->table_pages++;
    return t;
}
static struct lm_table *table_find(struct lm_vm *v,uint64_t pa) {
    for(struct lm_table *t=v->tables;t;t=t->next) if(t->mem.pa==pa) return t;
    return NULL;
}
static void table_store(struct lm_table *t,uint64_t *entry,uint64_t value) {
    if(*entry!=value) {*entry=value;t->dirty=true;}
}
static uint64_t *walk(struct lm_vm *v,uint64_t va,bool make) {
    struct lm_table *t=v->root;
    for(unsigned level=0;level<v->dev->table_levels-1;level++) {
        unsigned shift=12+9*(v->dev->table_levels-1-level);
        uint64_t *e=&((uint64_t*)t->mem.cpu)[(va>>shift)&511];
        struct lm_table *n;
        if((*e&3)!=3) {
            if(!make || !(n=table_new(v))) return NULL;
            table_store(t,e,n->mem.pa|PTE_TABLE);
            if(level==v->dev->table_levels-2) {n->parent_entry=e;n->parent=t;v->leaves[va>>21]=n;}
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
    for(;depth<v->dev->table_levels-1;depth++) {
        unsigned shift=12+9*(v->dev->table_levels-1-depth);
        uint64_t *e=&((uint64_t*)path[depth]->mem.cpu)[(va>>shift)&511];
        if((*e&3)!=3)break;
        struct lm_table *child=table_find(v,*e&PA_MASK);
        if(!child)break;
        parent[depth]=e;path[depth+1]=child;
    }
    while(depth) {
        struct lm_table *t=path[depth];bool empty=true;
        for(unsigned i=0;i<512;i++)if(((uint64_t*)t->mem.cpu)[i]!=PTE_INVALID){empty=false;break;}
        if(!empty)break;
        table_store(path[depth-1],parent[depth-1],PTE_INVALID);
        struct lm_table **link=&v->tables;
        while(*link&&*link!=t)link=&(*link)->next;
        if(!*link)break; /* Defensive: never free an unowned table. */
        *link=t->next;v->table_pages--;
        if(depth==v->dev->table_levels-1)v->leaves[va>>21]=NULL;
        v->dev->io.page_free(v->dev->io.cookie,&t->mem);
        v->dev->io.free(v->dev->io.cookie,t,sizeof(*t));
        depth--;
    }
}
static bool range_valid(uint64_t va,uint32_t n) {
    return n&&n<=LM_MAX_MAP_PAGES&&!(va&4095)&&va>=LM_VA_MIN&&
           va<LM_VA_END&&((uint64_t)n<<12)<=LM_VA_END-va;
}
/* All entries in this range have already been cleared. An empty-path test is
 * a property of a 2MiB leaf, not each of its 512 pages. Visit each affected
 * leaf once, including a partially allocated final path during map rollback.
 * The last emptied child still recursively reclaims its empty parents. */
static void prune_empty_range(struct lm_vm *v,uint64_t va,uint32_t pages) {
    uint64_t end=va+((uint64_t)pages<<12);
    while(va<end) {
        prune_empty_path(v,va);
        va=(va+UINT64_C(0x200000))&~UINT64_C(0x1fffff);
    }
}
struct lm_vm *lm_core_vm_new(struct lm_core *d) {
    if(!lm_core_ready(d)||d->vm_count>=16) return NULL;
    struct lm_vm *v=d->io.alloc(d->io.cookie,sizeof(*v));
    if(!v) return NULL;
    memset(v,0,sizeof(*v)); v->dev=d;
    if(!(v->root=table_new(v))) {d->io.free(d->io.cookie,v,sizeof(*v));return NULL;}
    v->next_vm=d->vms;d->vms=v;d->vm_count++; return v;
}
bool lm_core_vm_safe_to_free(const struct lm_vm *v) {
    return v&&!v->quarantined&&!v->busy&&!v->dev->external_unsafe;
}
static void pa_index_add_mapping(struct lm_vm *,const struct lm_mapping *);
static void pa_index_remove_mapping(struct lm_vm *,const struct lm_mapping *);
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
        /* attr0=0x88 selects GPU WB / Midgard-inner shareability semantics.
         * Panfrost uses GPU-local sharing for its cached BOs, including
         * vertex outputs consumed by tiler/fragment units. CPU coherency
         * remains disabled and still needs the CPU maintenance callbacks. */
        table_store(v->leaves[(va+((uint64_t)i<<12))>>21],e,
            mem[i].pa|3|(UINT64_C(1)<<10)|((flags&LM_MAP_WRITE)?(UINT64_C(1)<<6):(UINT64_C(3)<<6))|
            (d->io.gpu_shareable?PTE_GPU_INNER_SHARED:0)|
            ((flags&LM_MAP_EXEC)?0:(UINT64_C(1)<<54)));
    }
    if(i!=n) {
        uint32_t built=i;
        while(i) {i--;uint64_t at=va+((uint64_t)i<<12);uint64_t *e=walk(v,at,false);
            if(e)table_store(v->leaves[at>>21],e,PTE_INVALID);}
        /* Include the failed leaf's path: walk(make) may have allocated some
         * parents before exhausting the page quota/allocator. */
        prune_empty_range(v,va,built+1);
        d->io.free(d->io.cookie,m->mem,n*sizeof(*mem));d->io.free(d->io.cookie,m,sizeof(*m));return LM_ENOMEM;
    }
    m->next=v->maps;v->maps=m;v->mapped_pages+=n;pa_index_add_mapping(v,m);return LM_OK;
}
/* Recreate only the exact attributes used at map time. No CPU coherency claim. */
static uint64_t mapping_pte(struct lm_vm *v,struct lm_mapping *m,uint32_t i) {
    return m->mem[i].pa|3|(UINT64_C(1)<<10)|((m->flags&LM_MAP_WRITE)?(UINT64_C(1)<<6):(UINT64_C(3)<<6))|
           (v->dev->io.gpu_shareable?PTE_GPU_INNER_SHARED:0)|
           ((m->flags&LM_MAP_EXEC)?0:(UINT64_C(1)<<54));
}
/* All valid PA bits are below bit40. Refcounts need at most 17 bits for the
 * 65536-page VM cap. This index is metadata, never a GPU page table. */
#define PA_INDEX_REF UINT64_C(0x10000000000)
static uint32_t pa_index_slot(struct lm_vm *v,uint64_t pa) {
    uint32_t slot=(uint32_t)((pa>>12)*UINT64_C(11400714819323198485))&(v->pa_capacity-1);
    while(v->pa_index[slot]&&(v->pa_index[slot]&PA_MASK)!=pa)slot=(slot+1)&(v->pa_capacity-1);
    return slot;
}
static void pa_index_discard(struct lm_vm *v) {
    if(v->pa_index)v->dev->io.free(v->dev->io.cookie,v->pa_index,(size_t)v->pa_capacity*sizeof(*v->pa_index));
    v->pa_index=NULL;v->pa_capacity=0;v->alias_keys=0;
}
static void pa_index_add_mapping(struct lm_vm *v,const struct lm_mapping *m) {
    if(!v->pa_index)return;
    /* Growth invalidates the cache, not the successful mapping. The next
     * submit rebuilds it; allocation failure still selects full-VM mode3. */
    if(v->mapped_pages>v->pa_capacity/2) {pa_index_discard(v);return;}
    for(uint32_t i=0;i<m->pages;i++) {
        uint64_t pa=m->mem[i].pa;uint32_t slot=pa_index_slot(v,pa);
        uint64_t refs=v->pa_index[slot]>>40;
        if(refs==1)v->alias_keys++;
        v->pa_index[slot]=pa|((refs+1)<<40);
    }
}
static void pa_index_remove_mapping(struct lm_vm *v,const struct lm_mapping *m) {
    if(!v->pa_index)return;
    for(uint32_t i=0;i<m->pages;i++) {
        uint32_t slot=pa_index_slot(v,m->mem[i].pa);
        uint64_t entry=v->pa_index[slot],refs=entry>>40;
        if(!refs) {pa_index_discard(v);return;} /* Unknown state never proves no alias. */
        if(refs==2)v->alias_keys--;
        if(refs>1) {v->pa_index[slot]-=PA_INDEX_REF;continue;}
        v->pa_index[slot]=0;
        /* Close the probe-chain hole. No tombstones can accumulate under BO
         * churn; reinsertion keeps each entry's exact refcount/alias state. */
        for(uint32_t next=(slot+1)&(v->pa_capacity-1);v->pa_index[next];next=(next+1)&(v->pa_capacity-1)) {
            entry=v->pa_index[next];v->pa_index[next]=0;
            v->pa_index[pa_index_slot(v,entry&PA_MASK)]=entry;
        }
    }
}
/* A repeated PA (also within one BO) still requires full-VM maintenance and
 * visibility. Incremental updates avoid rescanning unchanged mappings after
 * each map/unmap; no CPU alias is exempted by its position in this index. */
static uint32_t scope_alias_mode(struct lm_vm *v) {
    if(v->pa_index)return v->alias_keys?2u:1u;
    uint32_t capacity=2;
    while(capacity<2*v->mapped_pages)capacity<<=1;
    struct lm_core *d=v->dev;
    v->pa_index=d->io.alloc(d->io.cookie,(size_t)capacity*sizeof(*v->pa_index));
    if(!v->pa_index)return 3;
    v->pa_capacity=capacity;v->alias_keys=0;
    memset(v->pa_index,0,(size_t)capacity*sizeof(*v->pa_index));
    for(struct lm_mapping *m=v->maps;m;m=m->next)pa_index_add_mapping(v,m);
    return v->alias_keys?2u:1u;
}
/* Mask a wholly inactive 2MiB leaf at its parent instead of rewriting up to
 * 512 PTEs twice. Only mixed leaves need per-entry masking. No pages become
 * reachable outside the BO list. Parent/leaf pages are still cleaned before
 * root publication, and detach/reset proof still precedes restoration. */
static bool scope_visible(struct lm_vm *v,bool visible,uint32_t *parents,uint32_t *entries) {
    if(!visible) {
        for(struct lm_table *t=v->tables;t;t=t->next)t->scope_selected=false;
        for(struct lm_mapping *m=v->maps;m;m=m->next)if(m->selected) {
            uint64_t end=m->va+((uint64_t)m->pages<<12);
            for(uint64_t region=m->va>>21;region<=((end-1)>>21);region++) {
                struct lm_table *t=v->leaves[region];
                if(!t||!t->parent_entry)return false;
                t->scope_selected=true;
            }
        }
    } else {
        for(struct lm_table *t=v->tables;t;t=t->next)if(t->scope_hidden) {
            if(!t->parent)return false;
            table_store(t->parent,t->parent_entry,t->mem.pa|PTE_TABLE);t->scope_hidden=false;
        }
    }
    for(struct lm_mapping *m=v->maps;m;m=m->next)if(m->selected&&m->readonly&&(m->flags&LM_MAP_WRITE)) {
        for(uint32_t i=0;i<m->pages;) {
            uint64_t va=m->va+((uint64_t)i<<12);
            struct lm_table *t=v->leaves[va>>21];
            if(!t||!t->parent_entry)return false;
            uint32_t n=512-(uint32_t)((va>>12)&511);
            if(n>m->pages-i)n=m->pages-i;
            uint64_t *entry=&((uint64_t*)t->mem.cpu)[(va>>12)&511];
            /* AArch64 AP[2:1]=11: GPU read-only. Preserve PA, XN, attr, SH. */
            for(uint32_t j=0;j<n;j++) {
                uint64_t pte=mapping_pte(v,m,i+j);
                table_store(t,entry+j,visible?pte:pte|(UINT64_C(3)<<6));
            }
            i+=n;
        }
    }
    for(struct lm_mapping *m=v->maps;m;m=m->next)if(!m->selected) {
        for(uint32_t i=0;i<m->pages;) {
            uint64_t va=m->va+((uint64_t)i<<12);
            struct lm_table *t=v->leaves[va>>21];
            if(!t||!t->parent_entry)return false;
            uint32_t n=512-(uint32_t)((va>>12)&511);
            if(n>m->pages-i)n=m->pages-i;
            if(t->scope_selected) {
                uint64_t *e=&((uint64_t*)t->mem.cpu)[(va>>12)&511];
                for(uint32_t j=0;j<n;j++)table_store(t,e+j,visible?mapping_pte(v,m,i+j):PTE_INVALID);
                if(entries)*entries+=n;
            }
            i+=n;
        }
    }
    if(!visible)for(struct lm_table *t=v->tables;t;t=t->next)
        if(t->parent_entry&&!t->scope_selected) {
            if(!t->parent)return false;
            table_store(t->parent,t->parent_entry,PTE_INVALID);t->scope_hidden=true;
            if(parents)(*parents)++;
        }
    return true;
}
int lm_core_vm_unmap(struct lm_vm *v,uint64_t va,uint32_t n) {
    if(!v||!range_valid(va,n)) return LM_EINVAL;
    if(!lm_core_vm_safe_to_free(v)||v->dev->bound==v) return LM_EBUSY;
    struct lm_mapping **link=&v->maps;
    while(*link&&((*link)->va!=va||(*link)->pages!=n)) link=&(*link)->next;
    if(!*link) return LM_EINVAL;
    struct lm_mapping *m=*link;
    pa_index_remove_mapping(v,m);
    for(uint32_t i=0;i<n;i++) {uint64_t at=va+((uint64_t)i<<12);uint64_t *e=walk(v,at,false);
        if(e)table_store(v->leaves[at>>21],e,PTE_INVALID);}
    prune_empty_range(v,va,n);
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
    struct lm_vm **link=&d->vms;
    while(*link&&*link!=v)link=&(*link)->next_vm;
    if(!*link)return LM_EINVAL;
    while(v->maps) {int e=lm_core_vm_unmap(v,v->maps->va,v->maps->pages);if(e)return e;}
    pa_index_discard(v);
    while(v->tables) {struct lm_table *t=v->tables;v->tables=t->next;
        d->io.page_free(d->io.cookie,&t->mem);d->io.free(d->io.cookie,t,sizeof(*t));}
    *link=v->next_vm;d->vm_count--;d->io.free(d->io.cookie,v,sizeof(*v));return LM_OK;
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
/* Called only on a retained, busy VM after the fault address/status was read,
 * before reset can detach the root and scope restoration can hide the cause.
 * Inspect page-table/BO metadata only; no GPU-buffer reads or allocations. */
static void fault_vm_snapshot(struct lm_vm *v,struct lm_job_result *r) {
    if(!r->mmu_fault)return;
    uint64_t va=r->fault_address;
    r->fault_vm.valid=true;r->fault_vm.scoped=v->scope_active;
    if(va<LM_VA_MIN||va>=LM_VA_END)return;
    uint64_t *translated=walk(v,va,false);
    r->fault_vm.visible=translated&&((*translated&3)==3);
    struct lm_table *leaf=v->leaves[va>>21];
    if(leaf)r->fault_vm.leaf_pte=((uint64_t*)leaf->mem.cpu)[(va>>12)&511];
    for(struct lm_mapping *m=v->maps;m;m=m->next) {
        if(va<m->va||va-m->va>=((uint64_t)m->pages<<12))continue;
        uint32_t page=(uint32_t)((va-m->va)>>12);
        r->fault_vm.mapped=true;r->fault_vm.selected=m->selected;
        r->fault_vm.va=m->va;r->fault_vm.pages=m->pages;r->fault_vm.flags=m->flags;
        r->fault_vm.page_pa=m->mem[page].pa;r->fault_vm.expected_pte=mapping_pte(v,m,page);
        break;
    }
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
/* A DVFS failure is outside the core's normal MMIO/reset contract. Neither a
 * stale ready bit nor a VM's local busy bit proves external DMA stopped. */
void lm_core_external_revoke(struct lm_core *d) {
    if(!d)return;
    d->external_unsafe=true;d->ready=false;d->poisoned=true;
    d->terminal_offline=true;d->dvfs_as_prepared=false;
}
static int dvfs_idle_snapshot(struct lm_core *d,struct lm_job_result *r) {
    if(!d||!r)return LM_EINVAL;
    memset(r,0,sizeof(*r));r->failed_stage=LM_JOB_DVFS_GUARD;
    if(!lm_core_ready(d)||d->external_unsafe||d->recovery_only||!d->reset_initialized)
        return LM_ENODEV;
    if(d->bound)return LM_EBUSY;
    if(!d->js_present||(d->js_present&~7u)||!d->as_present||(d->as_present&~0xffffu))
        return LM_ENOTSUP;
    r->gpu_fault=rr(d,GPU_IRQ_RAW)&0x81u;
    r->mmu_fault=rr(d,MMU_RAW)&(d->as_present|(d->as_present<<16));
    r->job_irq=rr(d,JOB_RAW);
    if(io_error(d)||r->gpu_fault||r->mmu_fault||(r->job_irq&(d->js_present<<16)))
        return LM_EIO; /* Preserve pending faults; never clear unknown IRQs. */
    /* JOB_CLR(0) refreshes CURRENT/NEXT without acknowledging any job IRQ. */
    ww(d,JOB_CLR,0);
    /* With no software-bound VM, hardware activity is UNKNOWN DMA/ownership,
     * not a benign busy job that may be ignored while releasing backing. */
    if(!job_idle(d,r))return LM_EIO;
    for(unsigned i=0;i<3;i++)if(d->js_present&(1u<<i)) {
        uint32_t next=rr(d,JS_BASE(i)+0x60),status=rr(d,JS_BASE(i)+0x24);
        if(io_error(d))return LM_EIO;
        if((next&7u)||(status&0xffu)==8u) {
            r->mismatch_reg=JS_BASE(i)+((next&7u)?0x60:0x24);
            r->observed=(next&7u)?next:status;return LM_EIO;
        }
    }
    return LM_OK;
}
/* Opt-in setup only before any user VM exists, after a successful core reset
 * and private DMA teardown. Never reset or detach an inherited active AS. */
int lm_core_dvfs_prepare(struct lm_core *d,struct lm_job_result *r) {
    int e=dvfs_idle_snapshot(d,r);if(e)return e;
    if(d->vm_count)return LM_EBUSY;
    for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<16;i++)if(d->as_present&(1u<<i)) {
        uint32_t base=AS_BASE+0x40u*i;
        uint64_t root=rr64(d,base);uint32_t status=rr(d,base+0x28),mode=rr(d,base+0x30)&0xfu;
        if(io_error(d))return LM_EIO;
        if(root||(status&1u)||(mode!=0u&&mode!=1u)) {
            r->mismatch_reg=base;r->observed=root;return LM_EIO;
        }
    }
    for(unsigned i=0;i<16;i++)if(d->as_present&(1u<<i)) {
        uint32_t base=AS_BASE+0x40u*i;
        ww64(d,base+0x30,1);ww64(d,base,0);ww(d,base+0x18,1);
        e=poll_bits(d,base+0x28,1,0,LM_WAIT_NS);if(e)return e;
        if(!readback(d,r,base,0,true)||!readback(d,r,base+0x30,1,true))return LM_EIO;
    }
    d->dvfs_as_prepared=true;
    return lm_core_dvfs_guard(d,r);
}
/* This proves core idle/detached state only, not clock/rail/firmware ownership.
 * The adapter must independently exclude all external resource writers. */
int lm_core_dvfs_guard(struct lm_core *d,struct lm_job_result *r) {
    int e=dvfs_idle_snapshot(d,r);if(e)return e;
    if(!d->dvfs_as_prepared)return LM_ENOTSUP;
    unsigned count=0;
    for(struct lm_vm *v=d->vms;v;v=v->next_vm) {
        if(++count>16||v->dev!=d||v->quarantined)return LM_EIO;
        if(v->busy)return LM_EBUSY;
    }
    if(count!=d->vm_count)return LM_EIO;
    for(unsigned i=0;i<16;i++)if(d->as_present&(1u<<i)) {
        uint32_t base=AS_BASE+0x40u*i;
        if(!readback(d,r,base,0,true)||!readback(d,r,base+0x30,1,true))return LM_EIO;
        uint32_t status=rr(d,base+0x28);
        if(io_error(d))return LM_EIO;
        if(status&1u) {r->mismatch_reg=base+0x28;r->observed=status;return LM_EIO;}
    }
    r->gpu_fault=rr(d,GPU_IRQ_RAW)&0x81u;
    r->mmu_fault=rr(d,MMU_RAW)&(d->as_present|(d->as_present<<16));
    r->job_irq=rr(d,JOB_RAW);
    if(io_error(d)||r->gpu_fault||r->mmu_fault||(r->job_irq&(d->js_present<<16)))
        return LM_EIO;
    r->failed_stage=LM_JOB_NONE;return LM_OK;
}

int lm_core_return_guard(struct lm_core *d,struct lm_core_quiescence *out) {
    if(!out)return LM_EINVAL;
    memset(out,0,sizeof(*out));struct lm_job_result result;
    int error=lm_core_dvfs_guard(d,&result);if(error)return error;
    for(struct lm_vm *v=d->vms;v;v=v->next_vm) {
        if(v->scope_active||v->busy)return LM_EBUSY;
        if(!lm_core_vm_safe_to_free(v)||!v->root||!v->root->mem.cpu||!v->root->mem.pa)return LM_EIO;
        uint32_t pages=0,tables=0;
        for(struct lm_mapping *m=v->maps;m;m=m->next) {
            if(!m->mem||!m->pages||m->pages>LM_MAX_MAP_PAGES||pages>LM_MAX_VM_PAGES-m->pages)return LM_EIO;
            for(uint32_t i=0;i<m->pages;i++)if(!m->mem[i].cpu||!m->mem[i].pa)return LM_EIO;
            pages+=m->pages;
        }
        for(struct lm_table *t=v->tables;t;t=t->next) {
            if(!t->mem.cpu||!t->mem.pa||++tables>LM_MAX_VM_PAGES)return LM_EIO;
        }
        if(pages!=v->mapped_pages||tables!=v->table_pages)return LM_EIO;
        out->vm_count++;out->mapped_page_refs+=pages;out->table_pages+=tables;
    }
    return LM_OK;
}
/* dc maintenance is by CPU VA, not PA. Only coalesce adjacent retained pages
 * inside a single mapping; never bridge a hole, another mapping or wraparound.
 * Each callback still completes its range, and the caller completes the phase. */
static void sync_mapping(struct lm_core *d,struct lm_mapping *m,bool cpu,struct lm_job_result *r) {
    for(uint32_t i=0;i<m->pages;) {
        uint32_t n=1;
        uintptr_t base=(uintptr_t)m->mem[i].cpu;
        while(d->io.cache_batch&&n<64&&n<m->pages-i &&
              base<=UINTPTR_MAX-(uintptr_t)(n+1)*LM_PAGE_SIZE &&
              (uintptr_t)m->mem[i+n].cpu==base+(uintptr_t)n*LM_PAGE_SIZE)n++;
        if(cpu)d->io.for_cpu(d->io.cookie,m->mem[i].cpu,(size_t)n*LM_PAGE_SIZE);
        else d->io.for_device(d->io.cookie,m->mem[i].cpu,(size_t)n*LM_PAGE_SIZE);
        if(r->profile.enabled) {
            if(cpu){r->profile.cpu_ranges++;r->profile.cpu_pages+=n;}
            else {r->profile.device_ranges++;r->profile.clean_pages+=n;}
        }
        i+=n;
    }
}
int lm_core_vm_cpu_access(struct lm_vm *v,uint64_t va,uint32_t pages) {
    if(!v)return LM_EINVAL;
    struct lm_core *d=v->dev;
    if(d->external_unsafe)return LM_ENODEV;
    if(!d->io.gpu_only_sync)return LM_OK; /* Legacy mapping path unchanged. */
    if(!lm_core_ready(d)||!lm_core_vm_safe_to_free(v))return LM_ENODEV;
    if(d->bound||v->scope_active)return LM_EBUSY;
    for(struct lm_mapping *m=v->maps;m;m=m->next)if(m->va==va&&m->pages==pages) {
        struct lm_job_result r;memset(&r,0,sizeof(r));
        /* Initial CPU allocation bytes must reach memory before a first map.
         * After GPU ownership, invalidate only: never write a stale CPU view
         * back over completed GPU data. The caller holds its BO/client lock. */
        if(!m->device_initialized) {
            sync_mapping(d,m,false,&r);m->device_initialized=true;
        }
        sync_mapping(d,m,true,&r);d->io.barrier(d->io.cookie);
        m->gpu_only=false;return LM_OK;
    }
    return LM_EINVAL;
}
static int bind_vm(struct lm_vm *v,struct lm_job_result *r) {
    struct lm_core *d=v->dev;
    uint64_t started=profile_now(d);
    uint32_t cleaned_tables=0;
    for(struct lm_table *t=v->tables;t;t=t->next)if(!d->io.table_dirty_sync||t->dirty) {
        d->io.for_device(d->io.cookie,t->mem.cpu,4096);
        t->dirty=false;cleaned_tables++;
    }
    for(struct lm_mapping *m=v->maps;m;m=m->next)if(!v->scope_active||m->selected) {
        bool owned=d->io.gpu_only_sync&&v->scope_active&&m->gpu_only;
        if(!owned||!m->device_initialized) {
            sync_mapping(d,m,false,r);m->device_initialized=true;
        }
    }
    d->io.barrier(d->io.cookie);
    if(r->profile.enabled) {
        r->profile.device_ranges+=cleaned_tables;
        r->profile.cleaned_table_pages=cleaned_tables;
        r->profile.for_device_ns=profile_now(d)-started;
    }
    /* Unmapped between jobs. Root depth and GPU walk policy are one mode. */
    ww64(d,AS_BASE+0x30,d->transcfg);
    ww64(d,AS_BASE,v->root->mem.pa);
    ww64(d,AS_BASE+8,UINT64_C(0x4c8d888d8f88));
    d->bound=v;
    int e=as_cmd(d,1);if(e)return e;
    if(!readback(d,r,AS_BASE,v->root->mem.pa,true) ||
       !readback(d,r,AS_BASE+8,UINT64_C(0x4c8d888d8f88),true) ||
       !readback(d,r,AS_BASE+0x30,d->transcfg,true) ||
        io_error(d))return LM_EIO;
    if(d->io.pre_as_flush) {
        /* UPDATE above has broadcast this root. Invalidate translations only
         * after CPU table clean and the new AS configuration are complete.
         * Like Linux panfrost mmu_hw_do_operation_locked, LOCK + FLUSH waits
         * for completion without a separate UNLOCK command. */
        r->failed_stage=LM_JOB_AS_FLUSH;
        ww64(d,AS_BASE+0x10,31); /* entire supported 0..4GiB GPU VA range */
        if(!readback(d,r,AS_BASE+0x10,31,true))return LM_EIO;
        if((e=as_cmd(d,2)))return e;
        if((e=as_cmd(d,d->io.pre_as_flush==1?5:4)))return e;
        if((e=as_cmd(d,1)))return e;
        d->io.barrier(d->io.cookie);
    }
    return LM_OK;
}
/* Resolve only retained, currently visible descriptor memory. Every snapshot
 * is aligned and fits in one GPU page. Missing/out-of-scope backing is counted
 * by the mask; never walk an untrusted GPU VA as a CPU pointer. */
static bool descriptor_copy(struct lm_vm *v,struct lm_job_result *r,unsigned index,uint64_t va) {
    if(index>=4)return false;
    r->descriptors.va[index]=va;
    if((va&63u) || va<LM_VA_MIN || va>=LM_VA_END ||
       !lm_core_vm_lookup(v,va,NULL) || !lm_core_vm_lookup(v,va+63u,NULL))return false;
    for(struct lm_mapping *m=v->maps;m;m=m->next) {
        if(va<m->va || va-m->va>((uint64_t)m->pages<<12)-64u)continue;
        if(v->scope_active&&!m->selected)return false;
        uint64_t offset=va-m->va;
        const volatile uint32_t *p=(const volatile uint32_t *)
            ((const uint8_t *)m->mem[offset>>12].cpu+(offset&4095));
        for(unsigned i=0;i<16;i++)r->descriptors.words[index][i]=p[i];
        r->descriptors.mapping[index]=m->va;r->descriptors.pages[index]=m->pages;
        r->descriptors.mask|=1u<<index;return true;
    }
    return false;
}
static uint32_t chain_node_copy(struct lm_vm *v,struct lm_chain_node *node) {
    uint64_t va=node->va;
    if((va&63u) || !lm_core_vm_lookup(v,va,NULL) || !lm_core_vm_lookup(v,va+63,NULL))return 0;
    for(struct lm_mapping *m=v->maps;m;m=m->next) {
        if(va<m->va || va-m->va>((uint64_t)m->pages<<12)-64)continue;
        if(v->scope_active&&!m->selected)return 0;
        uint64_t offset=va-m->va;
        node->bo_va=m->va;node->bo_pages=m->pages;
        uint32_t wanted=16;
        for(uint32_t i=0;i<wanted;i++) {
            uint64_t at=offset+(uint64_t)i*4;
            node->words[i]=*(const volatile uint32_t *)((const uint8_t *)m->mem[at>>12].cpu+(at&4095));
            if(i==15) {
                node->type=(node->words[4]>>1)&127u;
                uint32_t full=node->type==10?96u:(node->type==7||node->type==5||node->type==4)?64u:16u;
                if((uint64_t)full*4<=((uint64_t)m->pages<<12)-offset)wanted=full;
            }
        }
        node->next=(uint64_t)node->words[6]|(uint64_t)node->words[7]<<32;
        node->captured_words=wanted;return wanted;
    }
    return 0;
}
static void chain_snapshot(struct lm_vm *v,uint64_t jc,struct lm_job_result *r) {
    struct lm_chain_snapshot *s=v->dev->chain;if(!s)return;
    memset(s,0,sizeof(*s));r->chain=s;
    uint64_t va=jc;
    while(va && s->count<LM_CHAIN_NODES) {
        for(uint32_t i=0;i<s->count;i++)if(s->nodes[i].va==va){s->stop_reason=2;s->stop_va=va;return;}
        struct lm_chain_node *node=&s->nodes[s->count++];node->va=va;
        if(!chain_node_copy(v,node)){s->stop_reason=3;s->stop_va=va;return;}
        if(!(node->words[4]&1u)){s->stop_reason=4;s->stop_va=va;return;}
        va=node->next;
    }
    if(va){s->stop_reason=1;s->stop_va=va;}
}
static void descriptor_snapshot(struct lm_vm *v,uint64_t jc,struct lm_job_result *r) {
    if(!v->dev->io.capture_descriptors)return;
    chain_snapshot(v,jc,r);
    for(struct lm_mapping *m=v->maps;m;m=m->next)if(!v->scope_active||m->selected) {
        ++r->descriptors.ranges;r->descriptors.readonly_ranges+=m->readonly;
    }
    if(!descriptor_copy(v,r,0,jc))return;
    /* v7.xml: fragment type 9, payload @32, tagged FBD pointer @40;
     * params' tiler pointer @56. Low six FBD bits encode type/ZS/RT count. */
    if(((r->descriptors.words[0][4]>>1)&127u)!=9u ||
       ((v->dev->id>>16)&0xffffu)!=0x7402u)return;
    uint64_t tagged=(uint64_t)r->descriptors.words[0][10]|
        (uint64_t)r->descriptors.words[0][11]<<32;
    uint64_t fbd=tagged&~UINT64_C(63);
    if(!descriptor_copy(v,r,1,fbd))return;
    uint64_t tiler=(uint64_t)r->descriptors.words[1][14]|
        (uint64_t)r->descriptors.words[1][15]<<32;
    (void)descriptor_copy(v,r,2,tiler);
    if(fbd<=UINT64_MAX-192u)
        (void)descriptor_copy(v,r,3,fbd+128u+((tagged&2u)?64u:0u));
}
static void pre_job_dump(struct lm_vm *v,uint64_t jc) {
    struct lm_core *d=v->dev;
    if(!d->io.pre_job||d->pre_job_dumps>=32)return;
    /* Resolve GPU VA through retained mapping metadata, never dereference jc
     * as a kernel VA. The validated 64-byte-aligned header fits in one page. */
    for(struct lm_mapping *m=v->maps;m;m=m->next) {
        if(jc<m->va||jc-m->va>=((uint64_t)m->pages<<12))continue;
        uint64_t offset=jc-m->va;
        const volatile uint32_t *p=(const volatile uint32_t *)
            ((const uint8_t *)m->mem[offset>>12].cpu+(offset&4095));
        uint32_t words[4];
        for(unsigned i=0;i<4;i++)words[i]=p[i];
        d->pre_job_dumps++;
        d->io.pre_job(d->io.cookie,jc,words);
        return;
    }
}
int lm_core_submit_wait(struct lm_vm *v,uint64_t jc,uint32_t requirements,uint64_t timeout_ns,struct lm_job_result *r) {
    if(!r)return LM_EINVAL;
    memset(r,0,sizeof(*r));
    if(!v||!jc||(jc&63)||requirements&~1u||!timeout_ns||timeout_ns>UINT64_C(10000000000))return LM_EINVAL;
    struct lm_core *d=v->dev;
    if(!lm_core_ready(d)||!lm_core_vm_safe_to_free(v))return LM_ENODEV;
    if(d->bound)return LM_EBUSY;
    /* Job descriptors are data, executable shaders elsewhere have explicit EXEC. */
    uint32_t jc_flags=0,jc_end_flags=0;
    if(!lm_core_vm_lookup(v,jc,&jc_flags)||!lm_core_vm_lookup(v,jc+63,&jc_end_flags)||
       !(jc_flags&LM_MAP_WRITE)||!(jc_end_flags&LM_MAP_WRITE))return LM_EFAULT;
    uint32_t slot=(requirements&1)?0:1;
    if(!(d->js_present&(1u<<slot)))return LM_ENOTSUP;
    uint64_t started=0;
    if(d->io.before_job) {
        started=d->io.now_ns(d->io.cookie);r->started_ns=started;
        r->failed_stage=LM_JOB_DVFS_GUARD;
        int admission=d->io.before_job(d->io.cookie);
        uint64_t now=d->io.now_ns(d->io.cookie);
        if(!admission&&!lm_core_ready(d))admission=LM_ENODEV;
        if(!admission&&(now<started||now-started>=timeout_ns)) {
            r->failed_stage=LM_JOB_DEADLINE;admission=now<started?LM_EIO:LM_ETIMEDOUT;
        }
        if(admission) {
            r->completed_ns=now;r->elapsed_ns=now>=started?now-started:0;
            return admission; /* No bound root, reset, stage sequence or DMA. */
        }
        r->failed_stage=LM_JOB_NONE;
    }
    /* External admission precedes even the first GPU-bank read. Legacy and
     * firmware callers with no callback keep their original clock/budget path. */
    uint32_t slot_state=rr(d,JS_BASE(slot)+0x60);
    if(io_error(d))return LM_EIO;
    if(slot_state&7)return LM_EBUSY;
    if(!d->io.before_job) {started=d->io.now_ns(d->io.cookie);r->started_ns=started;}
    v->busy=true;
    struct lm_poll_counts control_before=d->control_wait;
    r->profile.enabled=d->io.profile_jobs;
    if(r->profile.enabled){
        r->profile.vm_pages=v->mapped_pages;
        r->profile.mapped_pages=v->scope_active?v->active_pages:v->mapped_pages;
        r->profile.table_pages=v->table_pages;
        r->profile.hybrid_poll=d->io.spin_us!=NULL;
        if(v->scope_active)for(struct lm_mapping *m=v->maps;m;m=m->next)
            if(m->selected&&m->readonly)r->profile.readonly_pages+=m->pages;
    }
    uint64_t measured=profile_now(d);
    phase(d,LM_PHASE_BIND);r->failed_stage=LM_JOB_BIND;
    int e=bind_vm(v,r);
    if(r->profile.enabled)r->profile.bind_ns=profile_now(d)-measured-r->profile.for_device_ns;
    if(!e) {
        r->failed_stage=LM_JOB_IRQ_CLEAR;
        ww(d,JOB_CLR,(1u<<slot)|(1u<<(16+slot)));ww(d,MMU_CLR,0x10001);
        ww(d,GPU_IRQ_CLR,0x81);
        if(runtime_fault(d) || (rr(d,JOB_RAW)&((1u<<slot)|(1u<<(16+slot)))) || !job_idle(d,r))e=LM_EIO;
    }
    if(!e){pre_job_dump(v,jc);descriptor_snapshot(v,jc,r);}
    if(!e&&d->io.pre_job_flush) {
        /* CPU pages have reached the for_device completion point. Complete
         * GPU cache maintenance before publishing this job's NEXT state. */
        measured=profile_now(d);
        phase(d,LM_PHASE_CACHE);r->failed_stage=LM_JOB_CACHE;e=caches(d);
        if(r->profile.enabled)r->profile.preflush_ns=profile_now(d)-measured;
    }
    if(!e) {
        uint64_t now=d->io.now_ns(d->io.cookie);
        if(now<started||now-started>=timeout_ns) {
            r->failed_stage=LM_JOB_DEADLINE;e=now<started?LM_EIO:LM_ETIMEDOUT;
        }
    }
    if(!e) {
        phase(d,LM_PHASE_NEXT);r->failed_stage=LM_JOB_NEXT;
        ww64(d,JS_BASE(slot)+0x40,jc);ww64(d,JS_BASE(slot)+0x50,d->shaders);
        ww(d,JS_BASE(slot)+0x58,LM_JOB_NEXT_CONFIG);
        if(!readback(d,r,JS_BASE(slot)+0x40,jc,true) || !readback(d,r,JS_BASE(slot)+0x50,d->shaders,true) ||
            !readback(d,r,JS_BASE(slot)+0x58,LM_JOB_NEXT_CONFIG,false) || io_error(d))e=LM_EIO;
    }
    if(!e) {
        uint64_t now=d->io.now_ns(d->io.cookie);
        if(now<started||now-started>=timeout_ns) {
            r->failed_stage=LM_JOB_DEADLINE;e=now<started?LM_EIO:LM_ETIMEDOUT;
        }
    }
    if(!e) {
        measured=profile_now(d);
        uint64_t thread_start=r->profile.enabled&&d->io.thread_ns?d->io.thread_ns(d->io.cookie):0;
        phase(d,LM_PHASE_WAIT);r->failed_stage=LM_JOB_WAIT;
        struct lm_poll_state pause={.start=d->io.now_ns(d->io.cookie),.budget_us=100};
        d->io.barrier(d->io.cookie);
        if(d->io.job_done) {r->jm.start_ns=d->io.now_ns(d->io.cookie);r->jm.launched=true;}
        ww(d,JS_BASE(slot)+0x60,1);
        r->sequence=++d->sequence;
        uint32_t loops=0;
        uint64_t previous_poll=0;
        for(;;) {
            uint64_t poll_at=profile_now(d);
            if(r->profile.enabled) {
                if(r->profile.polls && poll_at-previous_poll>r->profile.max_poll_gap_ns)
                    r->profile.max_poll_gap_ns=poll_at-previous_poll;
                previous_poll=poll_at;r->profile.polls++;
            }
            r->job_irq=rr(d,JOB_RAW);
            r->mmu_fault=rr(d,MMU_RAW)&0x10001;
            r->gpu_fault=rr(d,GPU_IRQ_RAW)&0x81;
            if(io_error(d)||r->mmu_fault||r->gpu_fault||(r->job_irq&(1u<<(16+slot)))) {e=LM_EIO;break;}
            if(r->job_irq&(1u<<slot)) {
                if(r->profile.enabled)r->profile.done_seen_ns=profile_now(d);
                break;
            }
            if(r->profile.enabled)r->profile.last_busy_ns=poll_at;
            uint64_t now=d->io.now_ns(d->io.cookie);
            if(now-started>=timeout_ns||++loops>=1000000) {e=LM_ETIMEDOUT;break;}
            e=poll_pause(d,&pause,now,timeout_ns-(now-started),&r->profile.job_wait);
            if(e)break;
        }
        r->job_status=rr(d,JS_BASE(slot)+0x24);
        if(!e&&(io_error(d)||(r->job_status&0xff)>1 ||
                 (rr(d,JS_BASE(slot)+0x60)&7u)))e=LM_EIO;
        if(!e) {
            /* NEXT can be empty while CURRENT still runs. Refresh the
             * hardware snapshot, then prove both idle before cache/detach. */
            ww(d,JOB_CLR,(1u<<slot)|(1u<<(16+slot)));
            if(!job_idle(d,r)||runtime_fault(d))e=LM_EIO;
            if(!e&&d->io.job_done) {
                r->jm.end_ns=d->io.now_ns(d->io.cookie);
                r->jm.idle_verified=r->jm.end_ns>r->jm.start_ns;
            }
        }
        if(r->profile.enabled)r->profile.run_ns=profile_now(d)-measured;
        if(thread_start)r->profile.run_thread_ns=d->io.thread_ns(d->io.cookie)-thread_start;
    }
    if(!e){
        measured=profile_now(d);
        phase(d,LM_PHASE_CACHE);r->failed_stage=LM_JOB_CACHE;e=caches(d);
        if(r->profile.enabled)r->profile.postflush_ns=profile_now(d)-measured;
    }
    if(!e){
        measured=profile_now(d);
        phase(d,LM_PHASE_DETACH);r->failed_stage=LM_JOB_DETACH;e=detach(d);
        if(r->profile.enabled)r->profile.detach_ns=profile_now(d)-measured;
    }
    if(!e) {
        r->failed_stage=LM_JOB_ACK;
        ww(d,JOB_CLR,(1u<<slot)|(1u<<(16+slot)));
        if(runtime_fault(d)||!job_idle(d,r))e=LM_EIO;
    }
    if(e) {
        r->js_head_lo=rr(d,JS_BASE(slot)+0x00);r->js_head_hi=rr(d,JS_BASE(slot)+0x04);
        r->js_tail_lo=rr(d,JS_BASE(slot)+0x08);r->js_tail_hi=rr(d,JS_BASE(slot)+0x0c);
        r->fault_extra_lo=rr(d,AS_BASE+0x38);r->fault_extra_hi=rr(d,AS_BASE+0x3c);
        r->gpu_fault_status=rr(d,GPU_FAULT);
        r->gpu_fault_lo=rr(d,GPU_FAULT_LO);r->gpu_fault_hi=rr(d,GPU_FAULT_LO+4);
        r->mmu_fault=rr(d,MMU_RAW)&0x10001u;r->gpu_fault=rr(d,GPU_IRQ_RAW)&0x81u;
        if(r->mmu_fault) {r->fault_address=rr64(d,AS_BASE+0x20);r->mmu_fault=rr(d,AS_BASE+0x1c);}
        else if(r->gpu_fault) {r->fault_address=rr64(d,GPU_FAULT_LO);r->gpu_fault=rr(d,GPU_FAULT);}
        fault_vm_snapshot(v,r);
        phase(d,LM_PHASE_RESET);r->reset_completed=reset(d)==LM_OK;
        if(!r->reset_completed) {v->quarantined=true;r->quarantined=true;}
        /* Successful reset proves DMA stopped, but internal cores were reset.
         * Keep driver offline for remainder of boot; do not silently reuse context. */
        d->ready=false;d->terminal_offline=true;
    }
    if(!v->quarantined) {
        measured=profile_now(d);
        for(struct lm_mapping *m=v->maps;m;m=m->next) if((m->flags&LM_MAP_WRITE)&&(!v->scope_active||m->selected)&&
            !(!e&&v->scope_active&&m->readonly)&&
            !(d->io.gpu_only_sync&&v->scope_active&&m->gpu_only))
            sync_mapping(d,m,true,r);
        d->io.barrier(d->io.cookie);
        if(r->profile.enabled)r->profile.for_cpu_ns=profile_now(d)-measured;
    }
    r->completed_ns=d->io.now_ns(d->io.cookie);
    r->elapsed_ns=r->completed_ns-started;
    if(r->profile.enabled)r->profile.control_wait=(struct lm_poll_counts){
        .spins=d->control_wait.spins-control_before.spins,
        .sleeps=d->control_wait.sleeps-control_before.sleeps,
        .spin_ns=d->control_wait.spin_ns-control_before.spin_ns,
        .sleep_ns=d->control_wait.sleep_ns-control_before.sleep_ns};
    if(!e)r->failed_stage=LM_JOB_NONE;
    v->busy=false;
    if(d->io.job_done)d->io.job_done(d->io.cookie,r,e);
    return e;
}
int lm_core_submit_ranges(struct lm_vm *v,uint64_t jc,uint32_t req,uint64_t timeout,
                          struct lm_job_result *r,const struct lm_range *ranges,uint32_t count) {
    if(!r)return LM_EINVAL;
    memset(r,0,sizeof(*r));
    if(!v)return LM_EINVAL;
    struct lm_core *d=v->dev;
    if(!d->io.submit_scope)return lm_core_submit_wait(v,jc,req,timeout,r);
    if(!lm_core_ready(d)||!lm_core_vm_safe_to_free(v))return LM_ENODEV;
    if(d->bound||v->scope_active)return LM_EBUSY;
    if(!ranges||!count||count>LM_MAX_SUBMIT_RANGES||!jc||(jc&63)||req&~1u||
        !timeout||timeout>UINT64_C(10000000000))return LM_EINVAL;
    uint64_t dispatch_started=d->io.now_ns(d->io.cookie);
    uint64_t started=profile_now(d);
    uint32_t requested=0;
    bool job_selected=false;
    for(struct lm_mapping *m=v->maps;m;m=m->next){m->selected=false;m->gpu_only=false;m->readonly=false;}
    for(uint32_t i=0;i<count;i++) {
        struct lm_mapping *found=NULL;
        if(!range_valid(ranges[i].va,ranges[i].pages)||(ranges[i].flags&~(LM_RANGE_GPU_ONLY|LM_RANGE_READONLY)))goto invalid;
        for(struct lm_mapping *m=v->maps;m;m=m->next)
            if(m->va==ranges[i].va&&m->pages==ranges[i].pages){found=m;break;}
        if(!found)goto invalid; /* Exact retained mapping, never a partial BO. */
        bool is_job=(found->flags&LM_MAP_WRITE)&&jc>=found->va&&
                    jc-found->va<=((uint64_t)found->pages<<12)-64;
        if(is_job&&(ranges[i].flags&(LM_RANGE_GPU_ONLY|LM_RANGE_READONLY)))goto invalid;
        bool owned=(ranges[i].flags&LM_RANGE_GPU_ONLY)!=0;
        bool ro=(ranges[i].flags&LM_RANGE_READONLY)!=0;
        if(!found->selected){found->selected=true;requested+=found->pages;found->gpu_only=owned;found->readonly=ro;}
        else {found->gpu_only=found->gpu_only&&owned;found->readonly=found->readonly&&ro;}
        if(is_job)job_selected=true;
    }
    if(!job_selected)goto invalid;
    uint32_t mode=scope_alias_mode(v),parents=0,entries=0;
    uint64_t setup=0;
    int e=LM_OK;
    if(mode==1) {
        /* Even a partially changed software scope must use the common restore
         * path. No hardware root has been published at this point. */
        v->scope_active=true;v->active_pages=requested;
        if(!scope_visible(v,false,&parents,&entries)) {
            r->profile.enabled=d->io.profile_jobs;r->failed_stage=LM_JOB_SCOPE_SETUP;
            setup=profile_now(d)-started;e=LM_EIO;goto restore_scope;
        }
    }
    setup=profile_now(d)-started;
    /* Alias-index allocation and scope preparation are inside this dispatch's
     * wall budget, even without profiling. Expiry before hardware publication
     * restores the software scope below, without launching or resetting GPU. */
    uint64_t ready_at=d->io.now_ns(d->io.cookie);
    if(ready_at<dispatch_started||ready_at-dispatch_started>=timeout) {
        r->profile.enabled=d->io.profile_jobs;r->failed_stage=LM_JOB_DEADLINE;
        e=ready_at<dispatch_started?LM_EIO:LM_ETIMEDOUT;
    } else e=lm_core_submit_wait(v,jc,req,timeout-(ready_at-dispatch_started),r);
restore_scope:
    started=profile_now(d);
    if(v->scope_active&&!v->quarantined&&!d->bound&&!d->external_unsafe) {
        if(!scope_visible(v,true,NULL,NULL)) {
            if(!e||r->failed_stage==LM_JOB_NONE)r->failed_stage=LM_JOB_SCOPE_RESTORE;
            v->quarantined=true;d->ready=false;d->terminal_offline=true;r->quarantined=true;e=LM_EIO;
        } else {v->scope_active=false;v->active_pages=0;}
    }
    if(r->profile.enabled) {
        r->profile.requested_pages=requested;r->profile.scope_mode=mode;
        r->profile.scope_setup_ns=setup;r->profile.scope_restore_ns=profile_now(d)-started;
        r->profile.masked_leaf_tables=parents;r->profile.masked_leaf_entries=entries;
    }
    r->started_ns=dispatch_started;r->completed_ns=d->io.now_ns(d->io.cookie);
    return e;
invalid:
    for(struct lm_mapping *m=v->maps;m;m=m->next){m->selected=false;m->gpu_only=false;m->readonly=false;}
    return LM_EINVAL;
}
/* No allocation, PTE mutation, MMIO or pointer retention during prevalidation.
 * The serializer keeps these mappings stable until both jobs have retired. */
static int pair_validate(struct lm_vm *v,uint64_t jc,const struct lm_range *ranges,uint32_t count) {
    if(!ranges||!count||count>LM_MAX_SUBMIT_RANGES||!jc||(jc&63u))return LM_EINVAL;
    bool job=false;
    for(uint32_t i=0;i<count;i++) {
        if(!range_valid(ranges[i].va,ranges[i].pages)||
           (ranges[i].flags&~(LM_RANGE_GPU_ONLY|LM_RANGE_READONLY)))return LM_EINVAL;
        struct lm_mapping *m=v->maps;
        while(m&&(m->va!=ranges[i].va||m->pages!=ranges[i].pages))m=m->next;
        if(!m)return LM_EINVAL;
        if((m->flags&LM_MAP_WRITE)&&jc>=m->va&&jc-m->va<=((uint64_t)m->pages<<12)-64u) {
            if(ranges[i].flags&(LM_RANGE_GPU_ONLY|LM_RANGE_READONLY))return LM_EINVAL;
            job=true;
        }
    }
    return job?LM_OK:LM_EINVAL;
}
int lm_core_submit_pair(struct lm_vm *v,uint64_t vt_jc,uint64_t fs_jc,uint64_t timeout,
                        struct lm_job_result results[2],const struct lm_range *vt,uint32_t vtc,
                        const struct lm_range *fs,uint32_t fsc) {
    if(!results)return LM_EINVAL;
    memset(results,0,2*sizeof(*results));
    if(!v||!timeout||timeout>UINT64_C(10000000000))return LM_EINVAL;
    struct lm_core *d=v->dev;
    if(!lm_core_ready(d)||!lm_core_vm_safe_to_free(v))return LM_ENODEV;
    if(d->bound||v->scope_active)return LM_EBUSY;
    uint64_t started=d->io.now_ns(d->io.cookie);
    int e=pair_validate(v,vt_jc,vt,vtc);
    if(!e)e=pair_validate(v,fs_jc,fs,fsc);
    if(e)return e; /* Bad FS never causes a partial VTC launch. */
    for(unsigned stage=0;stage<2;stage++) {
        uint64_t now=d->io.now_ns(d->io.cookie);
        if(now<started)return LM_EIO;
        if(now-started>=timeout)return LM_ETIMEDOUT;
        e=lm_core_submit_ranges(v,stage?fs_jc:vt_jc,stage?1u:0u,timeout-(now-started),
            &results[stage],stage?fs:vt,stage?fsc:vtc);
        results[stage].pair_stage_attempted=true;results[stage].pair_stage_error=e;
        if(e)return e; /* First error skips FS; no retry or success fence. */
        /* Required flush/detach/CPU maintenance must finish even if it makes
         * retirement late. Keep actual stage success, but never report pair
         * deadline success merely because the final job already completed. */
        now=d->io.now_ns(d->io.cookie);
        if(now<started)return LM_EIO;
        if(now-started>=timeout)return LM_ETIMEDOUT;
    }
    return LM_OK;
}
int lm_core_shutdown(struct lm_core *d) {
    if(d&&d->external_unsafe)return LM_EBUSY;
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
    if(d->external_unsafe)return LM_EBUSY;
    if(d->vm_count||d->bound)return LM_EBUSY;
    if(d->poisoned||io_error(d)) {
        if(!io_error(d)||!d->io.recover_io)return LM_EBUSY;
        phase(d,LM_PHASE_RESET);int e=reset(d);if(e)return e;
        d->terminal_offline=true;
    }
    int e=lm_core_shutdown(d);if(e)return e;
    struct lm_ops io=d->io;
    if(d->chain)io.free(io.cookie,d->chain,sizeof(*d->chain));
    io.free(io.cookie,d,sizeof(*d));return LM_OK;
}
