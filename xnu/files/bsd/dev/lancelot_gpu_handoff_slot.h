/* Kernel-owned normal-RAM handoff slot, not a resource/peer permission grant.
 * Loader locates only the dedicated writable Mach-O section. XNU consumes
 * its own compiled object, never maps or dereferences the locator's PA. */
#ifndef LANCELOT_GPU_HANDOFF_SLOT_H
#define LANCELOT_GPU_HANDOFF_SLOT_H
#include "lancelot_gpu_handoff.h"
#define LGH_SLOT_MAGIC 0x31544f4c5348474cULL
#define LGH_SLOT_VERSION 1u
#define LGH_SLOT_BUNDLE_OFFSET 64u
struct lgh_kernel_slot {
    lgh_u64 magic;
    lgh_u32 version,size;
    lgh_u64 reserved[6];
    struct lgh_bundle bundle;
    lgh_u8 tail[48];
};
typedef char lgh_kernel_slot_size[(sizeof(struct lgh_kernel_slot)==320)?1:-1];
struct lgh_slot_region { lgh_u64 virtual_address,file_offset; };
static inline int lgh_bytes_zero(const void *p,lgh_u32 bytes) {
    const lgh_u8 *b=p;for(lgh_u32 i=0;i<bytes;i++)if(b[i])return 0;return 1;
}
static inline int lgh_bytes_same(const void *a,const void *b,lgh_u32 bytes) {
    const lgh_u8 *x=a,*y=b;for(lgh_u32 i=0;i<bytes;i++)if(x[i]!=y[i])return 0;return 1;
}
static inline int lgh_slot_header_valid(const struct lgh_kernel_slot *s) {
    return s&&s->magic==LGH_SLOT_MAGIC&&s->version==LGH_SLOT_VERSION&&s->size==sizeof(*s)&&
        lgh_bytes_zero(s->reserved,sizeof(s->reserved))&&lgh_bytes_zero(s->tail,sizeof(s->tail));
}
static inline int lgh_slot_template_valid(const struct lgh_kernel_slot *s) {
    return lgh_slot_header_valid(s)&&lgh_bytes_zero(&s->bundle,sizeof(s->bundle));
}
static inline lgh_u32 lgh_macho_u32(const lgh_u8 *p) {
    return (lgh_u32)p[0]|(lgh_u32)p[1]<<8|(lgh_u32)p[2]<<16|(lgh_u32)p[3]<<24;
}
static inline lgh_u64 lgh_macho_u64(const lgh_u8 *p) {
    return (lgh_u64)lgh_macho_u32(p)|(lgh_u64)lgh_macho_u32(p+4)<<32;
}
static inline int lgh_macho_name(const lgh_u8 *p,const char *s) {
    unsigned i=0;while(i<16&&s[i]){if(p[i]!=(lgh_u8)s[i])return 0;i++;}
    if(i==16)return 0;
    for(;i<16;i++)if(p[i])return 0;
    return 1;
}
static inline int lgh_range_inside(lgh_u64 lo,lgh_u64 size,lgh_u64 base,lgh_u64 bytes) {
    return size&&bytes&&base<=~(lgh_u64)0-bytes&&lo>=base&&lo-base<bytes&&size<=bytes-(lo-base);
}
static inline int lgh_range_overlap(lgh_u64 a,lgh_u64 as,lgh_u64 b,lgh_u64 bs) {
    return as&&bs&&a<=~(lgh_u64)0-as&&b<=~(lgh_u64)0-bs&&a<b+bs&&b<a+as;
}
/* Fixed little-endian arm64 Mach-O section lookup with independent bounds.
 * No magic scan, arbitrary PA, EFI descriptor assertion or executable alias.
 * This validates the slot's file/load provenance, not the entire boot chain. */
static inline int lgh_find_kernel_slot(const void *image,lgh_u64 bytes,struct lgh_slot_region *out) {
    if(!image||!out)return -22;
    lgh_zero(out,sizeof(*out));
    const lgh_u8 *p=image;
    if(bytes<32||lgh_macho_u32(p)!=0xfeedfacfu||lgh_macho_u32(p+4)!=0x0100000cu||
       lgh_macho_u32(p+12)!=2u)return -5;
    lgh_u32 count=lgh_macho_u32(p+16),commands=lgh_macho_u32(p+20);
    if(!count||count>256u||commands>bytes-32||count>commands/8u)return -5;
    lgh_u64 at=32,end=32+(lgh_u64)commands,owner=0,section_at=0;
    struct lgh_slot_region region={0};
    unsigned found=0;
    for(lgh_u32 command=0;command<count;command++) {
        if(end-at<8)return -5;
        lgh_u32 kind=lgh_macho_u32(p+at),length=lgh_macho_u32(p+at+4);
        if(length<8||(length&7u)||length>end-at)return -5;
        if(kind==0x19u) {
            if(length<72)return -5;
            lgh_u64 va=lgh_macho_u64(p+at+24),size=lgh_macho_u64(p+at+32);
            lgh_u64 file=lgh_macho_u64(p+at+40),file_bytes=lgh_macho_u64(p+at+48);
            lgh_u32 sections=lgh_macho_u32(p+at+64);
            if(sections>(length-72)/80u||va>~(lgh_u64)0-size||file>bytes||file_bytes>bytes-file||file_bytes>size)return -5;
            for(lgh_u32 index=0;index<sections;index++) {
                lgh_u64 s=at+72+(lgh_u64)index*80;
                if(!lgh_macho_name(p+s,"__lghhandoff"))continue;
                if(++found!=1||!lgh_macho_name(p+at+8,"__DATA")||!lgh_macho_name(p+s+16,"__DATA")||
                   lgh_macho_u32(p+at+60)!=3u)return -5;
                lgh_u64 sva=lgh_macho_u64(p+s+32),ssize=lgh_macho_u64(p+s+40);
                lgh_u32 soff=lgh_macho_u32(p+s+48),align=lgh_macho_u32(p+s+52),flags=lgh_macho_u32(p+s+64);
                if(ssize!=sizeof(struct lgh_kernel_slot)||(sva&63u)||(soff&63u)||align<6u||align>14u||flags||
                   !lgh_range_inside(sva,ssize,va,file_bytes)||soff!=(file+sva-va)||
                   !lgh_range_inside(soff,ssize,0,bytes))return -5;
                struct lgh_kernel_slot template;
                lgh_u8 *copy=(lgh_u8 *)&template;
                for(unsigned i=0;i<sizeof(template);i++)copy[i]=p[soff+i];
                if(!lgh_slot_template_valid(&template))return -5;
                region.virtual_address=sva;region.file_offset=soff;owner=at;section_at=s;
            }
        }
        at+=length;
    }
    if(at!=end||found!=1)return -5;
    /* Reject overlapping segments/sections which would overwrite the slot
     * when the loader materializes the other loadable content. */
    at=32;
    for(lgh_u32 command=0;command<count;command++) {
        lgh_u32 length=lgh_macho_u32(p+at+4);
        if(lgh_macho_u32(p+at)==0x19u) {
            lgh_u64 va=lgh_macho_u64(p+at+24),size=lgh_macho_u64(p+at+32);
            if(at!=owner&&lgh_range_overlap(va,size,region.virtual_address,sizeof(struct lgh_kernel_slot)))return -5;
            if(at==owner)for(lgh_u32 index=0;index<lgh_macho_u32(p+at+64);index++) {
                lgh_u64 s=at+72+(lgh_u64)index*80;
                if(s==section_at)continue;
                lgh_u64 sva=lgh_macho_u64(p+s+32),ssize=lgh_macho_u64(p+s+40);
                if(sva>~(lgh_u64)0-ssize||lgh_range_overlap(sva,ssize,region.virtual_address,sizeof(struct lgh_kernel_slot)))return -5;
            }
        }
        at+=length;
    }
    *out=region;return 0;
}
static inline int lgh_slot_physical(const struct lgh_slot_region *s,lgh_u64 virtual_base,
    lgh_u64 physical_base,lgh_u64 physical_size,lgh_u64 *out) {
    if(!out)return -22;
    *out=0;
    if(!s||!physical_base||(physical_base&63u)||s->virtual_address<virtual_base||
       physical_base>~(lgh_u64)0-physical_size)return -5;
    lgh_u64 offset=s->virtual_address-virtual_base;
    if((offset&63u)||offset>=physical_size||sizeof(struct lgh_kernel_slot)>physical_size-offset)return -5;
    *out=physical_base+offset;return 0;
}
/* Exact local address match BEFORE touching the volatile slot. sync is an
 * actual adapter barrier; two explicit byte reads reject torn offer updates.
 * CRC/source fields are format facts, not authentication or permissions. */
static inline int lgh_consume_kernel_slot(const struct lgh_locator *l,lgh_u64 boot,
    lgh_u64 own_bundle_pa,const volatile struct lgh_kernel_slot *slot,
    void (*sync)(void *),void *cookie,struct lgh_offer *out) {
    if(!out)return -22;
    lgh_zero(out,sizeof(*out));
    if(!l||!slot||!sync||!boot||!own_bundle_pa||(own_bundle_pa&63u)||
       own_bundle_pa>~(lgh_u64)0-sizeof(struct lgh_bundle)||l->magic!=LGH_LOCATOR_MAGIC||
       l->boot_id!=boot||l->bytes!=sizeof(struct lgh_bundle)||l->physical!=own_bundle_pa)return -5;
    struct lgh_kernel_slot a,b;
    const volatile lgh_u8 *source=(const volatile lgh_u8 *)slot;
    sync(cookie);for(unsigned i=0;i<sizeof(a);i++)((lgh_u8 *)&a)[i]=source[i];
    sync(cookie);for(unsigned i=0;i<sizeof(b);i++)((lgh_u8 *)&b)[i]=source[i];
    if(!lgh_bytes_same(&a,&b,sizeof(a))||!lgh_slot_header_valid(&a)||
       !lgh_bytes_zero(&a.bundle.returned,sizeof(a.bundle.returned))||!lgh_offer_valid(&a.bundle.offer,boot))return -5;
    *out=a.bundle.offer;return 0;
}
#endif
