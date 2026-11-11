#ifndef LANCELOT_PRESENT_POLICY_H
#define LANCELOT_PRESENT_POLICY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LMP_SWAP_BYTES 0x46cu
static inline uint32_t lmp_word(const void *p,size_t at){uint32_t v;memcpy(&v,(const uint8_t*)p+at,4);return v;}
static inline void lmp_put(void *p,size_t at,uint32_t v){memcpy((uint8_t*)p+at,&v,4);}
static inline int lmp_full_frame(const void *p,size_t n,uint32_t width,uint32_t height) {
    if(!p || n!=LMP_SWAP_BYTES || !width || !height || width>8192 || height>8192 || !lmp_word(p,0x54))return 0;
    for(unsigned i=1;i<4;i++)if(lmp_word(p,0x54+4*i))return 0;
    if(lmp_word(p,0x64)||lmp_word(p,0x68)||lmp_word(p,0xc4)||lmp_word(p,0xc8))return 0;
    const size_t xs[]={0x6c,0xcc},ys[]={0x70,0xd0};
    for(unsigned i=0;i<2;i++) {
        uint32_t w=lmp_word(p,xs[i]),h=lmp_word(p,ys[i]);
        if(w && w!=width)return 0;
        if(h && h!=height)return 0;
    }
    return 1;
}
static inline int lmp_rewrite(void *out,const void *in,size_t n,uint32_t source_width,uint32_t source_height,
    uint32_t surface,uint32_t panel_width,uint32_t panel_height) {
    if(!out || !surface || !panel_width || !panel_height || panel_width>8192 || panel_height>8192 ||
       !lmp_full_frame(in,n,source_width,source_height))return 0;
    memcpy(out,in,n);lmp_put(out,0x54,surface);lmp_put(out,0x6c,panel_width);lmp_put(out,0x70,panel_height);
    return 1;
}
#endif
