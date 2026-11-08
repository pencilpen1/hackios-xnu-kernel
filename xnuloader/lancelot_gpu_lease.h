/* MMU-off return gate: AP_ONLY is NOT a DVFS permission grant. */
#ifndef XNULOADER_GPU_LEASE_H
#define XNULOADER_GPU_LEASE_H
#include "../xnu/files/bsd/dev/lancelot_gpu_handoff.h"
#include "../xnu/files/bsd/dev/lancelot_gpu_handoff_slot.h"
#define EFI_MTK_GPU_LEASE_GUID {0x583f0279,0x6ae2,0x4f41,{0xa8,0x44,0x67,0x68,0x47,0x50,0x55,0x31}}
typedef struct efi_mtk_gpu_lease efi_mtk_gpu_lease;
struct efi_mtk_gpu_lease {
    uint32_t revision,size;
    efi_status (EFIAPI *writer_begin)(efi_mtk_gpu_lease *,uint32_t,uint64_t *);
    efi_status (EFIAPI *writer_end)(efi_mtk_gpu_lease *,uint64_t,uint8_t);
    efi_status (EFIAPI *prepare)(efi_mtk_gpu_lease *,uint64_t,uint32_t,uint32_t,struct lgh_offer *);
    efi_status (EFIAPI *seal)(efi_mtk_gpu_lease *,uint64_t,struct lgh_offer *);
    efi_status (EFIAPI *query)(efi_mtk_gpu_lease *,struct lgh_offer *);
    efi_status (EFIAPI *release)(efi_mtk_gpu_lease *,const struct lgh_return *);
};
static struct lgh_bundle loader_gpu_bundle __attribute__((aligned(64)));
/* Bound only to the loaded kernel's dedicated normal-RAM data section. The
 * resident EFI staging object above is never exported as an arbitrary PA. */
static struct lgh_kernel_slot *loader_gpu_kernel_slot __attribute__((used));
static struct lgh_offer loader_gpu_expected_offer __attribute__((aligned(64)));
static uint64_t loader_gpu_return_args[8] __attribute__((used));
static uint32_t loader_gpu_lease_armed;
static uint32_t loader_gpu_lease_failed __attribute__((used));
static efi_mtk_gpu_lease *loader_gpu_lease;
struct lgh_gate_io {void *cookie;uint32_t (*read)(void *,uint64_t);int64_t (*affinity)(void *,uint32_t);uint32_t (*mpidr)(void *);};
static int lgh_loader_return_check(const volatile struct lgh_bundle *b,const struct lgh_gate_io *io) {
    if(!b||!io||!io->read||!io->affinity||!io->mpidr)return 0;
    struct lgh_offer o=b->offer;struct lgh_return r=b->returned;
    if(!lgh_return_valid(&o,&r)||io->mpidr(io->cookie)!=o.boot_mpidr)return 0;
    for(uint32_t id=0;id<=0x700;id+=0x100)if(id!=o.boot_mpidr && io->affinity(io->cookie,id)!=1)return 0;
    for(unsigned pass=0;pass<2;pass++)
        if(io->read(io->cookie,0x1000c258)!=o.main_con0 || io->read(io->cookie,0x1000c25c)!=o.main_con1 || io->read(io->cookie,0x1000c264)!=o.main_con3)return 0;
    /* Independent AO clock readback; a CRC-valid stale packet is not proof
     * that the actual mux/PLL still agrees. Do not access powered-off job banks. */
    for(unsigned pass=0;pass<2;pass++)
        if((io->read(io->cookie,0x10000050)&0x83u)!=1u ||
           !(io->read(io->cookie,0x1000c248)&1u) || (io->read(io->cookie,0x1000c254)&3u)!=1u ||
           (io->read(io->cookie,0x1000c24c)&0x7fffffffu)!=(r.mfg_con1&0x7fffffffu))return 0;
    return o.crc==b->offer.crc && r.crc==b->returned.crc;
}
#ifndef LGH_LOADER_HOST_TEST
static uint32_t lgh_gate_read(void *p,uint64_t a) {
    (void)p;__asm__ volatile("dsb sy":::"memory");uint32_t v=*(volatile uint32_t *)(uintptr_t)a;
    __asm__ volatile("dsb sy":::"memory");return v;
}
static uint32_t lgh_gate_mpidr(void *p) {(void)p;uint64_t v;__asm__ volatile("mrs %0,mpidr_el1":"=r"(v));return v&0xffffffu;}
static int64_t lgh_gate_affinity(void *p,uint32_t target) {
    (void)p;register uint64_t a0 __asm__("x0")=0xc4000004ULL;
    register uint64_t a1 __asm__("x1")=target;register uint64_t a2 __asm__("x2")=0;
    __asm__ volatile("smc #0":"+r"(a0),"+r"(a1),"+r"(a2)::"x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    return (int64_t)a0;
}
__attribute__((used,noinline)) static int lancelot_fwlease_return_check(void) {
    if(!loader_gpu_lease_armed)return 1;
    if(!loader_gpu_kernel_slot||!lgh_slot_header_valid(loader_gpu_kernel_slot))return 0;
    const struct lgh_gate_io io={.read=lgh_gate_read,.affinity=lgh_gate_affinity,.mpidr=lgh_gate_mpidr};
    if(lgh_bytes_same(&loader_gpu_kernel_slot->bundle.offer,&loader_gpu_expected_offer,sizeof(loader_gpu_expected_offer))&&
       lgh_loader_return_check(&loader_gpu_kernel_slot->bundle,&io))return 1;
    loader_gpu_kernel_slot->bundle.offer.state=LGH_UNKNOWN;
    lgh_seal_offer(&loader_gpu_kernel_slot->bundle.offer);
    __asm__ volatile("dsb sy":::"memory");return 0;
}
#endif
#endif
