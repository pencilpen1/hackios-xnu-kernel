/* SPDX-License-Identifier: GPL-2.0-only
 * G52 JM read-only properties. Offsets cross-checked against supplied kbase
 * register headers and Panfrost; Android r54p2 public property values are a
 * comparison source, not proof of register access or matching driver source.
 * No GPU command, reset, power, AS binding, memory allocation or DMA here. */
#ifndef XNULOADER_LANCELOT_GPU_FEATURES_H
#define XNULOADER_LANCELOT_GPU_FEATURES_H
#include "../xnu/files/bsd/dev/lancelot_mali_platform.h"

static const uint32_t lpg_feature_offsets[] = {
    0x000,0x004,0x008,0x00c,0x010,0x014,0x018,0x01c,
    0x0a0,0x0a4,0x0a8,0x0ac,0x310,
    0x0b0,0x0b4,0x0b8,0x0bc,
    0x0c0,0x0c4,0x0c8,0x0cc,0x0d0,0x0d4,0x0d8,0x0dc,
    0x0e0,0x0e4,0x0e8,0x0ec,0x0f0,0x0f4,0x0f8,0x0fc,
    0x100,0x104,0x110,0x114,0x120,0x124,0xe00,0xe04,0x300
};
#define LPG_FEATURE_WORDS (sizeof(lpg_feature_offsets)/sizeof(lpg_feature_offsets[0]))
struct lpg_features { uint32_t word[LPG_FEATURE_WORDS]; };
static bool lpg_feature_read_allowed(uint32_t off) {
    for(unsigned i=0;i<LPG_FEATURE_WORDS;i++)if(lpg_feature_offsets[i]==off)return true;
    return false;
}
typedef int (*lpg_feature_reader)(void *,uint32_t,uint32_t *);
static int lpg_features_capture(void *cookie,lpg_feature_reader read,
    struct lpg_features *out) {
    if(!read||!out)return LMP_EINVAL;
    struct lpg_features tmp={0};
    for(unsigned pass=0;pass<2;pass++) {
        uint32_t status;
        int error=read(cookie,LMP_GPU_STATUS,&status);if(error)return error;
        if(status&(1u<<7))return LMP_ENOTSUP;
        for(unsigned i=0;i<LPG_FEATURE_WORDS;i++) {
            uint32_t value;error=read(cookie,lpg_feature_offsets[i],&value);
            if(error)return error;
            if(pass && value!=tmp.word[i])return LMP_EIO;
            tmp.word[i]=value;
        }
        error=read(cookie,LMP_GPU_STATUS,&status);if(error)return error;
        if(status&(1u<<7))return LMP_ENOTSUP;
    }
    if(tmp.word[0]!=0x74021000u)return LMP_ENOTSUP;
    *out=tmp;return LMP_OK; // Publish only the complete, stable two-pass sample.
}
static bool lpg_features_private_dma_candidate(const struct lpg_features *f) {
    /* Only qualification for the next candidate, never job/DMA acceptance.
     * Core backend's private 4GiB VA fits the measured 35-bit VA / 40-bit PA.
     * AS0 exists and JS1 explicitly advertises SET_VALUE_JOB (bit2). */
    return f && f->word[0]==0x74021000u && f->word[5]==0x2823u &&
        (f->word[6]&1u) && (f->word[7]&2u) && (f->word[18]&(1u<<2)) &&
        f->word[33]==3 && !f->word[34] && f->word[37]==1 && !f->word[38];
}
#endif
