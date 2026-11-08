/* Same-boot GPU firmware lease ABI. AP_ONLY is deliberately NOT a DVFS grant.
 * Kernel publishes return proof only after real JS/AS/DMA/backing/AP quiescence.
 * No hardware access, resource acquisition, fake sensor or peer ACK here. */
#ifndef LANCELOT_GPU_HANDOFF_H
#define LANCELOT_GPU_HANDOFF_H
#ifdef LGH_EDK2
#include <Base.h>
typedef UINT8 lgh_u8; typedef UINT32 lgh_u32; typedef UINT64 lgh_u64;
#else
#include <stdint.h>
typedef uint8_t lgh_u8; typedef uint32_t lgh_u32; typedef uint64_t lgh_u64;
#endif
#define LGH_MAGIC 0x31455341454c5047ULL
#define LGH_RETURN_MAGIC 0x315445525550474cULL
#define LGH_LOCATOR_MAGIC 0x31434f4c5047484cULL
#define LGH_VERSION 1u
#define LGH_MFGPLL 1u
#define LGH_MFGMUX 2u
#define LGH_GPU_RAILS 4u
#define LGH_MAINPLL 8u
#define LGH_FMETER 16u
#define LGH_RESOURCES 31u
#define LGH_AP_LOCKED 1u
#define LGH_MAIN_HELD 2u
#define LGH_ALL_PEERS_EXCLUSIVE 4u /* This producer NEVER sets this bit. */
#define LGH_IDLE 0u
#define LGH_PREPARED 1u
#define LGH_SEALED 2u
#define LGH_UNKNOWN 3u
#define LGH_RELEASED 4u
#define LGH_PEER_UNVERIFIED 0u
#define LGH_RETURN_SAFE 1u
#define LGH_RET_GPU_IDLE 1u
#define LGH_RET_AS_DETACHED 2u
#define LGH_RET_DMA_SAFE 4u
#define LGH_RET_BACKING_SAFE 8u
#define LGH_RET_APS_OFF 16u
#define LGH_RET_CLOCK_RAILS 32u
#define LGH_RET_REQUIRED 63u
struct lgh_offer {
    lgh_u64 magic, boot_id, nonce;
    lgh_u32 version, size, state, resources;
    lgh_u32 capabilities, peer_state, main_refs, crc;
    lgh_u64 main_hz;
    lgh_u32 main_con0, main_con1, main_con3, boot_mpidr;
    lgh_u32 other_ap_off_mask;
    lgh_u8 baseline_mu_commit[20];
    lgh_u32 reserved[2];
};
struct lgh_return {
    lgh_u64 magic, boot_id, nonce;
    lgh_u32 version, size, state, flags;
    lgh_u32 job_state, as_active_mask, other_ap_off_mask, crc;
    lgh_u64 gpu_hz;
    lgh_u32 clock_verified, rails_verified, vgpu_uv, vsram_uv;
    lgh_u32 mfg_parent, mfg_con1, measured_hz, reserved;
};
struct lgh_bundle { struct lgh_offer offer; struct lgh_return returned; };
struct lgh_locator { lgh_u64 magic, physical, bytes, boot_id; };
typedef char lgh_offer_size[(sizeof(struct lgh_offer)==112)?1:-1];
typedef char lgh_return_size[(sizeof(struct lgh_return)==96)?1:-1];
static inline void lgh_zero(void *p, lgh_u32 n) { lgh_u8 *b=p;for(lgh_u32 i=0;i<n;i++)b[i]=0; }
static inline lgh_u32 lgh_crc(const void *p,lgh_u32 bytes,lgh_u32 skip) {
    const lgh_u8 *b=p;lgh_u32 crc=~0u;
    for(lgh_u32 i=0;i<bytes;i++) {
        crc^=i>=skip&&i<skip+4?0:b[i];
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}
static inline void lgh_seal_offer(struct lgh_offer *o) { o->crc=lgh_crc(o,sizeof(*o),52); }
static inline int lgh_offer_valid(const struct lgh_offer *o,lgh_u64 boot) {
    static const lgh_u8 pin[20]={0x83,0xf6,0x41,0xfd,0x2a,0x64,0x29,0x68,0xaa,0xc4,0xfb,0x36,0x98,0x69,0x56,0xdb,0x70,0x76,0x11,0x8e};
    if(!o)return 0;
    for(unsigned i=0;i<20;i++)if(o->baseline_mu_commit[i]!=pin[i])return 0;
    return o && o->magic==LGH_MAGIC && o->version==LGH_VERSION && o->size==sizeof(*o) &&
        boot && o->boot_id==boot && o->nonce && o->state==LGH_SEALED &&
        o->resources==LGH_RESOURCES && o->capabilities==(LGH_AP_LOCKED|LGH_MAIN_HELD) &&
        o->peer_state==LGH_PEER_UNVERIFIED && o->main_refs==1 &&
        o->boot_mpidr<=0x700u && !(o->boot_mpidr&255u) &&
        o->other_ap_off_mask==(255u^(1u<<(o->boot_mpidr>>8))) &&
        (o->main_con0&1u) && (o->main_con3&3u)==1u && o->main_hz &&
        !o->reserved[0] && !o->reserved[1] && o->crc==lgh_crc(o,sizeof(*o),52);
}
/* Explicit refusal until a DIFFERENT, real peer-capable producer/ABI exists.
 * Identity, a CRC and user authorization are not peripheral permissions. */
static inline int lgh_dvfs_granted(const struct lgh_offer *o) { (void)o;return 0; }
static inline void lgh_seal_return(struct lgh_return *r) { r->crc=lgh_crc(r,sizeof(*r),52); }
static inline int lgh_return_valid(const struct lgh_offer *o,const struct lgh_return *r) {
    if(!o||!r||!lgh_offer_valid(o,o->boot_id)||r->magic!=LGH_RETURN_MAGIC||r->version!=1||
       r->size!=sizeof(*r)||r->boot_id!=o->boot_id||r->nonce!=o->nonce||r->state!=LGH_RETURN_SAFE||
       r->flags!=LGH_RET_REQUIRED||r->job_state||r->as_active_mask||
       r->other_ap_off_mask!=o->other_ap_off_mask||r->clock_verified!=1||r->rails_verified!=1||
       r->mfg_parent!=1||r->reserved||r->crc!=lgh_crc(r,sizeof(*r),52))return 0;
    lgh_u32 g=r->vgpu_uv,s=r->vsram_uv,d=(r->mfg_con1>>24)&7u;
    if(g<612500u||g>950000u||s<850000u||s>1050000u||s<g||s-g<100000u||s-g>250000u||
       (g-500000u)%6250u||(s-500000u)%6250u||d>4u||!(r->mfg_con1&0x3fffffu))return 0;
    lgh_u64 den=16384ULL*(1u<<d),hz=(26000000ULL*(r->mfg_con1&0x3fffffu)+den-1)/den;
    lgh_u64 diff=hz>r->measured_hz?hz-r->measured_hz:r->measured_hz-hz;
    static const lgh_u32 khz[30]={950000,925000,900000,875000,850000,823000,796000,769000,743000,716000,690000,663000,637000,611000,586000,560000,535000,509000,484000,467000,450000,434000,417000,400000,383000,366000,349000,332000,315000,299000};
    static const lgh_u32 uv[30]={900000,875000,850000,825000,800000,793750,781250,768750,756250,750000,737500,725000,712500,706250,700000,693750,687500,681250,668750,668750,662500,656250,650000,643750,643750,637500,631250,625000,618750,612500};
    lgh_u32 floor=900000;
    for(unsigned i=30;i-- >0;)if(hz<=(lgh_u64)khz[i]*1000u){floor=uv[i];break;}
    return hz==r->gpu_hz&&hz>=298999000ULL&&hz<=950000000ULL&&r->measured_hz&&diff<=102000u&&g>=floor;
}
/* Copy explicitly produced proof and seal; does NOT manufacture any flag. */
static inline int lgh_publish_return(struct lgh_bundle *b,const struct lgh_return *proof) {
    if(!b||!proof)return -22;
    struct lgh_return r=*proof;lgh_seal_return(&r);
    if(!lgh_return_valid(&b->offer,&r))return -5;
    b->returned=r;return 0; /* adapter must complete cache clean + barrier */
}
#endif
