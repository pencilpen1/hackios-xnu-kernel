#ifndef LANCELOT_PRESENT_CACHE_HOOK_H
#define LANCELOT_PRESENT_CACHE_HOOK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LP_STUB_LOW 0x6a4u
#define LP_SWAP_ENTRY_OFFSET 0x1530u
struct lp_vm_region {
    uintptr_t start, bytes;
    uint32_t depth, protection;
    int submap, error;
};
typedef int (*lp_vm_query)(uintptr_t, uint32_t, struct lp_vm_region *, void *);
static inline int lp_copy_code_page(void *copy,const void *source,size_t bytes,size_t offset,
                                    uint32_t before,uint32_t after) {
    if(!copy || !source || copy==source || (offset&3) || offset>bytes || bytes-offset<4)return 0;
    memcpy(copy,source,bytes);uint32_t value;memcpy(&value,(const uint8_t *)copy+offset,4);
    if(value!=before)return 0;
    memcpy((uint8_t *)copy+offset,&after,4);return 1;
}
static inline int lp_code_page_unchanged(const void *source,const void *copy,size_t bytes,
                                         size_t offset,uint32_t before) {
    if(!copy || !source || offset>bytes || bytes-offset<4)return 0;
    uint32_t value;memcpy(&value,(const uint8_t *)source+offset,4);
    return value==before && !memcmp(source,copy,offset) &&
        !memcmp((const uint8_t *)source+offset+4,(const uint8_t *)copy+offset+4,bytes-offset-4);
}
/* VM_REGION_BASIC_INFO_64 stops at the shared-region container, which XNU
 * maps read-only. Ask recursively for the actual code mapping. Keep the
 * target address on every descent and reject holes or a partial code page. */
static inline int lp_executable_page(uintptr_t target,size_t page_bytes,uint32_t execute,
                                     lp_vm_query query,void *context,struct lp_vm_region *out) {
    if(!out || !query || !page_bytes || (page_bytes&(page_bytes-1)))return 0;
    uintptr_t page=target&~(uintptr_t)(page_bytes-1);
    uint32_t depth=0;
    for(unsigned i=0;i<16;i++) {
        *out=(struct lp_vm_region){0};
        if(!query(target,depth,out,context) || out->start>target || target-out->start>=out->bytes)return 0;
        if(out->submap) {
            if(out->depth<depth || out->depth>=16)return 0;
            depth=out->depth+1;continue;
        }
        return (out->protection&execute)==execute && out->start<=page &&
            page-out->start<=out->bytes && page_bytes<=out->bytes-(page-out->start);
    }
    return 0;
}
static inline int lp_swap_entry(const uint32_t *code) {
    static const uint32_t expected[11]={0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd,
        0xaa0003f3,0xb9401400,0x91006262,0x528000a1,0x52808d83,0xd2800004,0xd2800005};
    for(unsigned i=0;i<11;i++)if(code[i]!=expected[i])return 0;
    return (code[11]&0xfc000000u)==0x94000000u;
}
static inline uintptr_t lp_call_target(uintptr_t pc,uint32_t word) {
    int64_t imm=word&0x03ffffffu;if(imm&(1ll<<25))imm-=1ll<<26;
    return (uintptr_t)((int64_t)pc+imm*4);
}
static inline int lp_stub_destination(uintptr_t stub,const uint32_t code[3],uintptr_t *target) {
    if(!target || (stub&3) || (code[0]&0x9f00001fu)!=0x90000010u ||
       (code[1]&0xffc003ffu)!=0x91000210u || code[2]!=0xd61f0200u)return 0;
    uint32_t low=(code[1]>>10)&4095u;
    int64_t pages=((code[0]>>29)&3u)|(((code[0]>>5)&0x7ffffu)<<2);
    if(pages&(1ll<<20))pages-=1ll<<21;
    *target=(uintptr_t)((int64_t)(stub&~(uintptr_t)4095)+pages*4096+low);
    return low==LP_STUB_LOW;
}
/* ADRP only reaches +/-4 GiB. A nearby RX page can reach the injected
 * function at any address via an absolute literal, preserving x0-x15/LR. */
static inline int lp_bridge_code(void *page,size_t bytes,uint64_t destination) {
    if(!page || !destination || bytes<LP_STUB_LOW+20)return 0;
    const uint32_t code[3]={0x58000070u,0xd61f0200u,0xd503201fu}; /* ldr x16,+12; br x16; nop */
    memcpy((uint8_t *)page+LP_STUB_LOW,code,sizeof(code));
    memcpy((uint8_t *)page+LP_STUB_LOW+12,&destination,sizeof(destination));return 1;
}
/* The optimized stub is ADRP x16; ADD x16,x16,#low; BR x16.
 * Keep ADD/BR intact. A replacement with the same low 12 bits permits one
 * aligned, atomic instruction change; callers see either complete target. */
static inline int lp_stub_plan(uintptr_t stub,const uint32_t code[3],uintptr_t original,
                               uintptr_t replacement,uint32_t *adrp) {
    uintptr_t old_target=0;
    if(!adrp || !lp_stub_destination(stub,code,&old_target))return 0;
    uint32_t low=LP_STUB_LOW;
    if(old_target!=original || (replacement&4095u)!=low || low!=LP_STUB_LOW)return 0;
    int64_t pages=((int64_t)(replacement&~(uintptr_t)4095)-(int64_t)(stub&~(uintptr_t)4095))/4096;
    if(pages<-(1ll<<20) || pages>=(1ll<<20))return 0;
    uint32_t imm=(uint32_t)pages&0x1fffffu;
    *adrp=0x90000010u|((imm&3u)<<29)|((imm>>2)<<5);
    return 1;
}
#endif
