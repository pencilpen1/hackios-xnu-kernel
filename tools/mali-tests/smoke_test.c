/* Actual production descriptor and guard checker; RAM only, no GPU backend. */
#include <assert.h>
#include <stdio.h>
#include "lancelot_mali_smoke.h"
int main(void) {
    uint32_t job[LM_DMA_TEST_WORDS],output[LM_DMA_TEST_WORDS],bad;
    const uint32_t value=0x4d414c49;
    bool job_bad;
    lm_v7_dma_prepare(job,output,UINT64_C(0x12345678000),value);
    assert(job[4]==0x10005 && job[8]==0x45678080 && job[9]==0x123);
    assert(job[10]==6 && job[12]==value);
    assert(!lm_v7_dma_validate(job,output,value,&bad,&job_bad));
    assert(!job_bad && bad==4*LM_DMA_TARGET_WORD);
    output[LM_DMA_TARGET_WORD]=value;
    assert(lm_v7_dma_validate(job,output,value,&bad,&job_bad));
    for(unsigned i=0;i<LM_DMA_TEST_WORDS;i++) {
        uint32_t saved=output[i];output[i]^=1;
        assert(!lm_v7_dma_validate(job,output,value,&bad,&job_bad));
        assert(!job_bad && bad==i*4);output[i]=saved;
    }
    for(unsigned i=16;i<LM_DMA_TEST_WORDS;i++) {
        uint32_t saved=job[i];job[i]^=1;
        assert(!lm_v7_dma_validate(job,output,value,&bad,&job_bad));
        assert(job_bad && bad==i*4);job[i]=saved;
    }
    /* Hardware completion/status writes inside the descriptor are permitted. */
    job[0]=1;job[1]=0x1234;
    assert(lm_v7_dma_validate(job,output,value,&bad,&job_bad));
    puts("Private DMA descriptor/target offset and all 2032 guard/target corruptions detected; RAM ONLY");
    return 0;
}
