/* SPDX-License-Identifier: MIT
 * Mesa 25.0.7 v7.xml descriptor, cross-checked by docs/mesa/job-smoke/verify.py.
 * WRITE_VALUE is a DMA/MMU smoke test; it is NOT rendering evidence.
 */
#ifndef LANCELOT_MALI_SMOKE_H
#define LANCELOT_MALI_SMOKE_H
#include <stdint.h>
#include <stdbool.h>
static inline void lm_v7_write_value_job(uint32_t job[16],uint64_t target,uint32_t value) {
    for(unsigned i=0;i<16;i++)job[i]=0;
    job[4]=UINT32_C(0x00010005);job[8]=(uint32_t)target;job[9]=(uint32_t)(target>>32);
    job[10]=6;job[12]=value;
}
#define LM_DMA_TEST_WORDS 1024u
#define LM_DMA_TARGET_WORD 32u
static inline uint32_t lm_dma_guard(unsigned word, bool job) {
    return (job ? UINT32_C(0x6a6f6247) : UINT32_C(0x646d6147)) ^ (word * UINT32_C(0x10204081));
}
static inline void lm_v7_dma_prepare(uint32_t job[LM_DMA_TEST_WORDS],
    uint32_t output[LM_DMA_TEST_WORDS],uint64_t output_va,uint32_t value) {
    for(unsigned i=0;i<LM_DMA_TEST_WORDS;i++) {
        job[i]=lm_dma_guard(i,true);output[i]=lm_dma_guard(i,false);
    }
    lm_v7_write_value_job(job,output_va+4u*LM_DMA_TARGET_WORD,value);
    output[LM_DMA_TARGET_WORD]=~value;
}
/* Hardware may update the 64-byte job descriptor; neither page's guard area
 * may change. A matching target alone is insufficient evidence of safe DMA. */
static inline bool lm_v7_dma_validate(const volatile uint32_t job[LM_DMA_TEST_WORDS],
    const volatile uint32_t output[LM_DMA_TEST_WORDS],uint32_t value,
    uint32_t *bad_offset,bool *bad_job) {
    for(unsigned i=16;i<LM_DMA_TEST_WORDS;i++)if(job[i]!=lm_dma_guard(i,true)) {
        *bad_offset=i*4u;*bad_job=true;return false;
    }
    for(unsigned i=0;i<LM_DMA_TEST_WORDS;i++)
        if(output[i]!=(i==LM_DMA_TARGET_WORD?value:lm_dma_guard(i,false))) {
            *bad_offset=i*4u;*bad_job=false;return false;
        }
    *bad_offset=0;*bad_job=false;return true;
}
#endif
