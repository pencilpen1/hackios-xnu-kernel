/* SPDX-License-Identifier: GPL-2.0-only
 * One private transaction using the actual shared core. Callers must provide
 * real powered MMIO/owned pages/cache callbacks and serialization.
 * This does not grant platform/firmware ownership or install a provider.
 * Any retained backing/core must survive until the device is proved stopped. */
#ifndef LANCELOT_MALI_PRIVATE_DMA_H
#define LANCELOT_MALI_PRIVATE_DMA_H
#include "lancelot_mali_core.h"
#include "lancelot_mali_smoke.h"

#define LM_PRIVATE_JOB_VA LM_VA_MIN
#define LM_PRIVATE_OUTPUT_VA (LM_VA_MIN+UINT64_C(0x2000))
#define LM_PRIVATE_VALUE UINT32_C(0x4c4d4732)
/* kbase AS_FAULTSTATUS: WRITE access, TRANSLATION_FAULT class; retain the
 * level and source fields as evidence without accepting another access/type. */
#define LM_AS_WRITE_TRANSLATION_FAULT(status) (((status)&UINT32_C(0x3f8))==UINT32_C(0x3c0))

/* Real fragment-job clear. A clear-only fragment job writes the RGBA8 surface
 * from the framebuffer descriptor; it is not a WRITE_VALUE job. The layout is
 * the Mesa 25.0.7 panfrost v7 (Bifrost) FRAMEBUFFER/RENDER_TARGET/FRAGMENT_JOB
 * packing, cross-checked against the generated v7_pack.h. */
#define LM_CLEAR_WIDTH 64u
#define LM_CLEAR_HEIGHT 64u
#define LM_CLEAR_BPP 4u
#define LM_CLEAR_BYTES (LM_CLEAR_WIDTH*LM_CLEAR_HEIGHT*LM_CLEAR_BPP)
#define LM_CLEAR_PAGES (LM_CLEAR_BYTES/LM_PAGE_SIZE)
#define LM_CLEAR_MAX_SURFACES 2u
#define LM_CLEAR_MAX_PAGES (LM_CLEAR_PAGES*LM_CLEAR_MAX_SURFACES)
#define LM_CLEAR_VALUE UINT32_C(0xff3366cc) /* R=0xcc G=0x66 B=0x33 A=0xff */
#define LM_CLEAR_VALUE2 UINT32_C(0xffcc9933)
#define LM_CLEAR_DESC_OFFSET 256u
#define LM_CLEAR_SECOND_FBD_OFFSET 512u
#define LM_CLEAR_SECOND_JOB_OFFSET 768u
#define LM_CLEAR_GUARD_OFFSET 896u
#define LM_CLEAR_GUARD_WORD UINT32_C(0x636c7247)

struct lm_private_dma {
    struct lm_ops io;
    struct lm_core *core;
    struct lm_vm *vm;
    struct lm_page job_page,output_page;
    struct lm_page clear_pages[LM_CLEAR_MAX_PAGES];
    struct lm_job_result job_result;
    int error,cleanup_error;
    uint32_t bad_offset;
    uint32_t observed_value,descriptor_status;
    uint32_t clear_bad_word,clear_actual;
    bool initialized,submitted,guards_valid,bad_job,backing_retained,cleaned;
    bool mmu_fault_test,expected_fault,clear_test,clear_chain;
    uint32_t clear_value,clear_value2;
    uint64_t expected_fault_va;
};

/* Write one FBD (128 bytes) and its RENDER_TARGET (64 bytes) at a 64-byte
 * aligned word offset. fbd_va is the GPU VA of that FBD. */
static inline void lm_v7_clear_fbd(uint32_t page[LM_PAGE_SIZE/4],unsigned word,
    uint64_t fbd_va,uint64_t output_va,uint32_t color) {
    (void)fbd_va;
    uint32_t *fbd=page+word;
    /* width/height are minus one; bounds are the full surface. */
    fbd[8]=(LM_CLEAR_WIDTH-1u)|((LM_CLEAR_HEIGHT-1u)<<16);
    fbd[10]=fbd[8];
    /* sample_count=1 (log2 0), single pattern, tie-break 2. The effective
     * tile is 16x16 = 256 pixels, so its log2 is 8; the RGBA8 colour buffer
     * allocation is ALIGN_POT(4*256,1024)=1024, stored divided by 1024. */
    fbd[11]=(2u<<6)|(8u<<9)|(1u<<24);
    /* D24 z internal format; no z/s write or clear. */
    fbd[12]=1u<<16;
    uint32_t *rt=fbd+32; /* Framebuffer aggregate is 128 bytes. */
    /* RGBA8 internal format, linear writeback, dithering, clean tile write.
     * The identity component swizzle is 0x688 (X, Y<<3, Z<<6, W<<9), not 0:
     * zero maps every component to R and hardware writes 0xRRRRRRRR. */
    rt[0]=1u<<26;
    rt[1]=1u|(19u<<3)|(2u<<8)|(1u<<15)|(UINT32_C(0x688)<<16)|(1u<<31);
    rt[8]=(uint32_t)output_va;rt[9]=(uint32_t)(output_va>>32);
    rt[10]=LM_CLEAR_WIDTH*LM_CLEAR_BPP;
    rt[11]=0; /* surface stride is only set for layered MSAA */
    rt[12]=rt[13]=rt[14]=rt[15]=color;
}
/* Write one 64-byte FRAGMENT_JOB descriptor. next_va links the next job. */
static inline void lm_v7_clear_job(uint32_t page[LM_PAGE_SIZE/4],unsigned word,
    uint64_t fbd_va,uint64_t next_va) {
    uint64_t tag=(fbd_va&~UINT64_C(0x3f))|1u;
    uint32_t *job=page+word;
    /* Job Header: is_64b, type=Fragment (9), index=1. */
    job[4]=UINT32_C(0x00010013);
    job[6]=(uint32_t)next_va;job[7]=(uint32_t)(next_va>>32);
    /* Bounds are in 16x16 tiles: (64/16)-1 = 3 on each axis. */
    job[9]=((LM_CLEAR_WIDTH/16u)-1u)|(((LM_CLEAR_HEIGHT/16u)-1u)<<16);
    job[10]=(uint32_t)tag;job[11]=(uint32_t)(tag>>32);
}
/* One or two chained fragment clears. The first job's Next is the second job
 * descriptor; this is the same job-chain mechanism Mesa uses for tiler then
 * fragment submission. fbd_va is the descriptor page base. */
static inline void lm_v7_clear_prepare(uint32_t page[LM_PAGE_SIZE/4],
    uint64_t fbd_va,uint64_t output_va,uint32_t color,uint32_t color2,
    unsigned surfaces) {
    for(unsigned i=0;i<LM_PAGE_SIZE/4u;i++)page[i]=0;
    lm_v7_clear_fbd(page,0,fbd_va,output_va,color);
    lm_v7_clear_job(page,LM_CLEAR_DESC_OFFSET/4u,fbd_va,
        surfaces>1?fbd_va+LM_CLEAR_SECOND_JOB_OFFSET:0);
    if(surfaces>1) {
        lm_v7_clear_fbd(page,LM_CLEAR_SECOND_FBD_OFFSET/4u,
            fbd_va+LM_CLEAR_SECOND_FBD_OFFSET,output_va+LM_CLEAR_BYTES,color2);
        lm_v7_clear_job(page,LM_CLEAR_SECOND_JOB_OFFSET/4u,
            fbd_va+LM_CLEAR_SECOND_FBD_OFFSET,0);
    }
    for(unsigned i=LM_CLEAR_GUARD_OFFSET/4u;i<LM_PAGE_SIZE/4u;i++)
        page[i]=LM_CLEAR_GUARD_WORD^i;
}

/* Every surface word must be the packed clear color and the descriptor page's
 * tail must be untouched. On failure report the first bad word and its value. */
static inline bool lm_v7_clear_validate(const struct lm_page *pages,uint32_t count,
    const uint32_t *page,uint32_t color,uint32_t color2,unsigned surfaces,
    uint32_t *bad_word,uint32_t *actual,bool *bad_job) {
    for(unsigned i=LM_CLEAR_GUARD_OFFSET/4u;i<LM_PAGE_SIZE/4u;i++)
        if(page[i]!=(LM_CLEAR_GUARD_WORD^i)) {
            *bad_word=i*4u;*actual=page[i];*bad_job=true;return false;
        }
    for(unsigned p=0;p<count;p++) {
        uint32_t want=(surfaces>1&&p>=LM_CLEAR_PAGES)?color2:color;
        for(unsigned i=0;i<LM_PAGE_SIZE/4u;i++) {
            uint32_t v=((const volatile uint32_t *)pages[p].cpu)[i];
            if(v!=want) {
                *bad_word=p*LM_PAGE_SIZE+i*4u;*actual=v;*bad_job=false;return false;
            }
        }
    }
    *bad_word=0;*actual=color;*bad_job=false;return true;
}

static int lm_private_dma_cleanup(struct lm_private_dma *s) {
    if(s->vm) {
        if(!lm_core_vm_safe_to_free(s->vm))return LM_EBUSY;
        int e=lm_core_vm_destroy(s->vm);if(e)return e;s->vm=NULL;
    }
    if(s->core) {
        int e=lm_core_destroy(s->core);if(e)return e;s->core=NULL;
    }
    /* VM detach/reset proofs precede backing free, not merely a DONE bit.
     * Core destruction's final reset must succeed too, otherwise retain pages. */
    if(s->output_page.cpu)s->io.page_free(s->io.cookie,&s->output_page);
    for(unsigned i=0;i<LM_CLEAR_MAX_PAGES;i++)
        if(s->clear_pages[i].cpu)s->io.page_free(s->io.cookie,&s->clear_pages[i]);
    if(s->job_page.cpu)s->io.page_free(s->io.cookie,&s->job_page);
    s->cleaned=true;return LM_OK;
}
static int lm_private_dma_run(struct lm_private_dma *s,const struct lm_ops *io) {
    if(!s||!io||s->initialized)return LM_EINVAL;
    s->initialized=true;s->io=*io;s->error=LM_ENOMEM;
    s->core=lm_core_create(io);
    if(!s->core)return s->error;
    s->error=lm_core_start(s->core);if(s->error)goto cleanup;
    s->vm=lm_core_vm_new(s->core);
    if(!s->vm){s->error=LM_ENOMEM;goto cleanup;}
    if(s->clear_test) {
        unsigned surfaces=s->clear_chain?2u:1u;
        unsigned output_pages=surfaces*LM_CLEAR_PAGES;
        uint32_t color2=s->clear_value2?s->clear_value2:s->clear_value;
        s->error=io->page_alloc(io->cookie,&s->job_page);if(s->error)goto cleanup;
        for(unsigned i=0;i<output_pages;i++) {
            s->error=io->page_alloc(io->cookie,&s->clear_pages[i]);if(s->error)goto cleanup;
            if(s->clear_pages[i].pa==s->job_page.pa){s->error=LM_EINVAL;goto cleanup;}
            for(unsigned j=0;j<i;j++)
                if(s->clear_pages[i].pa==s->clear_pages[j].pa){s->error=LM_EINVAL;goto cleanup;}
        }
        lm_v7_clear_prepare(s->job_page.cpu,LM_PRIVATE_JOB_VA,LM_PRIVATE_OUTPUT_VA,
            s->clear_value,color2,surfaces);
        s->error=lm_core_vm_map(s->vm,LM_PRIVATE_JOB_VA,&s->job_page,1,LM_MAP_WRITE);
        if(!s->error)s->error=lm_core_vm_map(s->vm,LM_PRIVATE_OUTPUT_VA,s->clear_pages,output_pages,LM_MAP_WRITE);
        if(!s->error) {
            uint32_t flags=0;
            if(lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA,&flags)!=s->job_page.pa || flags!=LM_MAP_WRITE ||
               lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA-4096,NULL) ||
               lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA+4096,NULL) ||
               lm_core_vm_lookup(s->vm,LM_PRIVATE_OUTPUT_VA+output_pages*LM_PAGE_SIZE,NULL))s->error=LM_EFAULT;
            for(unsigned i=0;!s->error && i<output_pages;i++)
                if(lm_core_vm_lookup(s->vm,LM_PRIVATE_OUTPUT_VA+i*LM_PAGE_SIZE,&flags)!=
                   s->clear_pages[i].pa || flags!=LM_MAP_WRITE)s->error=LM_EFAULT;
        }
        if(s->error)goto cleanup;
        s->error=lm_core_submit_wait(s->vm,LM_PRIVATE_JOB_VA+LM_CLEAR_DESC_OFFSET,1,
            UINT64_C(100000000),&s->job_result);
        s->submitted=s->job_result.sequence!=0;
        if(!s->job_result.quarantined) {
            s->observed_value=((volatile uint32_t *)s->clear_pages[0].cpu)[0];
            s->descriptor_status=((volatile uint32_t *)s->job_page.cpu)[LM_CLEAR_DESC_OFFSET/4u+4u];
            s->guards_valid=lm_v7_clear_validate(s->clear_pages,output_pages,
                s->job_page.cpu,s->clear_value,color2,surfaces,
                &s->clear_bad_word,&s->clear_actual,&s->bad_job);
            if(!s->guards_valid) {
                uint32_t expected=(surfaces>1&&s->clear_bad_word>=LM_CLEAR_BYTES)?color2:s->clear_value;
                s->bad_offset=s->clear_bad_word;
                s->job_result.mismatch_reg=s->clear_bad_word;
                s->job_result.expected=expected;
                s->job_result.observed=s->clear_actual;
                if(!s->error)s->error=LM_EIO;
            }
        }
        goto cleanup;
    }
    s->error=io->page_alloc(io->cookie,&s->job_page);if(s->error)goto cleanup;
    s->error=io->page_alloc(io->cookie,&s->output_page);if(s->error)goto cleanup;
    if(!s->job_page.cpu||!s->output_page.cpu||s->job_page.pa==s->output_page.pa) {
        s->error=LM_EINVAL;goto cleanup;
    }
    /* Negative test touches only the already-verified unmapped guard VA. No
     * identity map, foreign PA or shared MMU mutation is ever introduced. */
    s->expected_fault_va=s->mmu_fault_test?LM_PRIVATE_OUTPUT_VA+4096+4u*LM_DMA_TARGET_WORD:0;
    lm_v7_dma_prepare(s->job_page.cpu,s->output_page.cpu,
        LM_PRIVATE_OUTPUT_VA+(s->mmu_fault_test?4096:0),LM_PRIVATE_VALUE);
    s->error=lm_core_vm_map(s->vm,LM_PRIVATE_JOB_VA,&s->job_page,1,LM_MAP_WRITE);
    if(!s->error)s->error=lm_core_vm_map(s->vm,LM_PRIVATE_OUTPUT_VA,&s->output_page,1,LM_MAP_WRITE);
    if(s->error)goto cleanup;
    uint32_t flags=0;
    if(lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA,&flags)!=s->job_page.pa || flags!=LM_MAP_WRITE ||
       lm_core_vm_lookup(s->vm,LM_PRIVATE_OUTPUT_VA,&flags)!=s->output_page.pa || flags!=LM_MAP_WRITE ||
       lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA-4096,NULL) ||
       lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA+4096,NULL) ||
       lm_core_vm_lookup(s->vm,LM_PRIVATE_OUTPUT_VA+4096,NULL)) {
        s->error=LM_EFAULT;goto cleanup;
    }
    s->error=lm_core_submit_wait(s->vm,LM_PRIVATE_JOB_VA,0,UINT64_C(100000000),&s->job_result);
    s->submitted=s->job_result.sequence!=0;
    if(!s->job_result.quarantined) {
        s->observed_value=((volatile uint32_t *)s->output_page.cpu)[LM_DMA_TARGET_WORD];
        s->descriptor_status=((volatile uint32_t *)s->job_page.cpu)[0];
    }
    if(!s->error || (s->mmu_fault_test&&s->job_result.reset_completed&&!s->job_result.quarantined)) {
        s->guards_valid=lm_v7_dma_validate(s->job_page.cpu,s->output_page.cpu,
            s->mmu_fault_test?~LM_PRIVATE_VALUE:LM_PRIVATE_VALUE,
            &s->bad_offset,&s->bad_job);
        if(!s->guards_valid)s->error=LM_EIO;
    }
cleanup:
    s->cleanup_error=lm_private_dma_cleanup(s);
    s->backing_retained=s->cleanup_error!=0;
    s->expected_fault=s->mmu_fault_test&&!s->clear_test&&s->submitted&&s->error==LM_EIO&&s->guards_valid&&s->cleaned&&
        !s->cleanup_error&&s->job_result.failed_stage==LM_JOB_WAIT&&s->job_result.reset_completed&&
        !s->job_result.quarantined&&!s->job_result.gpu_fault&&
        LM_AS_WRITE_TRANSLATION_FAULT(s->job_result.mmu_fault)&&
        s->job_result.fault_address==s->expected_fault_va;
    return s->error?s->error:s->cleanup_error;
}
#endif
