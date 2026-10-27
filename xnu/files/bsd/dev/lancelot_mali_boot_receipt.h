/* SPDX-License-Identifier: GPL-2.0-only
 * Same-boot CRC-protected observation record, NOT a power/ownership provider.
 * XNU only validates and prints this bounded data; it never enables the GPU.
 */
#ifndef LANCELOT_MALI_BOOT_RECEIPT_H
#define LANCELOT_MALI_BOOT_RECEIPT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define LMBR_MAGIC UINT32_C(0x5244474c)
enum lmbr_word {
    LMBR_ERROR,LMBR_TEARDOWN,LMBR_GPU_ID,LMBR_GPU_STATUS,LMBR_HZ,LMBR_PWR_WRITES,
    LMBR_STARTED,LMBR_OFF,LMBR_FEATURE_VALID,LMBR_DMA_REQUEST,LMBR_DMA_ATTEMPT,
    LMBR_DMA_SAFE,LMBR_DMA_HALT,LMBR_POOL_ERROR,LMBR_RETAINED,LMBR_PEAK,
    LMBR_CLEANS,LMBR_INVALIDATES,LMBR_GPU_READS,LMBR_GPU_WRITES,LMBR_IO_ERROR,
    LMBR_RELEASE_ERROR,LMBR_SUBMITTED,LMBR_GUARDS,LMBR_BAD_OFFSET,LMBR_BAD_JOB,
    LMBR_CLEANUP,LMBR_VALUE,LMBR_JOB_IRQ,LMBR_JOB_STATUS,LMBR_MMU,LMBR_GPU_FAULT,
    LMBR_FEATURE_FIRST,
    LMBR_JOB_STAGE=LMBR_FEATURE_FIRST+42,LMBR_MISMATCH_REG,
    LMBR_EXPECTED_LO,LMBR_EXPECTED_HI,LMBR_OBSERVED_LO,LMBR_OBSERVED_HI,
    LMBR_RESET_COMPLETED,LMBR_QUARANTINED,LMBR_TEARDOWN_STATE,
    LMBR_FIRST_IO_ERROR,LMBR_FIRST_IO_REG,LMBR_FIRST_IO_PHASE,
    LMBR_RECOVERY_ATTEMPT,LMBR_RECOVERY_ACTIVE,LMBR_LAST_PHASE,
    LMBR_TRANSPORT_ERROR,LMBR_TRANSPORT_ADDRESS,LMBR_JOB_STATE,
    LMBR_MMU_FAULT_TEST,LMBR_EXPECTED_FAULT,LMBR_EXPECTED_VA_LO,LMBR_EXPECTED_VA_HI,LMBR_RESERVED_FIRST
};
struct lmbr_receipt {
    uint32_t magic,version,size,crc;
    uint64_t boot_id,pool_pa,sequence,fault_pa;
    uint32_t word[96];
};
_Static_assert(sizeof(struct lmbr_receipt)==432,"Fixed observation receipt ABI");
static inline uint32_t lmbr_crc(const struct lmbr_receipt *r) {
    const uint8_t*p=(const uint8_t*)r;uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<sizeof(*r);i++) {
        uint8_t v=(i>=offsetof(struct lmbr_receipt,crc)&&i<offsetof(struct lmbr_receipt,crc)+4)?0:p[i];
        crc^=v;
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0);
    }
    return ~crc;
}
static inline void lmbr_seal(struct lmbr_receipt*r,uint64_t boot) {
    r->magic=LMBR_MAGIC;r->version=4;r->size=sizeof(*r);r->boot_id=boot;r->crc=lmbr_crc(r);
}
static inline bool lmbr_valid(const struct lmbr_receipt*r,size_t bytes,uint64_t boot) {
    if(!r||bytes!=sizeof(*r)||r->magic!=LMBR_MAGIC||r->version<1||r->version>4||r->size!=sizeof(*r)||
       !boot||r->boot_id!=boot||r->crc!=lmbr_crc(r))return false;
    const unsigned bs[]={LMBR_STARTED,LMBR_OFF,LMBR_FEATURE_VALID,LMBR_DMA_REQUEST,
        LMBR_DMA_ATTEMPT,LMBR_DMA_SAFE,LMBR_DMA_HALT,LMBR_RETAINED,LMBR_SUBMITTED,LMBR_GUARDS,LMBR_BAD_JOB};
    for(unsigned i=0;i<sizeof(bs)/sizeof(bs[0]);i++)if(r->word[bs[i]]>1)return false;
    if(r->version>=2&&(r->word[LMBR_RESET_COMPLETED]>1||r->word[LMBR_QUARANTINED]>1||
        r->word[LMBR_JOB_STAGE]>7||(r->word[LMBR_TEARDOWN_STATE]&~31u)))return false;
    if(r->version>=3&&(r->word[LMBR_FIRST_IO_PHASE]>14||r->word[LMBR_LAST_PHASE]>14||
        r->word[LMBR_RECOVERY_ATTEMPT]>1||r->word[LMBR_RECOVERY_ACTIVE]>1))return false;
    if(r->version==4&&(r->word[LMBR_MMU_FAULT_TEST]>1||r->word[LMBR_EXPECTED_FAULT]>1))return false;
    for(unsigned i=r->version==1?74:r->version==2?83:r->version==3?92:LMBR_RESERVED_FIRST;i<96;i++)if(r->word[i])return false;
    return true;
}
#endif
