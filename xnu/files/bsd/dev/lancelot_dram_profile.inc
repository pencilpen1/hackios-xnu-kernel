/* The native GPU status thread is the sole caller. The observed banks remain
 * powered while executing from DRAM; all offsets are individually allowlisted.
 * Addresses match lancelot's captured Android dramc@1022a000 and emi@10219000. */
#include "lancelot_dram_profile.h"
static vm_offset_t lmd_banks[LMD_BANKS];
static int lmd_kernel_read(__unused void *cookie,enum lmd_bank bank,uint32_t off,uint32_t *value) {
    const vm_offset_t physical[LMD_BANKS]={0x1022a000,0x10228000,0x10219000};
    if(!value || !lmd_read_allowed(bank,off))return -22;
    if(!lmd_banks[bank])lmd_banks[bank]=ml_io_map(physical[bank],bank==LMD_EMI?0x1000:0x2000);
    if(!lmd_banks[bank])return -12;
    __asm__ volatile("dsb sy" ::: "memory");
    *value=*(const volatile uint32_t *)(lmd_banks[bank]+off);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
static struct lmd_clock lmd_kernel_snapshot(void) {
    struct lmd_clock value={0};
    value.error=-19;
    if(gpu_handoff_fact_status!=1 || gpu_is_returning())return value;
    const struct lmd_io io={.cookie=NULL,.read32=lmd_kernel_read};
    (void)lmd_capture(&io,&value);
    return value;
}
