/* SPDX-License-Identifier: GPL-2.0-only
 * Actual UEFI page lease and architected CPU mapping/cache checks. No XNU
 * ownership provider; no OS-specific power recipe. Included after loader libc.
 */
#ifndef XNULOADER_LANCELOT_GPU_DMA_NATIVE_H
#define XNULOADER_LANCELOT_GPU_DMA_NATIVE_H
#include "lancelot_gpu_dma.h"
#define LM_FIRMWARE_CORE 1
#include "../xnu/files/bsd/dev/lancelot_mali_core.c"
#undef LM_FIRMWARE_CORE
static struct lfd_context lancelot_dma_context;
static int lancelot_dma_allocate(void *cookie,void **cpu,uint64_t *pa,unsigned pages) {
    (void)cookie;
    if(pages!=LFD_PAGES || !BS->allocate_pages || !BS->free_pages)return LM_ENOTSUP;
    uint64_t at=LFD_MAX_PA-1;
    efi_status status=BS->allocate_pages(AllocateMaxAddress,EfiLoaderData,pages,&at);
    if(status)return LM_ENOMEM;
    *pa=at;*cpu=(void *)(uintptr_t)at;return LM_OK;
}
static int lancelot_dma_release(void *cookie,void *cpu,uint64_t pa,unsigned pages) {
    (void)cookie;
    if((uint64_t)(uintptr_t)cpu!=pa || pages!=LFD_PAGES)return LM_EINVAL;
    return BS->free_pages(pa,pages)?LM_EIO:LM_OK;
}
static int lancelot_dma_check_page(void *cookie,void *cpu,uint64_t pa) {
    (void)cookie;
    static uint8_t memory_map[65536];
    uint64_t size=sizeof(memory_map),key,stride;uint32_t version;
    if((READ_SYSREG(SCTLR_EL1)&5)!=5 || !BS->get_memory_map ||
       EFI_ERROR(BS->get_memory_map(&size,memory_map,&key,&stride,&version)) ||
       stride<sizeof(efi_memory_descriptor))return LM_ENOTSUP;
    bool found=false;
    for(uint64_t off=0;off+stride<=size;off+=stride) {
        efi_memory_descriptor*d=(efi_memory_descriptor *)(memory_map+off);
        if(d->type==EfiLoaderData && (d->attribute&8u) && !(d->attribute&0x20000u) && pa>=d->phys_start &&
           d->pages>=1 && d->pages<=UINT64_MAX/4096u && pa-d->phys_start<=d->pages*4096u-4096u) {
            found=true;break;
        }
    }
    if(!found)return LM_EFAULT;
    for(unsigned edge=0;edge<2;edge++) {
        uint64_t va=(uint64_t)(uintptr_t)cpu+(edge?4095u:0u),par;
        for(unsigned access=0;access<2;access++) {
            if(access)__asm__ volatile("at s1e1w, %1\n isb\n mrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
            else __asm__ volatile("at s1e1r, %1\n isb\n mrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
            if((par&1u) || (par&UINT64_C(0x0000fffffffff000))!=pa || (par>>56)!=0xffu)return LM_EFAULT;
        }
    }
    return LM_OK;
}
static int lancelot_dma_cache(void *cookie,void *p,size_t n,bool after) {
    (void)cookie;
    uint64_t start=(uint64_t)(uintptr_t)p,end=start+n;
    if(n!=4096 || (start&4095u) || end<start)return LM_EINVAL;
    uint64_t line=4ULL<<((READ_SYSREG(CTR_EL0)>>16)&15u);
    if(line<16 || line>4096)return LM_ENOTSUP;
    for(uint64_t at=start;at<end;at+=line) {
        if(after)__asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
        else __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");return LM_OK;
}
static void lancelot_dma_barrier(void *cookie) {
    (void)cookie;__asm__ volatile("dsb sy" ::: "memory");
}
static void lancelot_dma_trace(void *cookie,bool read,uint32_t off,uint32_t value,int error) {
    (void)cookie;
    if(off>=0x4000u)
        print("lancelot-gpu-power: deferred-write addr=%x value=%x error=%x\n",
            (uint64_t)off,(uint64_t)value,(uint64_t)(uint32_t)error);
    else print("lancelot-gpu-dma: trace kind=%d reg=%x value=%x error=%x\n",
        (uint64_t)read,(uint64_t)off,(uint64_t)value,(uint64_t)(uint32_t)error);
}
static const struct lfd_memory_ops lancelot_dma_memory={
    .allocate=lancelot_dma_allocate,.release=lancelot_dma_release,
    .check_page=lancelot_dma_check_page,.cache=lancelot_dma_cache,.barrier=lancelot_dma_barrier,
    .trace=lancelot_dma_trace
};
#endif
