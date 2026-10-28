/* Lancelot-only XNU adapter. This file never changes UEFI or the filesystem.
 * lancelot_mali=0(default): no maps/reads/writes.
 * =1: SPM-only snapshot. =2: reserved (reject without MMIO).
 * =3: full bring-up + private GPU WRITE_VALUE DMA test, no user service.
 * =4: same test then publish versioned LancelotMali service if it succeeds.
 * Neither host compilation nor private WRITE_VALUE is proof of rendering.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include "lancelot_mali_core.h"
#include "lancelot_mali_platform.h"
#include "lancelot_mali_pmic.h"
#include "lancelot_mali_smoke.h"
#include "lancelot_storage.h"
#include "lancelot_pwrap.h"
#include "lancelot_mali_boot_receipt.h"
extern void lancelot_mali_observe(void);
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern void gic_v3_spi_mask(uint32_t);
extern void LancelotMaliPublish(void);
static LCK_GRP_DECLARE(mali_lck_grp,"lancelot-mali");
static LCK_MTX_DECLARE(mali_lock,&mali_lck_grp);
static struct lmp_state platform;
static struct lm_core *engine;
static vm_offset_t banks[LMP_BANK_COUNT];
static vm_offset_t pwrap_bank;
static struct lmp_pmic_state pmic;
static bool pmic_valid;
static bool initialized, usable;
static uint32_t cache_line=64;
static struct lm_gpu_pwrap_owner pwrap_owner;
static void shared_pwrap_poison(__unused void *cookie) {
    int e=lancelot_pwrap_poison();
    if(e)printf("lancelot-mali: shared WACS2 poison error=%d\n",e);
}
int lm_gpu_register_pwrap_owner(const struct lm_gpu_pwrap_owner *owner) {
    if(!owner||!owner->acquire||!owner->release||!owner->poison)return LM_EINVAL;
    /* All actual WACS2 users must use the public arbiter. Do not accept an
     * unrelated per-driver mutex or no-op ownership implementation. */
    if(owner->cookie||owner->acquire!=lancelot_pwrap_acquire||
       owner->release!=lancelot_pwrap_release||owner->poison!=shared_pwrap_poison)return LM_ENOTSUP;
    lck_mtx_lock(&mali_lock);
    int e=(initialized||pwrap_owner.acquire)?LM_EBUSY:LM_OK;
    if(!e)pwrap_owner=*owner;
    lck_mtx_unlock(&mali_lock);return e;
}
static int pwrap_acquire(__unused void *c,uint64_t timeout_ns) {
    return pwrap_owner.acquire?pwrap_owner.acquire(pwrap_owner.cookie,timeout_ns):LM_ENOTSUP;
}
static void pwrap_release(__unused void *c) {
    if(pwrap_owner.release)pwrap_owner.release(pwrap_owner.cookie);
}
static void pwrap_poison(__unused void *c) {
    if(pwrap_owner.poison)pwrap_owner.poison(pwrap_owner.cookie);
}

static bool board_matches(void) {
    DTEntry chosen;const void *prop;unsigned bytes;struct lancelot_storage offer={0};
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess||
       SecureDTGetProperty(chosen,"lancelot-emmc",&prop,&bytes)!=kSuccess||
       (bytes!=LANCELOT_STORAGE_V1_SIZE&&bytes!=sizeof(offer)))return false;
    memcpy(&offer,prop,bytes);
    return offer.magic==LANCELOT_STORAGE_MAGIC&&offer.size==bytes&&
        ((offer.version==1&&bytes==LANCELOT_STORAGE_V1_SIZE)||(offer.version==2&&bytes==sizeof(offer)))&&
        offer.mmio==0x11230000&&offer.partition_bytes==LANCELOT_UBUNTU_BYTES&&
        offer.partition_offset&&!(offer.partition_offset&511)&&
        offer.partition_offset<(UINT64_C(1)<<41)-LANCELOT_UBUNTU_BYTES;
}
static void firmware_gpu_receipt(void) {
    /* v4 includes the explicit fault-test mode; observe the original failure
     * record, never convert an expected fault into a provider or render PASS. */
    DTEntry chosen;const void *prop;unsigned bytes;uint64_t usb[3];struct lmbr_receipt r;
    if(!board_matches() || SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"lancelot-gpu-receipt",&prop,&bytes)!=kSuccess)return;
    if(bytes!=sizeof(r)){printf("lancelot-gpu-receipt: invalid size; no GPU access\n");return;}
    memcpy(&r,prop,sizeof(r));
    if(SecureDTGetProperty(chosen,"lancelot-usb-log",&prop,&bytes)!=kSuccess || bytes!=sizeof(usb))return;
    memcpy(usb,prop,sizeof(usb));
    if(usb[0]!=UINT64_C(0x31474f4c4253554c) || !lmbr_valid(&r,sizeof(r),usb[2])) {
        printf("lancelot-gpu-receipt: invalid CRC/boot/format; no GPU access\n");return;
    }
    printf("lancelot-gpu-receipt: source=XNU boot=%#llx pool=%#llx sequence=%llu fault=%#llx size=%u crc=%#x verified=1 version=%u\n",
        r.boot_id,r.pool_pa,r.sequence,r.fault_pa,r.size,r.crc,r.version);
    for(unsigned i=0;i<96;i++)printf("lancelot-gpu-receipt: word=%u value=%#x\n",i,r.word[i]);
    printf("lancelot-gpu-receipt: firmware observation only; no driver/ownership/DMA provider created\n");
}
static uint64_t clock_ns(__unused void *c) {uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
static void wait_us(__unused void*c,uint32_t us) {delay_for_interval(us,NSEC_PER_USEC);}
static int read_bank(__unused void*c,enum lmp_bank bank,uint32_t off,uint32_t*out) {
    static const vm_offset_t pa[LMP_BANK_COUNT]={0x10006000,0x10001000,0x13000000,0x13040000,0x10000000,0x1000c000};
    uint32_t size;
    if(!out||bank<0||bank>=LMP_BANK_COUNT||(off&3))return LM_EINVAL;
    size=bank==LMP_BANK_GPU?0x4000u:0x1000u;if(off>=size)return LM_EINVAL;
    if(!banks[bank])banks[bank]=ml_io_map(pa[bank],size);
    if(!banks[bank])return LM_ENOMEM;
    __asm__ volatile("dsb sy":::"memory");
    *out=*(volatile uint32_t*)(banks[bank]+off);
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static int write_bank(__unused void*c,enum lmp_bank bank,uint32_t off,uint32_t value) {
    uint32_t size;
    if(bank<0||bank>=LMP_BANK_COUNT||(off&3))return LM_EINVAL;
    size=bank==LMP_BANK_GPU?0x4000u:0x1000u;if(off>=size||!banks[bank])return LM_EINVAL;
    *(volatile uint32_t*)(banks[bank]+off)=value;
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static int pwrap_read(__unused void*c,uint32_t off,uint32_t*out) {
    if(!out||(off&3)||off>=0x1000u)return LM_EINVAL;
    if(!pwrap_bank)pwrap_bank=ml_io_map(LMP_PMIC_WRAP_BASE_PHYS,0x1000);
    if(!pwrap_bank)return LM_ENOMEM;__asm__ volatile("dsb sy":::"memory");
    *out=*(volatile uint32_t*)(pwrap_bank+off);__asm__ volatile("dsb sy":::"memory");return 0;
}
static int pwrap_write(__unused void*c,uint32_t off,uint32_t value) {
    if((off&3)||off>=0x1000u)return LM_EINVAL;
    if(!pwrap_bank)pwrap_bank=ml_io_map(LMP_PMIC_WRAP_BASE_PHYS,0x1000);
    if(!pwrap_bank)return LM_ENOMEM;*(volatile uint32_t*)(pwrap_bank+off)=value;
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static bool core_mmio_failed;
static int core_mmio_error(__unused void*c) {return core_mmio_failed?LM_EIO:LM_OK;}
static uint32_t gpu_read(__unused void*c,uint32_t off) {
    uint32_t v=0; if(lmp_gpu_read32(&platform,off,&v)){usable=false;core_mmio_failed=true;}return v;
}
static void gpu_write(__unused void*c,uint32_t off,uint32_t value) {
    if(lmp_gpu_write32(&platform,off,value)){usable=false;core_mmio_failed=true;}
}
static void *meta_alloc(__unused void*c,size_t n) {return kalloc_data(n,Z_WAITOK|Z_ZERO);}
static void meta_free(__unused void*c,void*p,size_t n) {kfree_data(p,n);}
static int page_alloc(__unused void*c,struct lm_page*p) {
    vm_offset_t addr=0;
    if(kmem_alloc(kernel_map,&addr,round_page(LM_PAGE_SIZE),KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return LM_ENOMEM;
    p->cpu=(void*)addr;p->pa=kvtophys(addr);
    if(!p->pa||(p->pa&4095)) {kmem_free(kernel_map,addr,round_page(LM_PAGE_SIZE));p->cpu=NULL;return LM_EIO;}
    return 0;
}
static void page_free(__unused void*c,struct lm_page*p) {kmem_free(kernel_map,(vm_offset_t)p->cpu,round_page(LM_PAGE_SIZE));p->cpu=NULL;}
static void barrier(__unused void*c) {__asm__ volatile("dsb sy":::"memory");}
static void for_device(__unused void*c,void*p,size_t n) {
    uintptr_t end=(uintptr_t)p+n;
    for(uintptr_t a=(uintptr_t)p&~(uintptr_t)(cache_line-1);a<end;a+=cache_line)
        __asm__ volatile("dc civac, %0"::"r"(a):"memory");
    barrier(NULL);
}
static void for_cpu(__unused void*c,void*p,size_t n) {
    uintptr_t end=(uintptr_t)p+n;
    for(uintptr_t a=(uintptr_t)p&~(uintptr_t)(cache_line-1);a<end;a+=cache_line)
        __asm__ volatile("dc ivac, %0"::"r"(a):"memory");
    barrier(NULL);
}
static int private_dma_test(void) {
    struct lm_page pages[2]={{0}};struct lm_vm*v=lm_core_vm_new(engine);struct lm_job_result r={0};
    if(!v)return LM_ENOMEM;
    int e=page_alloc(NULL,&pages[0]);if(e)goto out;
    e=page_alloc(NULL,&pages[1]);if(e)goto out;
    const uint32_t pattern=0x4d414c49;
    lm_v7_dma_prepare(pages[0].cpu,pages[1].cpu,LM_VA_MIN+4096,pattern);
    e=lm_core_vm_map(v,LM_VA_MIN,pages,2,LM_MAP_WRITE);if(e)goto out;
    e=lm_core_submit_wait(v,LM_VA_MIN,0,UINT64_C(1000000000),&r);
    bool guards=false,bad_job=false;uint32_t bad_offset=0;
    if(!e) {
        guards=lm_v7_dma_validate(pages[0].cpu,pages[1].cpu,pattern,&bad_offset,&bad_job);
        if(!guards)e=LM_EIO;
    }
    uint32_t output=((volatile uint32_t*)pages[1].cpu)[LM_DMA_TARGET_WORD];
    printf("lancelot-mali: private WRITE_VALUE result=%d output=%#x expected=%#x job=%llu status=%#x MMU=%#x GPU=%#x reset=%u quarantine=%u; this is NOT rendering\n",
        e,output,pattern,r.sequence,r.job_status,r.mmu_fault,r.gpu_fault,r.reset_completed,r.quarantined);
    printf("lancelot-mali: private DMA guards=%s target-offset=%u bad-page=%s bad-offset=%u; only two private GPU pages mapped\n",
        guards?"PASS":"NOT-PASS",4u*LM_DMA_TARGET_WORD,bad_job?"job":"output",bad_offset);
 out:
    if(lm_core_vm_safe_to_free(v)) {
        int closed=lm_core_vm_destroy(v);
        if(!closed)for(unsigned i=0;i<2;i++)if(pages[i].cpu)page_free(NULL,&pages[i]);
    } else printf("lancelot-mali: DMA state unknown; test VM/pages quarantined until reboot\n");
    return e;
}
/* Board evidence for the WACS2 window. The AP owns the WACS2 command window;
 * peer processors use separate PWRAP windows (WACS_P2P / WACS_MD32) and the
 * hardware arbiter serialises bus access, so there is no same-WACS2 peer to
 * coordinate with. Sources:
 *  - vendor pwrap_hal.c: pwrap_init() ends in _pwrap_enable(), which sets
 *    HPRIO_ARB_EN=0x3fa65 with WACS0/WACS2/WACS_P2P/WACS_MD32 enabled, and
 *    pwrap_wacs2_write() drives WACS2 directly unless SSPM IPI forwarding is
 *    compiled in.
 *  - shipping Shockwave 4.19 kernel (CONFIG_MTK_TINYSYS_SSPM_SUPPORT=y,
 *    CONFIG_MTK_PMIC_WRAP=y): binds the mainline mtk-pmic-wrap driver, passes
 *    the WACS2 PMIC write test, and enables VGPU/VSRAM through the regulator
 *    framework while SSPM v1 runs; no PMIC IPI forwarding appears in dmesg.
 *  - XNU read-only sampling at the same boot: WACS2 INIT=1, FSM/REQ idle,
 *    SYNC_BUSY only from another channel.
 * This is evidence for the AP WACS2 window, not proof that every other
 * processor is stopped; the arbiter remains enabled. */
static const char lancelot_pwrap_evidence[] =
    "AP owns the WACS2 window; vendor pwrap_init enables WACS0/WACS2/P2P/MD32 "
    "with HPRIO_ARB_EN=0x3fa65 and the shipping 4.19 kernel drives WACS2 "
    "directly (write test pass, VGPU/VSRAM via regulators) while SSPM v1 "
    "runs; SSPM uses the P2P/MD32 windows; XNU read-only sampling sees WACS2 "
    "INIT=1 and FSM/REQ idle";
void lancelot_mali_init(void) {
    uint32_t mode=0;
    if(!PE_parse_boot_argn("lancelot_mali",&mode,sizeof(mode))||mode==0)return;
    if(mode!=1&&mode!=3&&mode!=4) {printf("lancelot-mali: rejected mode=%u; no MMIO\n",mode);return;}
    if(mode==1) {firmware_gpu_receipt();lancelot_mali_observe();return;}
    if(!board_matches()) {printf("lancelot-mali: board contract rejected; no MMIO\n");return;}
    /* Register the evidence-backed WACS2 handoff once, before any user. A
     * second registration is rejected; that is fine for repeated init calls. */
    if(!lancelot_pwrap_available()) {
        static bool handoff_registered;
        if(!handoff_registered) {
            static const struct lancelot_pwrap_handoff h={.mode=LANCELOT_PWRAP_AP_EXCLUSIVE,
                .evidence=lancelot_pwrap_evidence};
            int he=lancelot_pwrap_register_handoff(&h);
            printf("lancelot-mali: WACS2 handoff registration=%d evidence=board-vendor-mainline\n",he);
            if(!he)handoff_registered=true;
        }
        if(!lancelot_pwrap_available()) {
            printf("lancelot-mali: WACS2 handoff unavailable; no PWRAP/GPU access, no service\n");
            return;
        }
    }
    const struct lm_gpu_pwrap_owner shared={.acquire=lancelot_pwrap_acquire,
        .release=lancelot_pwrap_release,.poison=shared_pwrap_poison};
    int registration=lm_gpu_register_pwrap_owner(&shared);
    if(registration) {printf("lancelot-mali: shared WACS2 adapter registration=%d; no MMIO\n",registration);return;}
    lck_mtx_lock(&mali_lock);
    if(initialized) {lck_mtx_unlock(&mali_lock);return;} initialized=true;
    uint64_t ctr=__builtin_arm_rsr64("CTR_EL0");cache_line=4u<<((ctr>>16)&15);
    if(cache_line<16||cache_line>256){printf("lancelot-mali: unsupported cache line\n");goto done;}
    printf("lancelot-mali: kernel_vm_page=%u gpu_page=%u cache_line=%u mode=%u; GPU table granule is independent of XNU VM granule\n",
        (unsigned)PAGE_SIZE,LM_PAGE_SIZE,cache_line,mode);
    struct lmp_config config={LMP_PLATFORM_LANCELOT_MT6768,mode==1?LMP_MODE_SPM_ONLY:LMP_MODE_FULL,true,LMP_DEFAULT_TIMEOUT_NS};
    struct lmp_io_ops io={0};io.read32=read_bank;io.write32=write_bank;io.now_ns=clock_ns;io.delay_us=wait_us;
    if(mode!=1) {
        if(!pwrap_owner.acquire||!pwrap_owner.release||!pwrap_owner.poison||!lancelot_pwrap_available()) {
            printf("lancelot-mali: shared WACS2 ownership provider missing; no PWRAP/GPU access, no service\n");
            goto done;
        }
        struct lmp_pmic_registers regs;struct lmp_pmic_io pi={0};
        lmp_pmic_mt6358_registers(&regs);
        pi.read32=pwrap_read;pi.write32=pwrap_write;pi.now_ns=clock_ns;pi.delay_us=wait_us;
        pi.timeout_ns=UINT64_C(10000000);pi.acquire=pwrap_acquire;pi.release=pwrap_release;
        pi.poison=pwrap_poison;
        int pe=lmp_pmic_probe(&pmic,&pi,&regs);
        printf("lancelot-mali: MT6358/PWRAP probe=%d chip=%#x\n",pe,pmic.chip_id);
        if(pe)goto done;
        pe=lmp_pmic_get_platform_ops(&pmic,&io.pmic);if(pe)goto done;
        pmic_valid=true;
    }
    int e=lmp_init(&platform,&config,&io);struct lmp_status s={0};
    if(!e)e=lmp_status(&platform,&s);
    printf("lancelot-mali: mode=%u SPM=%#x/%#x domainMask=%#x match=%u statusResult=%d\n",mode,s.pwr_status,s.pwr_status_2nd,s.domain_mask,s.domain_ack_match,e);
    if(e||mode==1)goto done;
    if((e=lmp_start(&platform))) {printf("lancelot-mali: platform start failed=%d; no service\n",e);goto done;}
    uint32_t identity=0, gpu_status=0;
    e=lmp_gpu_read32(&platform,LMP_GPU_ID,&identity);
    if(!e)e=lmp_gpu_read32(&platform,LMP_GPU_STATUS,&gpu_status);
    printf("lancelot-mali: powered GPU_ID=%#x STATUS=%#x result=%d before private DMA\n",identity,gpu_status,e);
    if(e)goto done;
    for(uint32_t spi=0x112;spi<=0x116;spi++)gic_v3_spi_mask(32+spi);
    core_mmio_failed=false;
    struct lm_ops ops={.cookie=NULL,.read32=gpu_read,.write32=gpu_write,.now_ns=clock_ns,.delay_us=wait_us,
        .alloc=meta_alloc,.free=meta_free,.page_alloc=page_alloc,.page_free=page_free,
        .for_device=for_device,.for_cpu=for_cpu,.barrier=barrier,.io_error=core_mmio_error};
    engine=lm_core_create(&ops);
    if(!engine){printf("lancelot-mali: engine allocation failed\n");goto done;}
    e=lm_core_start(engine);if(e){printf("lancelot-mali: engine start=%d\n",e);goto done;}
    e=private_dma_test();
    usable=!e&&lm_core_ready(engine);
    printf("lancelot-mali: DMA smoke=%d ready=%u; render service requested=%u\n",e,usable,mode==4);
 done:
    lck_mtx_unlock(&mali_lock);
    if(usable&&mode==4)LancelotMaliPublish();
}
bool lm_gpu_ready(void) {lck_mtx_lock(&mali_lock);bool ok=usable&&lm_core_ready(engine);lck_mtx_unlock(&mali_lock);return ok;}
int lm_gpu_param(uint32_t p,uint64_t*out) {lck_mtx_lock(&mali_lock);int e=lm_core_get_param(engine,p,out);lck_mtx_unlock(&mali_lock);return e;}
struct lm_vm *lm_gpu_vm_new(void) {lck_mtx_lock(&mali_lock);struct lm_vm*v=usable?lm_core_vm_new(engine):NULL;lck_mtx_unlock(&mali_lock);return v;}
int lm_gpu_vm_destroy(struct lm_vm*v) {lck_mtx_lock(&mali_lock);int e=lm_core_vm_destroy(v);lck_mtx_unlock(&mali_lock);return e;}
int lm_gpu_vm_map(struct lm_vm*v,uint64_t va,const struct lm_page*p,uint32_t n,uint32_t flags) {lck_mtx_lock(&mali_lock);int e=lm_core_vm_map(v,va,p,n,flags);lck_mtx_unlock(&mali_lock);return e;}
int lm_gpu_vm_unmap(struct lm_vm*v,uint64_t va,uint32_t n) {lck_mtx_lock(&mali_lock);int e=lm_core_vm_unmap(v,va,n);lck_mtx_unlock(&mali_lock);return e;}
bool lm_gpu_vm_safe_to_free(const struct lm_vm*v) {lck_mtx_lock(&mali_lock);bool ok=lm_core_vm_safe_to_free(v);lck_mtx_unlock(&mali_lock);return ok;}
int lm_gpu_submit_wait(struct lm_vm*v,uint64_t jc,uint32_t req,uint64_t ns,struct lm_job_result*r) {
    lck_mtx_lock(&mali_lock);int e=lm_core_submit_wait(v,jc,req,ns,r);
    if(e)printf("lancelot-mali: submit=%d seq=%llu status=%#x fault=%#llx reset=%u quarantine=%u\n",e,r?r->sequence:0,r?r->job_status:0,r?r->fault_address:0,r?r->reset_completed:0,r?r->quarantined:0);
    if(!lm_core_ready(engine))usable=false;
    lck_mtx_unlock(&mali_lock);return e;
}
