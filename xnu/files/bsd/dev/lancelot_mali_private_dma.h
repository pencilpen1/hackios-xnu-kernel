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

/* Loader-only triangle capability test. Three jobs run in one chain:
 *   VERTEX (compute job, type 5) -> TILER (type 7) -> FRAGMENT (type 9).
 * The descriptor page layout is the Mesa-generated reference in
 * local/mesa-bifrost-host/triangle-ref (Mesa 25.0.7, v7 packers). Page A is
 * writable descriptor data; page B holds the shader binaries and is mapped
 * WRITE|EXEC. The output is the same 64x64 RGBA8 surface used by the clear
 * tests. The fragment shader writes (0.8, 0.4, 0.2, 1.0), which converts to
 * 0xff3366cc - exactly the clear colour, so every output word must match. */
#define LM_TRI_WIDTH 64u
#define LM_TRI_HEIGHT 64u
#define LM_TRI_BPP 4u
#define LM_TRI_BYTES (LM_TRI_WIDTH*LM_TRI_HEIGHT*LM_TRI_BPP)
#define LM_TRI_PAGES (LM_TRI_BYTES/LM_PAGE_SIZE)
#define LM_TRI_HEAP_BYTES 16384u
#define LM_TRI_HEAP_PAGES (LM_TRI_HEAP_BYTES/LM_PAGE_SIZE)
#define LM_TRI_VERTEX_COUNT 3u
#define LM_TRI_VALUE LM_CLEAR_VALUE
#define LM_TRI_SHADER_PAGE_VA (LM_PRIVATE_JOB_VA+UINT64_C(0x1000))
#define LM_TRI_HEAP_VA (LM_PRIVATE_JOB_VA+UINT64_C(0x8000))
#define LM_TRI_VERTEX_JOB 0x000u
#define LM_TRI_TILER_JOB 0x100u
#define LM_TRI_FRAG_JOB 0x200u
#define LM_TRI_VERT_RSD 0x240u
#define LM_TRI_FRAG_RSD 0x280u
#define LM_TRI_FRAG_BLEND 0x2C0u
#define LM_TRI_TILER_CTX 0x300u
#define LM_TRI_TILER_HEAP 0x3C0u
#define LM_TRI_VIEWPORT 0x400u
#define LM_TRI_VATTR 0x420u
#define LM_TRI_VATTR_BUF 0x440u
#define LM_TRI_VARY_ATTR 0x460u
#define LM_TRI_VARY_BUF 0x480u
#define LM_TRI_POS_BUF 0x4C0u
#define LM_TRI_PSIZ_BUF 0x500u
#define LM_TRI_VERTEX_DATA 0x540u
#define LM_TRI_FBD 0x800u
#define LM_TRI_RT 0x880u
#define LM_TRI_A_GUARD 0x900u
#define LM_TRI_B_GUARD 0x200u
#define LM_TRI_A_GUARD_WORD UINT32_C(0x54524947)
#define LM_TRI_B_GUARD_WORD UINT32_C(0x53484452)

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
    bool triangle_test;
    uint32_t clear_value,clear_value2;
    uint64_t expected_fault_va;
    const void *tri_vertex_shader;
    const void *tri_fragment_shader;
    unsigned tri_vertex_shader_bytes,tri_fragment_shader_bytes;
    struct lm_page tri_pages[2];
    struct lm_page tri_output[LM_TRI_PAGES];
    struct lm_page tri_heap[LM_TRI_HEAP_PAGES];
    uint32_t tri_bad_word,tri_actual;
    unsigned tri_bad_stage;
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

static inline void lm_v7_tri_header(uint32_t *j,unsigned type,unsigned index,
    unsigned dep1,unsigned dep2,uint64_t next) {
    j[4]=1u|((uint32_t)type<<1)|((uint32_t)index<<16);
    j[5]=dep1|(dep2<<16);
    j[6]=(uint32_t)next;j[7]=(uint32_t)(next>>32);
}
static inline void lm_v7_tri_invocation(uint32_t *p,unsigned vertices) {
    p[0]=vertices-1u;
    p[1]=(32u<<22)|(2u<<28); /* z shift 32, MIN_EFFICIENT split */
}
static inline void lm_v7_tri_draw(uint32_t *d,uint64_t position,uint64_t state,
    uint64_t attribute_buffers,uint64_t attributes,uint64_t varying_buffers,
    uint64_t varyings,uint64_t viewport) {
    /* four_components_per_vertex, 64-bit descriptor, front_face_ccw; the
     * padded instance sizes are 1 and therefore pack to zero. */
    d[0]=0x23u;
    d[4]=(uint32_t)position;d[5]=(uint32_t)(position>>32);
    d[14]=(uint32_t)state;d[15]=(uint32_t)(state>>32);
    d[16]=(uint32_t)attribute_buffers;d[17]=(uint32_t)(attribute_buffers>>32);
    d[18]=(uint32_t)attributes;d[19]=(uint32_t)(attributes>>32);
    d[20]=(uint32_t)varying_buffers;d[21]=(uint32_t)(varying_buffers>>32);
    d[22]=(uint32_t)varyings;d[23]=(uint32_t)(varyings>>32);
    d[24]=(uint32_t)viewport;d[25]=(uint32_t)(viewport>>32);
}
static inline void lm_v7_tri_attr(uint32_t *p,unsigned buffer,uint32_t format) {
    p[0]=(format<<10)|(1u<<9)|buffer;p[1]=0;
}
static inline void lm_v7_tri_attr_buffer(uint32_t *p,uint64_t pointer,
    uint32_t stride,uint32_t size) {
    uint64_t q=pointer>>6;
    p[0]=1u|((uint32_t)q<<6); /* 1D + low pointer bits */
    p[1]=(uint32_t)(q>>26);   /* high pointer bits; divisor 1 packs to zero */
    p[2]=stride;p[3]=size;
}
static inline void lm_v7_tri_vertex_rsd(uint32_t *r,uint64_t shader) {
    memset(r,0,64);
    r[0]=(uint32_t)shader;r[1]=(uint32_t)(shader>>32);
    r[3]=0x00020001u; /* attribute_count=1, varying_count=2 (POS, PSIZ) */
    r[4]=0x00002000u; /* 32 registers per thread */
    r[12]=0x00006000u; /* preload VertexID and InstanceID */
}
static inline void lm_v7_tri_fragment_rsd(uint32_t *r,uint64_t shader) {
    memset(r,0,64);
    r[0]=(uint32_t)shader;r[1]=(uint32_t)(shader>>32);
    /* depth source fixed-function, 32 regs, forward-kill allowed, wait6.
     * Weak-early kill/update, RGBA8 sample mask, depth Always, clip flags. */
    r[4]=0x41502200u;
    r[8]=0x37d8ffffu;
    r[9]=0x40000000u;
    r[12]=0x00000080u; /* coverage preload */
}
static inline void lm_v7_tri_blend(uint32_t *b) {
    b[0]=0x00000a00u; /* enable + round to FB precision (dithering off) */
    b[1]=0xf0122122u; /* replace: src + src*zero, RGBA mask */
    b[2]=0x00000019u; /* opaque, 4 components */
    b[3]=0x010ed000u; /* RGBA8 tilebuffer conversion, F32 register file */
}
static inline void lm_v7_tri_tiler_context(uint32_t *c,uint64_t heap) {
    memset(c,0,192);
    c[2]=6u; /* hierarchy_mask for 64x64, max_levels=2; single-sampled */
    c[3]=((LM_TRI_WIDTH-1u)&0xffffu)|(((LM_TRI_HEIGHT-1u)&0xffffu)<<16);
    c[6]=(uint32_t)heap;c[7]=(uint32_t)(heap>>32);
}
static inline void lm_v7_tri_tiler_heap(uint32_t *h,uint64_t base) {
    memset(h,0,32);
    h[1]=LM_TRI_HEAP_BYTES;
    h[2]=(uint32_t)base;h[3]=(uint32_t)(base>>32);
    h[4]=(uint32_t)base;h[5]=(uint32_t)(base>>32);
    h[6]=(uint32_t)(base+LM_TRI_HEAP_BYTES);
    h[7]=(uint32_t)((base+LM_TRI_HEAP_BYTES)>>32);
}
static inline void lm_v7_tri_viewport(uint32_t *v) {
    memset(v,0,32);
    v[2]=0x42800000u;v[3]=0x42800000u; /* 64.0f, 64.0f */
    v[5]=0x3f800000u;                  /* 1.0f */
    v[7]=((LM_TRI_WIDTH-1u)&0xffffu)|(((LM_TRI_HEIGHT-1u)&0xffffu)<<16);
}
static inline void lm_v7_tri_fbd(uint32_t *f,uint64_t tiler) {
    memset(f,0,128);
    f[8]=((LM_TRI_WIDTH-1u)&0xffffu)|(((LM_TRI_HEIGHT-1u)&0xffffu)<<16);
    f[10]=f[8];
    f[11]=(2u<<6)|(8u<<9)|(1u<<24);
    f[12]=1u<<16; /* D24, no Z/S write */
    f[14]=(uint32_t)tiler;f[15]=(uint32_t)(tiler>>32);
}
static inline void lm_v7_tri_rt(uint32_t *rt,uint64_t output) {
    memset(rt,0,64);
    rt[0]=1u<<26; /* RGBA8 tilebuffer */
    /* write enable, RGBA8 linear writeback, identity swizzle, clean write,
     * dithering disabled so the shader's exact 0xcc/0x66/0x33/0xff lands. */
    rt[1]=1u|(19u<<3)|(2u<<8)|(UINT32_C(0x688)<<16)|(1u<<31);
    rt[8]=(uint32_t)output;rt[9]=(uint32_t)(output>>32);
    rt[10]=LM_TRI_WIDTH*LM_TRI_BPP;
}
static inline void lm_v7_tri_prepare(uint32_t *a,uint8_t *b,uint64_t a_va,
    uint64_t b_va,uint64_t output,uint64_t heap,const void *vs,unsigned vn,
    const void *fs,unsigned fn) {
    memset(a,0,LM_PAGE_SIZE);memset(b,0,LM_PAGE_SIZE);
    memcpy(b+0x000,vs,vn);memcpy(b+0x100,fs,fn);
    uint64_t ctx=a_va+LM_TRI_TILER_CTX;
    uint64_t heap_desc=a_va+LM_TRI_TILER_HEAP;
    uint64_t vshader=b_va+0x000,fshader=b_va+0x100;
    /* VERTEX job: compute aggregate, type 5, depends on nothing. */
    uint32_t *vj=a+LM_TRI_VERTEX_JOB/4;
    lm_v7_tri_header(vj,5,1,0,0,a_va+LM_TRI_TILER_JOB);
    lm_v7_tri_invocation(vj+8,LM_TRI_VERTEX_COUNT);
    vj[10]=5u<<26; /* job_task_split=5 */
    lm_v7_tri_draw(vj+16,0,a_va+LM_TRI_VERT_RSD,a_va+LM_TRI_VATTR_BUF,
        a_va+LM_TRI_VATTR,a_va+LM_TRI_VARY_BUF,a_va+LM_TRI_VARY_ATTR,0);
    /* TILER job, depends on the vertex job. */
    uint32_t *tj=a+LM_TRI_TILER_JOB/4;
    lm_v7_tri_header(tj,7,2,1,0,a_va+LM_TRI_FRAG_JOB);
    lm_v7_tri_invocation(tj+8,LM_TRI_VERTEX_COUNT);
    tj[10]=0x18030008u; /* Triangles, both depth culls, job_task_split=6 */
    tj[13]=LM_TRI_VERTEX_COUNT-1u;
    tj[16]=(uint32_t)(a_va+LM_TRI_PSIZ_BUF);
    tj[17]=(uint32_t)((a_va+LM_TRI_PSIZ_BUF)>>32);
    tj[18]=(uint32_t)ctx;tj[19]=(uint32_t)(ctx>>32);
    lm_v7_tri_draw(tj+32,0,a_va+LM_TRI_FRAG_RSD,0,0,0,0,a_va+LM_TRI_VIEWPORT);
    /* Keep the fragment draw's position result buffer. */
    tj[32+4]=(uint32_t)(a_va+LM_TRI_POS_BUF);
    tj[32+5]=(uint32_t)((a_va+LM_TRI_POS_BUF)>>32);
    /* FRAGMENT job, depends on tiling. */
    uint32_t *fj=a+LM_TRI_FRAG_JOB/4;
    lm_v7_tri_header(fj,9,3,2,0,0);
    fj[9]=((LM_TRI_WIDTH/16u)-1u)|((((LM_TRI_HEIGHT/16u)-1u))<<16);
    fj[10]=(uint32_t)(a_va+LM_TRI_FBD);
    fj[11]=(uint32_t)((a_va+LM_TRI_FBD)>>32);
    /* Descriptors. */
    lm_v7_tri_vertex_rsd(a+LM_TRI_VERT_RSD/4,vshader);
    lm_v7_tri_fragment_rsd(a+LM_TRI_FRAG_RSD/4,fshader);
    lm_v7_tri_blend(a+LM_TRI_FRAG_BLEND/4);
    lm_v7_tri_tiler_context(a+LM_TRI_TILER_CTX/4,heap_desc);
    lm_v7_tri_tiler_heap(a+LM_TRI_TILER_HEAP/4,heap);
    lm_v7_tri_viewport(a+LM_TRI_VIEWPORT/4);
    lm_v7_tri_attr(a+LM_TRI_VATTR/4,0,16u|(UINT32_C(175)<<12)); /* RG32F */
    lm_v7_tri_attr_buffer(a+LM_TRI_VATTR_BUF/4,a_va+LM_TRI_VERTEX_DATA,8,24);
    memset(a+LM_TRI_VATTR_BUF/4+4,0,16); /* Bifrost prefetch terminator */
    lm_v7_tri_attr(a+LM_TRI_VARY_ATTR/4,1,UINT32_C(94)<<12);  /* position */
    lm_v7_tri_attr(a+LM_TRI_VARY_ATTR/4+2,2,UINT32_C(199)<<12); /* psiz */
    memset(a+LM_TRI_VARY_BUF/4,0,64);
    lm_v7_tri_attr_buffer(a+LM_TRI_VARY_BUF/4+4,a_va+LM_TRI_POS_BUF,16,48);
    lm_v7_tri_attr_buffer(a+LM_TRI_VARY_BUF/4+8,a_va+LM_TRI_PSIZ_BUF,2,6);
    lm_v7_tri_fbd(a+LM_TRI_FBD/4,ctx);
    lm_v7_tri_rt(a+LM_TRI_RT/4,output);
    /* Full-screen clip-space triangle. */
    int32_t verts[6]={0xbf800000,0xbf800000,0x40400000,0xbf800000,
        0xbf800000,0x40400000};
    memcpy(a+LM_TRI_VERTEX_DATA/4,verts,sizeof(verts));
    /* Guards after the used descriptor area and after the shader binaries. */
    for(unsigned i=LM_TRI_A_GUARD/4;i<LM_PAGE_SIZE/4u;i++)
        a[i]=LM_TRI_A_GUARD_WORD^i;
    for(unsigned i=LM_TRI_B_GUARD/4;i<LM_PAGE_SIZE/4u;i++)
        ((uint32_t *)b)[i]=LM_TRI_B_GUARD_WORD^i;
}
/* Return false at the first failure. stage: 0 output, 1 position, 2 psiz,
 * 3 page A guard, 4 page B guard. */
static inline bool lm_v7_tri_validate(const struct lm_page *output,unsigned n,
    const uint32_t *a,const uint8_t *b,uint32_t *bad_word,uint32_t *actual,
    unsigned *stage) {
    for(unsigned p=0;p<n;p++)
        for(unsigned i=0;i<LM_PAGE_SIZE/4u;i++) {
            uint32_t v=((const volatile uint32_t *)output[p].cpu)[i];
            if(v!=LM_TRI_VALUE) {
                *stage=0;*bad_word=p*LM_PAGE_SIZE+i*4u;*actual=v;return false;
            }
        }
    static const uint32_t want_pos[12]={0xbf800000,0xbf800000,0,0x3f800000,
        0x40400000,0xbf800000,0,0x3f800000,0xbf800000,0x40400000,0,0x3f800000};
    for(unsigned i=0;i<12;i++) {
        uint32_t v=a[LM_TRI_POS_BUF/4+i];
        if(v!=want_pos[i]) {
            *stage=1;*bad_word=i*4u;*actual=v;return false;
        }
    }
    for(unsigned i=0;i<LM_TRI_VERTEX_COUNT;i++) {
        uint16_t v=((const uint16_t *)((const uint8_t *)a+LM_TRI_PSIZ_BUF))[i];
        if(v!=0x3c00u) {
            *stage=2;*bad_word=LM_TRI_PSIZ_BUF+i*2u;*actual=v;return false;
        }
    }
    for(unsigned i=LM_TRI_A_GUARD/4;i<LM_PAGE_SIZE/4u;i++)
        if(a[i]!=(LM_TRI_A_GUARD_WORD^i)) {
            *stage=3;*bad_word=i*4u;*actual=a[i];return false;
        }
    for(unsigned i=LM_TRI_B_GUARD/4;i<LM_PAGE_SIZE/4u;i++)
        if(((const uint32_t *)b)[i]!=(LM_TRI_B_GUARD_WORD^i)) {
            *stage=4;*bad_word=i*4u;*actual=((const uint32_t *)b)[i];return false;
        }
    *stage=0;*bad_word=0;*actual=LM_TRI_VALUE;return true;
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
    for(unsigned i=0;i<2;i++)
        if(s->tri_pages[i].cpu)s->io.page_free(s->io.cookie,&s->tri_pages[i]);
    for(unsigned i=0;i<LM_TRI_PAGES;i++)
        if(s->tri_output[i].cpu)s->io.page_free(s->io.cookie,&s->tri_output[i]);
    for(unsigned i=0;i<LM_TRI_HEAP_PAGES;i++)
        if(s->tri_heap[i].cpu)s->io.page_free(s->io.cookie,&s->tri_heap[i]);
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
    if(s->triangle_test) {
        if(!s->tri_vertex_shader || !s->tri_fragment_shader ||
           !s->tri_vertex_shader_bytes || !s->tri_fragment_shader_bytes ||
           s->tri_vertex_shader_bytes>0x100u || s->tri_fragment_shader_bytes>0x100u) {
            s->error=LM_EINVAL;goto cleanup;
        }
        s->error=io->page_alloc(io->cookie,&s->tri_pages[0]);if(s->error)goto cleanup;
        s->error=io->page_alloc(io->cookie,&s->tri_pages[1]);if(s->error)goto cleanup;
        for(unsigned i=0;i<LM_TRI_PAGES;i++) {
            s->error=io->page_alloc(io->cookie,&s->tri_output[i]);if(s->error)goto cleanup;
        }
        for(unsigned i=0;i<LM_TRI_HEAP_PAGES;i++) {
            s->error=io->page_alloc(io->cookie,&s->tri_heap[i]);if(s->error)goto cleanup;
        }
        /* All backing must be distinct; a duplicate PA would alias a
         * descriptor, shader, output or heap page and invalidate the test. */
        for(unsigned i=0;i<2;i++)for(unsigned j=0;j<i;j++)
            if(s->tri_pages[i].pa==s->tri_pages[j].pa){s->error=LM_EINVAL;goto cleanup;}
        for(unsigned i=0;i<2;i++)for(unsigned j=0;j<LM_TRI_PAGES;j++)
            if(s->tri_pages[i].pa==s->tri_output[j].pa){s->error=LM_EINVAL;goto cleanup;}
        for(unsigned i=0;i<2;i++)for(unsigned j=0;j<LM_TRI_HEAP_PAGES;j++)
            if(s->tri_pages[i].pa==s->tri_heap[j].pa){s->error=LM_EINVAL;goto cleanup;}
        for(unsigned i=0;i<LM_TRI_PAGES;i++)for(unsigned j=0;j<i;j++)
            if(s->tri_output[i].pa==s->tri_output[j].pa){s->error=LM_EINVAL;goto cleanup;}
        for(unsigned i=0;i<LM_TRI_PAGES;i++)for(unsigned j=0;j<LM_TRI_HEAP_PAGES;j++)
            if(s->tri_output[i].pa==s->tri_heap[j].pa){s->error=LM_EINVAL;goto cleanup;}
        for(unsigned i=0;i<LM_TRI_HEAP_PAGES;i++)for(unsigned j=0;j<i;j++)
            if(s->tri_heap[i].pa==s->tri_heap[j].pa){s->error=LM_EINVAL;goto cleanup;}
        lm_v7_tri_prepare(s->tri_pages[0].cpu,s->tri_pages[1].cpu,LM_PRIVATE_JOB_VA,
            LM_TRI_SHADER_PAGE_VA,LM_PRIVATE_OUTPUT_VA,LM_TRI_HEAP_VA,
            s->tri_vertex_shader,s->tri_vertex_shader_bytes,
            s->tri_fragment_shader,s->tri_fragment_shader_bytes);
        s->error=lm_core_vm_map(s->vm,LM_PRIVATE_JOB_VA,&s->tri_pages[0],1,LM_MAP_WRITE);
        if(!s->error)s->error=lm_core_vm_map(s->vm,LM_TRI_SHADER_PAGE_VA,&s->tri_pages[1],1,
            LM_MAP_WRITE|LM_MAP_EXEC);
        if(!s->error)s->error=lm_core_vm_map(s->vm,LM_PRIVATE_OUTPUT_VA,s->tri_output,
            LM_TRI_PAGES,LM_MAP_WRITE);
        if(!s->error)s->error=lm_core_vm_map(s->vm,LM_TRI_HEAP_VA,s->tri_heap,
            LM_TRI_HEAP_PAGES,LM_MAP_WRITE);
        if(!s->error) {
            uint32_t flags=0;
            if(lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA,&flags)!=s->tri_pages[0].pa ||
               flags!=LM_MAP_WRITE ||
               lm_core_vm_lookup(s->vm,LM_TRI_SHADER_PAGE_VA,&flags)!=s->tri_pages[1].pa ||
               flags!=(LM_MAP_WRITE|LM_MAP_EXEC) ||
               lm_core_vm_lookup(s->vm,LM_PRIVATE_JOB_VA-4096,NULL) ||
               lm_core_vm_lookup(s->vm,LM_TRI_HEAP_VA+LM_TRI_HEAP_PAGES*LM_PAGE_SIZE,NULL))
                s->error=LM_EFAULT;
            for(unsigned i=0;!s->error && i<LM_TRI_PAGES;i++)
                if(lm_core_vm_lookup(s->vm,LM_PRIVATE_OUTPUT_VA+i*LM_PAGE_SIZE,&flags)!=
                   s->tri_output[i].pa || flags!=LM_MAP_WRITE)s->error=LM_EFAULT;
            for(unsigned i=0;!s->error && i<LM_TRI_HEAP_PAGES;i++)
                if(lm_core_vm_lookup(s->vm,LM_TRI_HEAP_VA+i*LM_PAGE_SIZE,&flags)!=
                   s->tri_heap[i].pa || flags!=LM_MAP_WRITE)s->error=LM_EFAULT;
        }
        if(s->error)goto cleanup;
        s->error=lm_core_submit_wait(s->vm,LM_PRIVATE_JOB_VA,1,
            UINT64_C(100000000),&s->job_result);
        s->submitted=s->job_result.sequence!=0;
        if(!s->job_result.quarantined) {
            s->observed_value=((volatile uint32_t *)s->tri_output[0].cpu)[0];
            s->guards_valid=lm_v7_tri_validate(s->tri_output,LM_TRI_PAGES,
                s->tri_pages[0].cpu,s->tri_pages[1].cpu,&s->tri_bad_word,
                &s->tri_actual,&s->tri_bad_stage);
            if(!s->guards_valid) {
                s->bad_offset=s->tri_bad_word;
                s->job_result.mismatch_reg=s->tri_bad_word;
                s->job_result.expected=LM_TRI_VALUE;
                s->job_result.observed=s->tri_actual;
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
