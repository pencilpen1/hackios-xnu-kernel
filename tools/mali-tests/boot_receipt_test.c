/* Bounded same-boot observation record only; no MMIO or power provider. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_boot_receipt.h"
int main(void) {
    struct lmbr_receipt r={0},changed;r.word[LMBR_DMA_REQUEST]=1;
    r.word[LMBR_VALUE]=0x4c4d4732;lmbr_seal(&r,0x12345678);
    assert(lmbr_valid(&r,sizeof(r),r.boot_id));
    for(unsigned i=0;i<sizeof(r);i++) {
        changed=r;((unsigned char*)&changed)[i]^=1;
        assert(!lmbr_valid(&changed,sizeof(changed),r.boot_id));
    }
    for(unsigned n=0;n<sizeof(r);n++)assert(!lmbr_valid(&r,n,r.boot_id));
    assert(!lmbr_valid(&r,sizeof(r),r.boot_id+1));
    changed=r;changed.word[LMBR_DMA_SAFE]=2;lmbr_seal(&changed,r.boot_id);
    assert(!lmbr_valid(&changed,sizeof(changed),r.boot_id));
    changed=r;changed.word[LMBR_JOB_STAGE]=1;changed.word[LMBR_MISMATCH_REG]=0x2408;
    changed.word[LMBR_EXPECTED_LO]=0x888d8f88;changed.word[LMBR_EXPECTED_HI]=0x4c8d;
    changed.word[LMBR_RESET_COMPLETED]=1;changed.word[LMBR_TEARDOWN_STATE]=27;
    lmbr_seal(&changed,r.boot_id);assert(lmbr_valid(&changed,sizeof(changed),r.boot_id));
    changed.version=1;changed.crc=lmbr_crc(&changed);
    assert(!lmbr_valid(&changed,sizeof(changed),r.boot_id));
    changed=r;changed.version=1;changed.crc=lmbr_crc(&changed);
    assert(lmbr_valid(&changed,sizeof(changed),r.boot_id));
    changed=r;changed.word[95]=1;lmbr_seal(&changed,r.boot_id);
    changed.version=3;changed.crc=lmbr_crc(&changed);
    assert(!lmbr_valid(&changed,sizeof(changed),r.boot_id));
    changed=r;changed.word[LMBR_MMU_FAULT_TEST]=2;lmbr_seal(&changed,r.boot_id);
    assert(!lmbr_valid(&changed,sizeof(changed),r.boot_id));
    puts("Receipt: all 432 byte corruptions/lengths, old boot, malformed flags/reserved fields rejected; RAM ONLY");
    return 0;
}
