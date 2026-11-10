/* Lancelot-only XNU adapter. This file never changes UEFI or the filesystem.
 * lancelot_mali=0(default): no maps/reads/writes.
 * =1: SPM-only snapshot. =2: reserved (reject without MMIO).
 * =3: full bring-up + private GPU WRITE_VALUE DMA test, no user service.
 * =4: same test then publish versioned LancelotMali service if it succeeds.
 * Neither host compilation nor private WRITE_VALUE is proof of rendering.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <mach/processor_info.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include "lancelot_mali_core.h"
#include "lancelot_mali_cache_ops.h"
#include "lancelot_mali_profile.h"
#include "lancelot_mali_fault.h"
#include "lancelot_mali_platform.h"
#include "lancelot_mali_pmic.h"
#include "lancelot_mali_smoke.h"
#include "lancelot_storage.h"
#include "lancelot_pwrap.h"
#include "lancelot_mali_boot_receipt.h"
#include "lancelot_mali_metrics.h"
#include "lancelot_mali_cpu_profile.h"
#include "lancelot_gpu_handoff_slot.h"
#include "lancelot_mali_load.h"
#include "lancelot_mali_queue.h"
/* Permanent kernel data: loaded/owned with this kernel, never an EFI-image PA
 * mapped through ml_io_map. The locator must name this exact local object. */
static volatile struct lgh_kernel_slot gpu_handoff_slot
    __attribute__((used,aligned(64),section("__DATA,__lghhandoff"))) = {
        .magic=LGH_SLOT_MAGIC,.version=LGH_SLOT_VERSION,.size=sizeof(struct lgh_kernel_slot)
    };
static struct lgh_offer gpu_handoff_fact;
static int gpu_handoff_fact_status;
static bool gpu_handoff_checked;
static int firmware_gpu_handoff_fact(void);
extern kern_return_t processor_info(processor_t,processor_flavor_t,host_t *,processor_info_t,mach_msg_type_number_t *);
/* BSD's exported processor header hides this declaration; the permanent array
 * is the same osfmk/kern/processor.h object, indexed by logical CPU number. */
extern processor_t processor_array[];
extern void lancelot_mali_observe(void);
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern void gic_v3_spi_mask(uint32_t);
extern void LancelotMaliPublish(void);
static LCK_GRP_DECLARE(mali_lck_grp,"lancelot-mali");
static LCK_MTX_DECLARE(mali_lock,&mali_lck_grp);
static struct lmp_state platform;
static struct lm_core *engine;
static uint32_t hakios_requested;
static int hkmt_boot(void);
static bool hkmt_ready(void);
static int hkmt_dvfs_guard(void);
static void hkmt_revoke(void);
int lancelot_hakios_backend(void) {
    uint32_t on=0;(void)PE_parse_boot_argn("lancelot_hakios",&on,sizeof(on));return on==1;
}
static bool mali_driver_ready(void) {return hakios_requested?hkmt_ready():lm_core_ready(engine);}
static int mali_driver_dvfs_guard(struct lm_job_result *r) {
    if(hakios_requested){if(r)bzero(r,sizeof(*r));return hkmt_dvfs_guard();}
    return lm_core_dvfs_guard(engine,r);
}
static int mali_driver_dvfs_prepare(struct lm_job_result *r) {
    return hakios_requested?mali_driver_dvfs_guard(r):lm_core_dvfs_prepare(engine,r);
}
static vm_offset_t banks[LMP_BANK_COUNT];
static vm_offset_t pwrap_bank;
static struct lmp_pmic_state pmic;
static bool pmic_valid;
static bool initialized, usable;
static uint32_t cache_line=64;
static uint32_t profile_jobs;
static struct lm_profile_policy profile_policy;
static struct lm_first_failure first_failure;
static thread_call_t activity_call;
static thread_call_t dvfs_call;
static struct lgd_sampler dvfs_load;
static bool dvfs_poll_enabled; /* Access only through acquire/release atomics. */
static int dvfs_target_error=LM_ENOTSUP;
static uint64_t dvfs_admit_cached,dvfs_admit_full,dvfs_admit_wall_ns;
static struct lm_job_result admission_sample;
static void snapshot_dvfs_sample(struct lm_job_result *);
static uint64_t dvfs_report_at;
static uint32_t native_dvfs_requested;
static bool native_control_ready,native_draining,native_final_context;
static bool gpu_returning,gpu_return_prepared,gpu_return_final_done;
static struct lm_core_quiescence gpu_return_backing;
static struct lm_gpu_dvfs_limit_provider gpu_limit_provider;
static int dvfs_before_job(void *);
static void dvfs_job_done(void *,const struct lm_job_result *,int);
static int start_dvfs_executor_locked(void);
static int native_runtime_guard(void *);
static void native_runtime_revoke(void *,int);
static int native_limit_read(void *,struct lmp_dvfs_limits *);
static bool hakios_queue_depth(uint32_t *);
static uint64_t dvfs_backlog_boosts;
static bool gpu_is_returning(void) {return __atomic_load_n(&gpu_returning,__ATOMIC_ACQUIRE);}
static uint32_t native_mpidr(void) {
    uint64_t value;__asm__ volatile("mrs %0,mpidr_el1":"=r"(value));return value&0xffffffu;
}
static void native_executor_assert(void) {
    /* Frozen final context intentionally uses no abandoned scheduler mutex. */
    if(!native_final_context)LCK_MTX_ASSERT(&mali_lock,LCK_MTX_ASSERT_OWNED);
}
static struct {
    uint64_t attempts,last_sequence,success_sequence,started_ns,done_ns;
    uint64_t run_ns,maintenance_ns,scope_ns;
    int32_t pid,error;
} activity;
static vm_offset_t cpu_clock_bank;
static uint32_t cpu_previous[8][4];
static uint64_t cpu_previous_at[8];
static struct lancelot_cpu_profile cpu_profile;
static LCK_MTX_DECLARE(cpu_metrics_lock,&mali_lck_grp);
static thread_call_t cpu_metrics_call;
static void cpu_profile_copy(struct lancelot_cpu_profile *out) {
    lck_mtx_lock(&cpu_metrics_lock);*out=cpu_profile;lck_mtx_unlock(&cpu_metrics_lock);
}
static uint64_t gpu_previous_at,gpu_previous_jobs,gpu_previous_run,gpu_previous_maintenance;
static struct {uint64_t maps,unmaps,map_pages,unmap_pages,map_ns,unmap_ns,errors;} vm_metrics;
static void start_activity_observer(void);
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
int lm_gpu_register_dvfs_limits(const struct lm_gpu_dvfs_limit_provider *provider) {
    if(gpu_is_returning())return LM_ENODEV;
    if(!provider||!provider->read)return LM_EINVAL;
    if(!ml_get_interrupts_enabled()||ml_at_interrupt_context())return LM_EBUSY;
    lck_mtx_lock(&mali_lock);
    int error=gpu_is_returning()?LM_ENODEV:gpu_limit_provider.read?LM_EBUSY:LM_OK;
    if(!error)gpu_limit_provider=*provider;
    lck_mtx_unlock(&mali_lock);return error;
}
static int pwrap_acquire(__unused void *c,uint64_t timeout_ns) {
    if(native_final_context)return lancelot_pwrap_return_acquire(timeout_ns);
    return pwrap_owner.acquire?pwrap_owner.acquire(pwrap_owner.cookie,timeout_ns):LM_ENOTSUP;
}
static void pwrap_release(__unused void *c) {
    if(native_final_context) {lancelot_pwrap_return_release();return;}
    if(pwrap_owner.release)pwrap_owner.release(pwrap_owner.cookie);
}
static void pwrap_poison(__unused void *c) {
    if(native_final_context) {(void)lancelot_pwrap_return_poison();return;}
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
static uint64_t thread_ns(__unused void *c) {uint64_t ns;absolutetime_to_nanoseconds(thread_get_runtime_self(),&ns);return ns;}
static void wait_us(__unused void*c,uint32_t us) {delay_for_interval(us,NSEC_PER_USEC);}
/* Deliberately preemptible: no interrupt masking or preemption disable.
 * machine_delay_until has no stopped-clock iteration guard; retain one here
 * so a broken timer cannot hang inside the core's otherwise bounded polls. */
static int spin_us(__unused void*c,uint32_t us) {
    if(!us||us>10)return LM_EINVAL;
    uint64_t interval=0;clock_interval_to_absolutetime_interval(us,NSEC_PER_USEC,&interval);
    if(!interval)return LM_ETIMEDOUT;
    uint64_t start=mach_absolute_time();
    for(uint32_t i=0;i<1000000;i++) {
        uint64_t now=mach_absolute_time();
        if(now<start)return LM_ETIMEDOUT;
        if(now-start>=interval)return LM_OK;
        __asm__ volatile("yield":::"memory");
    }
    return LM_ETIMEDOUT;
}
static int read_bank(__unused void*c,enum lmp_bank bank,uint32_t off,uint32_t*out) {
    static const vm_offset_t pa[LMP_BANK_COUNT]={0x10006000,0x10001000,0x13000000,0x13040000,0x10000000,0x1000c000};
    uint32_t size;
    if(!out||bank<0||bank>=LMP_BANK_COUNT||(off&3))return LM_EINVAL;
    size=bank==LMP_BANK_GPU?0x4000u:0x1000u;if(off>=size)return LM_EINVAL;
    if(native_final_context&&!banks[bank])return LM_ENODEV;
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
    if(native_final_context&&!pwrap_bank)return LM_ENODEV;
    if(!pwrap_bank)pwrap_bank=ml_io_map(LMP_PMIC_WRAP_BASE_PHYS,0x1000);
    if(!pwrap_bank)return LM_ENOMEM;__asm__ volatile("dsb sy":::"memory");
    *out=*(volatile uint32_t*)(pwrap_bank+off);__asm__ volatile("dsb sy":::"memory");return 0;
}
static int pwrap_write(__unused void*c,uint32_t off,uint32_t value) {
    if((off&3)||off>=0x1000u)return LM_EINVAL;
    if(native_final_context&&!pwrap_bank)return LM_ENODEV;
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
static int firmware_gpu_handoff_fact(void) {
    lck_mtx_lock(&mali_lock);
    if(gpu_handoff_checked) {int e=gpu_handoff_fact_status;lck_mtx_unlock(&mali_lock);return e;}
    gpu_handoff_checked=true;
    DTEntry chosen;const void *prop;unsigned bytes;uint64_t usb[3];struct lgh_locator locator;
    uint32_t requested=0;PE_parse_boot_argn("lancelot_gpu_lease",&requested,sizeof(requested));
    int e=0;
    if(!board_matches()||SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess) {e=LM_EIO;goto done;}
    if(SecureDTGetProperty(chosen,"lancelot-gpu-handoff",&prop,&bytes)!=kSuccess) {
        e=requested?LM_EIO:0;goto done;
    }
    if(requested!=1||bytes!=sizeof(locator)) {e=LM_EIO;goto done;}
    memcpy(&locator,prop,sizeof(locator));
    if(SecureDTGetProperty(chosen,"lancelot-usb-log",&prop,&bytes)!=kSuccess||bytes!=sizeof(usb)) {e=LM_EIO;goto done;}
    memcpy(usb,prop,sizeof(usb));
    if(usb[0]!=UINT64_C(0x31474f4c4253554c)||!usb[2]) {e=LM_EIO;goto done;}
    uint64_t own_pa=kvtophys((vm_offset_t)&gpu_handoff_slot.bundle);
    e=lgh_consume_kernel_slot(&locator,usb[2],own_pa,&gpu_handoff_slot,barrier,NULL,&gpu_handoff_fact);
    if(!e) {
        gpu_handoff_fact_status=1;
        printf("lancelot-gpu-handoff: AP_ONLY_fact=1 boot=%#llx nonce=%#llx PA=%#llx storage=kernel_owned_DATA peer_verified=0 DVFS_grant=0\n",
            gpu_handoff_fact.boot_id,gpu_handoff_fact.nonce,own_pa);
    }
 done:
    if(e) {gpu_handoff_fact_status=e;printf("lancelot-gpu-handoff: rejected=%d no_external_PA_map=1 no_DVFS_grant=1\n",e);}
    int status=gpu_handoff_fact_status;lck_mtx_unlock(&mali_lock);return status;
}
static void for_device(__unused void*c,void*p,size_t n) {
    lm_cache_clean_lines(p,n,cache_line);
    barrier(NULL);
}
static void for_cpu(__unused void*c,void*p,size_t n) {
    lm_cache_cpu_lines(p,n,cache_line);
    barrier(NULL);
}
static void pre_job_log(__unused void*c,uint64_t jc,const uint32_t words[4]) {
    printf("lancelot-mali: pre-job jc=%llx words=%08x %08x %08x %08x\n",
        (unsigned long long)jc,words[0],words[1],words[2],words[3]);
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
        else {if(!e)e=closed;printf("lancelot-mali: private DMA VM retirement failed=%d; backing retained, no service\n",closed);}
    } else {if(!e)e=LM_EIO;printf("lancelot-mali: DMA state unknown; test VM/pages quarantined until reboot\n");}
    return e;
}
void lancelot_mali_init(void) {
    uint32_t mode=0;
    if(!PE_parse_boot_argn("lancelot_mali",&mode,sizeof(mode))||mode==0)return;
    PE_parse_boot_argn("lancelot_hakios",&hakios_requested,sizeof(hakios_requested));
    uint32_t old_queue=0;PE_parse_boot_argn("lancelot_mali_queue",&old_queue,sizeof(old_queue));
    if(hakios_requested>1 || (hakios_requested && old_queue)) {
        printf("lancelot-mali: conflicting/invalid driver selection; no GPU access\n");return;
    }
    PE_parse_boot_argn("lancelot_gpu_dvfs",&native_dvfs_requested,sizeof(native_dvfs_requested));
    if(native_dvfs_requested>2||(native_dvfs_requested&&mode!=3&&mode!=4)) {
        printf("lancelot-mali: invalid native DVFS/mode; no GPU access\n");return;
    }
    if(mode!=1&&mode!=3&&mode!=4) {printf("lancelot-mali: rejected mode=%u; no MMIO\n",mode);return;}
    if(mode==1) {(void)firmware_gpu_handoff_fact();firmware_gpu_receipt();lancelot_mali_observe();return;}
    if(!board_matches()) {printf("lancelot-mali: board contract rejected; no MMIO\n");return;}
    if(firmware_gpu_handoff_fact()<0) {printf("lancelot-mali: invalid requested GPU handoff; no GPU access/service\n");return;}
    if(native_dvfs_requested&&gpu_handoff_fact_status!=1) {
        printf("lancelot-mali: native AP DVFS requires existing kernel-owned AP writer/MAINPLL-reference facts, not a firmware DVFS grant; no GPU access\n");return;
    }
    int he = lancelot_pwrap_ensure_board_handoff();
    if (he) {
        printf("lancelot-mali: shared WACS2 board handoff unavailable=%d; no GPU access\n", he);
        return;
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
    /* Boot firmware can leave MFG powered; lmp_reset_release takes such a
     * domain over in full mode instead of refusing it. Report the inherited
     * bits so the log says which state the AP inherited. */
    printf("lancelot-mali: MFG inherited MFG/CORE0/CORE1/ASYNC=%d/%d/%d/%d\n",
        platform.initial_domain_on[0]?1:0,platform.initial_domain_on[1]?1:0,
        platform.initial_domain_on[2]?1:0,platform.initial_domain_on[3]?1:0);
    if(e||mode==1)goto done;
    if((e=lmp_start(&platform))) {printf("lancelot-mali: platform start failed=%d stage=%u; no service\n",e,platform.last_stage?platform.last_stage:0);goto done;}
    uint32_t identity=0, gpu_status=0;
    e=lmp_gpu_read32(&platform,LMP_GPU_ID,&identity);
    if(!e)e=lmp_gpu_read32(&platform,LMP_GPU_STATUS,&gpu_status);
    printf("lancelot-mali: powered GPU_ID=%#x STATUS=%#x result=%d before private DMA\n",identity,gpu_status,e);
    if(e)goto done;
    for(uint32_t spi=0x112;spi<=0x116;spi++)gic_v3_spi_mask(32+spi);
    core_mmio_failed=false;
    if(hakios_requested) {
        PE_parse_boot_argn("lancelot_profile",&profile_jobs,sizeof(profile_jobs));
        e=hkmt_boot();usable=!e&&hkmt_ready();
        goto rendering_backend_ready;
    }
    uint32_t preflush=1,jobdump=0,preasflush=0,gpushare=1,cachebatch=0,submitscope=0,gpuonly=0,poll=1,faultdesc=0,queued=0;
    PE_parse_boot_argn("lancelot_mali_queue",&queued,sizeof(queued));
    PE_parse_boot_argn("lancelot_preflush",&preflush,sizeof(preflush));
    PE_parse_boot_argn("lancelot_jobdump",&jobdump,sizeof(jobdump));
    PE_parse_boot_argn("lancelot_faultdesc",&faultdesc,sizeof(faultdesc));
    printf("lancelot-mali: faultdesc=%u (retained CPU descriptors after clean, before launch; no fault-time buffer reads)\n",faultdesc==1);
    PE_parse_boot_argn("lancelot_preasflush",&preasflush,sizeof(preasflush));
    PE_parse_boot_argn("lancelot_gpushare",&gpushare,sizeof(gpushare));
    PE_parse_boot_argn("lancelot_cachebatch",&cachebatch,sizeof(cachebatch));
    PE_parse_boot_argn("lancelot_profile",&profile_jobs,sizeof(profile_jobs));
    PE_parse_boot_argn("lancelot_submitscope",&submitscope,sizeof(submitscope));
    PE_parse_boot_argn("lancelot_gpuonly",&gpuonly,sizeof(gpuonly));
    PE_parse_boot_argn("lancelot_poll",&poll,sizeof(poll));
    if(poll>1){printf("lancelot-mali: invalid poll=%u (0=legacy 1=bounded hybrid)\n",poll);goto done;}
    if(preasflush>2) {printf("lancelot-mali: invalid preasflush=%u (0=off 1=mem 2=pt)\n",preasflush);goto done;}
    if(gpushare>1) {printf("lancelot-mali: invalid gpushare=%u (0=legacy 1=GPU-inner)\n",gpushare);goto done;}
    if(cachebatch>1||profile_jobs>2||submitscope>1||gpuonly>1||(gpuonly&&!submitscope)) {printf("lancelot-mali: invalid cachebatch/submitscope/gpuonly (0 or 1) or profile (0..2); gpuonly requires scope\n");goto done;}
    printf("lancelot-mali: preflush=%u jobdump=%u (first 32 jobs, CPU view)\n",preflush!=0,jobdump!=0);
    printf("lancelot-mali: preasflush=%u (0=off 1=FLUSH_MEM 2=FLUSH_PT)\n",preasflush);
    printf("lancelot-mali: gpushare=%u (GPU PTE SH=%u MEMATTR[0]=0x88; CPU coherency disabled)\n",gpushare,gpushare?3u:0u);
    printf("lancelot-mali: cachebatch=%u (max 64 CPU-VA pages/range; all cache lines maintained) profile=%u\n",cachebatch,profile_jobs);
    if(profile_jobs==2)printf("lancelot-mali: steady profile=2 (first192; then <=1 pair/second per requirement; clock records, no lifetime cap)\n");
    printf("lancelot-mali: inherited source=%uHz (PLL/mux-derived, not a runtime frequency measurement)\n",platform.source_hz);
    printf("lancelot-mali: submitscope=%u (1=BO-list PTE isolation; aliases/OOM keep full-VM maintenance)\n",submitscope);
    printf("lancelot-mali: scopealgo=2 (inactive leaf-parent masks, mixed-leaf PTEs) cacheloop=4 (all lines, range DSB retained)\n");
    printf("lancelot-mali: faultmeta=1 (pre-reset retained VA/workset/PTE snapshot; no GPU data reads)\n");
    printf("lancelot-mali: gpuonly=%u (trusted never-exported WC BOs; initial clean and explicit CPU handoff; GPU flush/wait retained)\n",gpuonly);
    printf("lancelot-mali: access=1 (scoped GPU RO PTEs before post-DMA maintenance reduction; aliases/OOM retain full VM)\n");
    printf("lancelot-mali: poll=%u (preemptible requested-spin budgets: control50us job100us; subsequent requested sleeps50us; deadlines/faults retained)\n",poll);
    struct lm_ops ops={.cookie=NULL,.read32=gpu_read,.write32=gpu_write,.now_ns=clock_ns,.thread_ns=thread_ns,.delay_us=wait_us,
        .alloc=meta_alloc,.free=meta_free,.page_alloc=page_alloc,.page_free=page_free,
        .for_device=for_device,.for_cpu=for_cpu,.barrier=barrier,.io_error=core_mmio_error,
        .pre_job_flush=preflush!=0,.pre_job=jobdump?pre_job_log:NULL,.capture_descriptors=faultdesc==1,
        .g52_walk_nc=queued==1,.pre_as_flush=preasflush,.gpu_shareable=gpushare!=0,
        .cache_batch=cachebatch!=0,.profile_jobs=profile_jobs!=0,.submit_scope=submitscope!=0,.gpu_only_sync=gpuonly!=0,
        .table_dirty_sync=true,
        .spin_us=poll?spin_us:NULL,.before_job=dvfs_before_job,.job_done=dvfs_job_done};
    engine=lm_core_create(&ops);
    if(!engine){printf("lancelot-mali: engine allocation failed\n");goto done;}
    e=lm_core_start(engine);if(e){printf("lancelot-mali: engine start=%d\n",e);goto done;}
    printf("lancelot-mali: address_profile=%s levels=%u transcfg=%#x requested_queue=%u\n",
        queued==1?"MT6768_VA35_WALK_NC":"legacy_WALK_WB",queued==1?3u:4u,
        queued==1?(6u|(20u<<6)|(1u<<24)):(6u|(2u<<24)|(1u<<30)),queued);
    e=private_dma_test();
    usable=!e&&mali_driver_ready();
    printf("lancelot-mali: DMA smoke=%d ready=%u; render service requested=%u\n",e,usable,mode==4);
 rendering_backend_ready:
    if(usable) {
        int start=start_dvfs_executor_locked();
        if(start&&native_dvfs_requested) {
            native_runtime_revoke(NULL,start);
            printf("lancelot-mali: native DVFS init failed=%d; no render service\n",start);
        }
    }
 done:
    lck_mtx_unlock(&mali_lock);
    if(usable&&mode==4)LancelotMaliPublish();
    if((engine||hakios_requested)&&profile_jobs==2)start_activity_observer();
}
/* Explicit native AP policy, using retained AP writer/MAINPLL facts and live
 * hardware checks. AP_ONLY is not reinterpreted as a firmware/all-peer grant. */
static int native_sources_ready(void) {
    native_executor_assert();
    if(!native_dvfs_requested||gpu_handoff_fact_status!=1||!pmic_valid||
        !lgh_offer_valid(&gpu_handoff_fact,gpu_handoff_fact.boot_id))return LM_ENODEV;
    const uint32_t offset[3]={LMP_APMIXED_MAINPLL_CON0,LMP_APMIXED_MAINPLL_CON0+4,LMP_APMIXED_MAINPLL_CON3};
    const uint32_t expected[3]={gpu_handoff_fact.main_con0,gpu_handoff_fact.main_con1,gpu_handoff_fact.main_con3};
    for(unsigned pass=0;pass<2;pass++) {
        for(unsigned i=0;i<3;i++) {
            uint32_t value=0;int error=read_bank(NULL,LMP_BANK_APMIXED,offset[i],&value);
            if(error)return error;
            if(value!=expected[i])return LM_EIO;
        }
        uint32_t hp=0;int error=read_bank(NULL,LMP_BANK_APMIXED,LMP_APMIXED_FHCTL_HP_EN,&hp);
        if(error)return error;
        if(hp&LMP_FHCTL_MFGPLL_HP_MASK)return LM_ENOTSUP; /* Never clear an active DDS/SSC owner's bit. */
    }
    return lmp_pmic_native_ap_ready(&pmic);
}
static int native_runtime_guard(__unused void *cookie) {
    native_executor_assert();
    if(!native_control_ready||!usable||core_mmio_failed||!mali_driver_ready())return LM_ENODEV;
    if(native_final_context) {
        if(!__atomic_load_n(&gpu_return_prepared,__ATOMIC_ACQUIRE)||
            native_mpidr()!=gpu_handoff_fact.boot_mpidr)return LM_EIO;
    } else if(gpu_is_returning()&&!native_draining)return LM_EBUSY;
    struct lm_job_result result;int error=mali_driver_dvfs_guard(&result);
    return error?error:native_sources_ready();
}
static void dvfs_stop_locked(void) {
    native_executor_assert();
    __atomic_store_n(&dvfs_poll_enabled,false,__ATOMIC_RELEASE);
    lgd_sampler_stop(&dvfs_load);
    /* Permanent call/backing: an already racing rearm can queue one last call,
     * which observes stopped before lock/work. No wait/cancel/free here. */
}
static void dvfs_revoke_locked(int error) {
    native_executor_assert();
    usable=false;dvfs_target_error=error;
    platform.dvfs.attached=false;platform.dvfs.meter_valid=false;platform.dvfs.limit_policy_valid=false;
    if(!platform.dvfs.error)platform.dvfs.error=error;
    if(hakios_requested)hkmt_revoke();else lm_core_external_revoke(engine);dvfs_stop_locked();
    /* Unknown external resources/backing stay retained; no release/restore. */
}
static void native_runtime_revoke(__unused void *cookie,int error) {
    native_control_ready=false;dvfs_revoke_locked(error);
}
static int native_limit_read(__unused void *cookie,struct lmp_dvfs_limits *out) {
    LCK_MTX_ASSERT(&mali_lock,LCK_MTX_ASSERT_OWNED);
    if(!out)return LM_EINVAL;
    *out=(struct lmp_dvfs_limits){0};
    return gpu_limit_provider.read?gpu_limit_provider.read(gpu_limit_provider.cookie,out):LM_ENODEV;
}
static int dvfs_service_locked(bool admission) {
    LCK_MTX_ASSERT(&mali_lock,LCK_MTX_ASSERT_OWNED);
    if(!platform.dvfs.attached)return LM_OK; /* Observation only, no MMIO. */
    if(!dvfs_load.running||!usable||!mali_driver_ready())return LM_ENODEV;
    dvfs_load.current_opp=platform.dvfs.current_opp;
    uint32_t target=dvfs_load.demand_valid?dvfs_load.demand_opp:platform.dvfs.current_opp;
    /* Even a same-OPP request rechecks real caps, clock/rails/meter and all
     * mandatory guards before the next job. reported/applied are not demand. */
    bool reused=false;uint64_t began=clock_ns(NULL);
    int e=admission&&hakios_requested?
        lmp_dvfs_admit(&platform,target,UINT64_C(10000000),&reused):lmp_dvfs_target(&platform,target);
    if(admission){dvfs_admit_wall_ns+=clock_ns(NULL)-began;if(reused)++dvfs_admit_cached;else ++dvfs_admit_full;}
    dvfs_target_error=e;
    if(!e)(void)lgd_sampler_applied(&dvfs_load,platform.dvfs.current_opp);
    else if(e!=LM_EBUSY)dvfs_revoke_locked(e);
    return e;
}
static int dvfs_before_job(__unused void *cookie) {
    LCK_MTX_ASSERT(&mali_lock,LCK_MTX_ASSERT_OWNED);
    /* A rejected admission must never inherit an earlier job's meter sample. */
    bzero(&admission_sample,sizeof(admission_sample));
    if(gpu_is_returning())return LM_ENODEV;
    if(!dvfs_load.running)return platform.dvfs.attached?LM_ENODEV:LM_OK;
    if(!lgd_sampler_idle(&dvfs_load,clock_ns(NULL))&&platform.dvfs.attached) {
        dvfs_revoke_locked(LM_EIO);return LM_EIO;
    }
    if(native_dvfs_requested==2&&hakios_requested) {
        uint32_t depth;
        if(hakios_queue_depth(&depth)&&depth>=8u) {
            uint64_t now=clock_ns(NULL);
            if(!lgd_sampler_hold(&dvfs_load,0,now)) {
                dvfs_revoke_locked(LM_EIO);return LM_EIO;
            }
            lgd_sampler_demand(&dvfs_load,now);dvfs_backlog_boosts++;
        }
    }
    int e=dvfs_service_locked(true);snapshot_dvfs_sample(&admission_sample);return e;
}
static void dvfs_job_done(__unused void *cookie,const struct lm_job_result *r,int error) {
    LCK_MTX_ASSERT(&mali_lock,LCK_MTX_ASSERT_OWNED);
    if(!dvfs_load.running)return;
    bool invalid=error||!r||!r->jm.launched||!r->jm.idle_verified;
    if(invalid)
        lgd_sampler_break(&dvfs_load,clock_ns(NULL));
    else invalid=!lgd_sampler_job(&dvfs_load,r->jm.start_ns,r->jm.end_ns);
    if(invalid&&platform.dvfs.attached)dvfs_revoke_locked(LM_EIO);
    if(!mali_driver_ready())dvfs_stop_locked();
}
static void dvfs_tick(__unused thread_call_param_t a,__unused thread_call_param_t b) {
    if(gpu_is_returning())return;
    if(!__atomic_load_n(&dvfs_poll_enabled,__ATOMIC_ACQUIRE))return;
    if(!lck_mtx_try_lock(&mali_lock)) {
        /* A job owns this serializer. Its completion accounts for every real
         * JM interval across skipped ticks; lock contention is not zero load. */
        if(__atomic_load_n(&dvfs_poll_enabled,__ATOMIC_ACQUIRE)) {
            uint64_t deadline;clock_interval_to_deadline(50,NSEC_PER_MSEC,&deadline);
            thread_call_enter_delayed(dvfs_call,deadline);
        }
        return;
    }
    if(dvfs_load.running) {
        uint64_t now=clock_ns(NULL);
        if(!usable||!mali_driver_ready())dvfs_stop_locked();
        else {
            if(!lgd_sampler_idle(&dvfs_load,now)&&platform.dvfs.attached)dvfs_revoke_locked(LM_EIO);
            else (void)dvfs_service_locked(false);
            if(!dvfs_report_at||now<dvfs_report_at||now-dvfs_report_at>=UINT64_C(1000000000)) {
                printf("lancelot-dvfs-load: at=%llu windows=%llu gaps=%llu valid=%u busy_ns=%llu jobs_overlap=%u demand_opp=%u current_opp=%u current_verified=%u attached=%u target_error=%d source=JM_wall_not_HW\n",
                    now/1000,dvfs_load.windows,dvfs_load.discontinuities,dvfs_load.demand_valid,
                    dvfs_load.last.busy_ns,dvfs_load.last.jobs,dvfs_load.demand_opp,dvfs_load.current_opp,
                     platform.dvfs.attached&&platform.dvfs.meter_valid,platform.dvfs.attached,dvfs_target_error);
                printf("lancelot-dvfs-applied: at=%llu up=%llu down=%llu transitions=%u hz=%u measured_hz=%u measured_at_ns=%llu VGPU=%u VSRAM=%u valid=%u pressure_jobs=%llu last_jm_ns=%llu max_jm_ns=%llu hold_until_ns=%llu hold_opp=%u hold_valid=%u missing=%u max_hz=%u\n",
                    now/1000,dvfs_load.applied_up,dvfs_load.applied_down,platform.dvfs.transitions,
                    platform.source_hz,platform.dvfs.measured_hz,platform.dvfs.measured_ns,
                    platform.dvfs.rails.uv[0],platform.dvfs.rails.uv[1],
                    platform.dvfs.attached&&platform.dvfs.meter_valid,dvfs_load.pressure_jobs,
                    dvfs_load.last_job_ns,dvfs_load.max_job_ns,dvfs_load.burst_until_ns,
                    dvfs_load.burst_opp,dvfs_load.burst_valid,platform.dvfs.provider_missing_mask,
                    platform.dvfs.effective_max_hz);
                dvfs_report_at=now;
                printf("lancelot-dvfs-admit: at=%llu cached=%llu full=%llu wall_us=%llu max_meter_age_us=10000 fresh_guards=1 backlog_boosts=%llu\n",
                    now/1000,dvfs_admit_cached,dvfs_admit_full,dvfs_admit_wall_ns/1000,dvfs_backlog_boosts);
            }
        }
        if(dvfs_load.running) {
            uint64_t deadline;clock_interval_to_deadline(50,NSEC_PER_MSEC,&deadline);
            thread_call_enter_delayed(dvfs_call,deadline);
        }
    }
    lck_mtx_unlock(&mali_lock);
}
static int start_dvfs_executor_locked(void) {
    LCK_MTX_ASSERT(&mali_lock,LCK_MTX_ASSERT_OWNED);
    uint32_t index;
    /* Register-derived inherited OPP is used only as the demand-policy seed,
     * not a measured/verified applied state, provider or execution grant. */
    if(dvfs_call)return LM_EBUSY;
    if(!usable||!mali_driver_ready()||!lgd_index_from_hz(platform.source_hz,&index))
        return native_dvfs_requested?LM_ENOTSUP:LM_OK;
    dvfs_call=thread_call_allocate(dvfs_tick,NULL);
    if(!dvfs_call) {printf("lancelot-mali: DVFS idle call allocation failed; governor not started\n");return LM_ENOMEM;}
    if(native_dvfs_requested) {
        int error=native_sources_ready();struct lm_job_result result;
        if(!error)error=mali_driver_dvfs_prepare(&result);
        if(error)return error;
        native_control_ready=true;
        const struct lmp_dvfs_ops ops={.control_policy=LMP_DVFS_NATIVE_AP,
            .guard=native_runtime_guard,.revoke=native_runtime_revoke,.limits=native_limit_read,
            .missing_provider_max_hz=native_dvfs_requested==2?LGD_MAX_KHZ*1000u:0};
        if((error=lmp_dvfs_attach(&platform,&ops)))return error;
        if((error=lmp_dvfs_target(&platform,platform.dvfs.current_opp)))return error;
        index=platform.dvfs.current_opp;dvfs_target_error=LM_OK;
        printf("lancelot-dvfs: native_AP=1 attached=1 opp=%u hz=%u measured_hz=%u VGPU=%u VSRAM=%u optional_missing=%u peer_verified=0 firmware_grant=0 table=Android30 request_mode=%u max_hz=%u\n",
            index,platform.source_hz,platform.dvfs.measured_hz,platform.dvfs.rails.uv[0],platform.dvfs.rails.uv[1],platform.dvfs.provider_missing_mask,
            native_dvfs_requested,platform.dvfs.effective_max_hz);
    }
    (void)lgd_sampler_start(&dvfs_load,clock_ns(NULL),index);
    __atomic_store_n(&dvfs_poll_enabled,true,__ATOMIC_RELEASE);
    printf("lancelot-mali: DVFS demand=1 tick=50ms profile_independent=1 AP_backend_attached=%u native_AP=%u firmware_grant=0 request_mode=%u\n",platform.dvfs.attached,native_dvfs_requested!=0,native_dvfs_requested);
    uint64_t deadline;clock_interval_to_deadline(50,NSEC_PER_MSEC,&deadline);
    thread_call_enter_delayed(dvfs_call,deadline);
    return LM_OK;
}

int lm_gpu_return_prepare(void) {
    if(hakios_requested)return LM_ENOTSUP;
    if(lancelot_mali_queued_backend()) {
        int drained=lancelot_mali_queue_drain(UINT64_C(10000000000));
        if(drained)return drained;
    }
    if(__atomic_load_n(&gpu_return_prepared,__ATOMIC_ACQUIRE))return LM_OK;
    if(!engine&&gpu_handoff_slot.bundle.offer.magic!=LGH_MAGIC)return LM_OK;
    if(!native_dvfs_requested||!native_control_ready||!engine)return LM_ENOTSUP;
    if(!ml_get_interrupts_enabled()||ml_at_interrupt_context())return LM_EBUSY;
    __atomic_store_n(&gpu_returning,true,__ATOMIC_RELEASE);
    __atomic_store_n(&dvfs_poll_enabled,false,__ATOMIC_RELEASE);
    if(dvfs_call)(void)thread_call_cancel_wait(dvfs_call);
    if(activity_call)(void)thread_call_cancel_wait(activity_call);
    uint64_t start=clock_ns(NULL);bool locked=false;
    for(unsigned tries=0;tries<300000;tries++) {
        if(lck_mtx_try_lock(&mali_lock)) {locked=true;break;}
        uint64_t now=clock_ns(NULL);if(now<start||now-start>=UINT64_C(12000000000))break;
        wait_us(NULL,50);
    }
    if(!locked)return LM_ETIMEDOUT;
    native_draining=true;dvfs_stop_locked();
    struct lm_core_quiescence backing;
    int error=lm_core_return_guard(engine,&backing);
    if(!error)error=lmp_dvfs_target(&platform,LGD_OPP_COUNT-1u);
    if(!error)error=lm_core_return_guard(engine,&backing);
    if(!error&&pmic.command_pending)error=LM_EIO;
    if(!error)error=lancelot_pwrap_freeze_for_return(UINT64_C(10000000));
    if(!error) {
        gpu_return_backing=backing;
        __atomic_store_n(&gpu_return_prepared,true,__ATOMIC_RELEASE);
        printf("lancelot-dvfs-return: orderly=1 vm=%u mapped_refs=%u tables=%u backing_retained=1 WACS2_frozen=1 no_reset_or_free=1\n",
            backing.vm_count,backing.mapped_page_refs,backing.table_pages);
    } else printf("lancelot-dvfs-return: orderly failed=%d backing retained, no SAFE\n",error);
    native_draining=false;lck_mtx_unlock(&mali_lock);return error;
}
int lm_gpu_return_final(void) {
    if(hakios_requested)return LM_ENOTSUP;
    if(__atomic_load_n(&gpu_return_final_done,__ATOMIC_ACQUIRE))return LM_OK;
    if(!engine&&gpu_handoff_slot.bundle.offer.magic!=LGH_MAGIC)return LM_OK;
    if(!__atomic_load_n(&gpu_return_prepared,__ATOMIC_ACQUIRE)||!native_control_ready||
        native_mpidr()!=gpu_handoff_fact.boot_mpidr)return LM_EIO;
    int error=lancelot_pwrap_return_begin(gpu_handoff_fact.boot_mpidr);if(error)return error;
    native_final_context=true;
    struct lm_core_quiescence backing;struct lmp_dvfs_sample sample;
    error=lm_core_return_guard(engine,&backing);
    if(!error&&(backing.vm_count!=gpu_return_backing.vm_count||
        backing.mapped_page_refs!=gpu_return_backing.mapped_page_refs||backing.table_pages!=gpu_return_backing.table_pages))error=LM_EIO;
    if(!error)error=lmp_dvfs_verify(&platform,&sample);
    if(!error)error=lm_core_return_guard(engine,&backing);
    struct lgh_offer slot_offer;
    const volatile uint8_t *source=(const volatile uint8_t *)&gpu_handoff_slot.bundle.offer;
    for(unsigned i=0;i<sizeof(slot_offer);i++)((uint8_t *)&slot_offer)[i]=source[i];
    if(!error&&(!lgh_slot_header_valid((const struct lgh_kernel_slot *)(uintptr_t)&gpu_handoff_slot)||
        !lgh_bytes_same(&slot_offer,&gpu_handoff_fact,sizeof(slot_offer))))error=LM_EIO;
    if(error) {printf("lancelot-dvfs-return: final failed=%d backing retained, no SAFE\n",error);return error;}
    struct lgh_return proof={.magic=LGH_RETURN_MAGIC,.boot_id=slot_offer.boot_id,.nonce=slot_offer.nonce,
        .version=LGH_VERSION,.size=sizeof(proof),.state=LGH_RETURN_SAFE,.flags=LGH_RET_REQUIRED,
        .other_ap_off_mask=slot_offer.other_ap_off_mask,.gpu_hz=sample.clock.hz,
        .clock_verified=1,.rails_verified=1,.vgpu_uv=sample.rails.uv[0],.vsram_uv=sample.rails.uv[1],
        .mfg_parent=sample.clock.top&LMP_TOP_MFG_SEL_MASK,.mfg_con1=sample.clock.mfg[1],.measured_hz=sample.measured_hz};
    error=lgh_publish_return((struct lgh_bundle *)(uintptr_t)&gpu_handoff_slot.bundle,&proof);
    if(error)return error;
    for_device(NULL,(void *)(uintptr_t)&gpu_handoff_slot,sizeof(gpu_handoff_slot));
    __atomic_store_n(&gpu_return_final_done,true,__ATOMIC_RELEASE);
    printf("lancelot-dvfs-return: SAFE=1 AP_OFF=independent_PSCI ALL_JS_AS_idle=1 hz=%llu measured=%u VGPU=%u VSRAM=%u backing_retained=1 PoC_clean=1\n",
        proof.gpu_hz,proof.measured_hz,proof.vgpu_uv,proof.vsram_uv);
    return LM_OK;
}
static void report_first_failure_locked(uint64_t now_ns) {
    if(!lm_first_failure_should_report(&first_failure,now_ns))return;
    const struct lm_job_result *r=&first_failure.result;
    printf("lancelot-first-failure: q=%llu pid=%d req=%u error=%d at=%llu jc=%#llx stage=%u status=%#x reset=%u quarantine=%u\n",
        r->sequence,first_failure.pid,first_failure.requirements,first_failure.error,first_failure.at_ns/1000,
        first_failure.jc,r->failed_stage,r->job_status,r->reset_completed,r->quarantined);
    printf("lancelot-first-detail: q=%llu mmu=%#x gpu=%#x fault=%#llx reg=%#x expected=%#llx observed=%#llx\n",
        r->sequence,r->mmu_fault,r->gpu_fault,r->fault_address,r->mismatch_reg,r->expected,r->observed);
    printf("lancelot-first-state: q=%llu irq=%#x state=%#x head=%08x%08x tail=%08x%08x extra=%08x%08x gpu_status=%#x\n",
        r->sequence,r->job_irq,r->job_state,r->js_head_hi,r->js_head_lo,r->js_tail_hi,r->js_tail_lo,
        r->fault_extra_hi,r->fault_extra_lo,r->gpu_fault_status);
    printf("lancelot-first-vm: q=%llu valid=%u scope=%u mapped=%u selected=%u visible=%u va=%#llx pages=%u flags=%u pa=%#llx leaf=%#llx expected=%#llx\n",
        r->sequence,r->fault_vm.valid,r->fault_vm.scoped,r->fault_vm.mapped,
        r->fault_vm.selected,r->fault_vm.visible,r->fault_vm.va,r->fault_vm.pages,
        r->fault_vm.flags,r->fault_vm.page_pa,r->fault_vm.leaf_pte,r->fault_vm.expected_pte);
    printf("lancelot-first-descriptors: q=%llu token=%llu pid=%d mask=%#x ranges=%u ro_ranges=%u source=CPU_after_clean_before_launch\n",
        r->sequence,r->queue_token,first_failure.pid,r->descriptors.mask,
        r->descriptors.ranges,r->descriptors.readonly_ranges);
    printf("lancelot-first-dvfs: q=%llu valid=%u hz=%u measured_hz=%u sampled_ns=%llu VGPU=%u VSRAM=%u target_error=%d source=admission_before_launch\n",
        r->sequence,r->dvfs_sample.valid,r->dvfs_sample.hz,r->dvfs_sample.measured_hz,
        r->dvfs_sample.sampled_ns,r->dvfs_sample.vgpu_uv,r->dvfs_sample.vsram_uv,r->dvfs_sample.target_error);
    printf("lancelot-first-dvfs-after: q=%llu valid=%u target_error=%d source=cached_after_core_return\n",
        r->sequence,r->dvfs_after_sample.valid,r->dvfs_after_sample.target_error);
    for(unsigned index=0;index<4;index++)if(r->descriptors.mask&(1u<<index)) {
        printf("lancelot-first-descriptor-map: q=%llu record=%u va=%#llx bo=%#llx pages=%u\n",
            r->sequence,index,r->descriptors.va[index],r->descriptors.mapping[index],r->descriptors.pages[index]);
        for(unsigned word=0;word<16;word+=4)
            printf("lancelot-first-descriptor: q=%llu record=%u word=%u data=%08x,%08x,%08x,%08x\n",
                r->sequence,index,word,r->descriptors.words[index][word],r->descriptors.words[index][word+1],
                r->descriptors.words[index][word+2],r->descriptors.words[index][word+3]);
    }
    if(r->chain) {
        const struct lm_chain_snapshot *s=r->chain;
        uint64_t head=(uint64_t)r->js_head_lo|(uint64_t)r->js_head_hi<<32;
        uint64_t tail=(uint64_t)r->js_tail_lo|(uint64_t)r->js_tail_hi<<32;
        uint32_t matched=0;
        printf("lancelot-first-chain: q=%llu count=%u stop=%u stop_va=%#llx source=CPU_before_launch\n",r->sequence,s->count,s->stop_reason,s->stop_va);
        for(uint32_t i=0;i<s->count;i++) {
            const struct lm_chain_node *n=&s->nodes[i];
            bool fault=n->va==head || n->va==tail;matched+=fault;
            printf("lancelot-first-node: q=%llu index=%u va=%#llx next=%#llx bo=%#llx pages=%u type=%u words=%u fault_head=%u fault_tail=%u\n",
                r->sequence,i,n->va,n->next,n->bo_va,n->bo_pages,n->type,n->captured_words,n->va==head,n->va==tail);
            if(fault)for(uint32_t w=0;w<n->captured_words;w+=4)
                printf("lancelot-first-node-data: q=%llu index=%u word=%u data=%08x,%08x,%08x,%08x\n",r->sequence,i,w,n->words[w],n->words[w+1],n->words[w+2],n->words[w+3]);
        }
        printf("lancelot-first-chain-match: q=%llu matched=%u unavailable_is_not_zero_data=1\n",r->sequence,matched);
    }
}
void lm_gpu_report_first_failure(void) {if(gpu_is_returning())return;lck_mtx_lock(&mali_lock);report_first_failure_locked(clock_ns(NULL));lck_mtx_unlock(&mali_lock);}
/* Clock registers are always-on TOP/APMIXED/MCUCFG, never the GPU data/job banks.
 * Both CPU register sets are read twice. Unknown mux/divider or instability is
 * explicitly unavailable; no PLL/voltage/governor/PMU register is written. */
static void cpu_metrics_tick(uint64_t now) {
    struct lancelot_cpu_profile sample={.sampled_ns=now};
    static uint64_t last_report;
    static uint32_t reported_topology[8][5],reported_topology_mask;
    static bool topology_reported;
    uint32_t current_topology[8][5]={0},topology_mask=0;
    bool report=!last_report || now<last_report || now-last_report>=NSEC_PER_SEC;
    if(report)last_report=now;
    for(unsigned i=0;i<8;i++)sample.domain[i]=-1;
    const ml_topology_info_t *topology=ml_get_topology_info();
    if(topology)for(unsigned i=0;i<topology->num_cpus;i++) {
        const ml_topology_cpu_t *c=&topology->cpus[i];
        /* MT6768 shipping DT: MPIDR 0x000..0x500 use LL, 0x600/0x700 L.
         * Report raw topology as well: logical IDs are reordered by the PE. */
        int domain=(c->phys_id&~0x700u)?-1:(c->phys_id>=0x600u?1:0);
        if(c->cpu_id<8) {
            sample.domain[c->cpu_id]=domain;topology_mask|=1u<<c->cpu_id;
            uint32_t *entry=current_topology[c->cpu_id];
            entry[0]=c->phys_id;entry[1]=c->cluster_id;entry[2]=c->cluster_type;
            entry[3]=c->l2_access_penalty;entry[4]=(uint32_t)(domain+1);
        }
    }
    if(report && (!topology_reported || topology_mask!=reported_topology_mask ||
       memcmp(&reported_topology,&current_topology,sizeof(current_topology)))) {
        for(unsigned cpu=0;cpu<8;cpu++)if(topology_mask&(1u<<cpu)) {
            const uint32_t *entry=current_topology[cpu];
            printf("lancelot-cpu-topology: at=%llu cpu=%u phys=%x cluster=%u type=%u l2_penalty=%u clock_domain=%d source=EDT_and_MT6768_MPIDR\n",
                now/1000,cpu,entry[0],entry[1],entry[2],entry[3],(int)entry[4]-1);
        }
        memcpy(&reported_topology,&current_topology,sizeof(current_topology));
        reported_topology_mask=topology_mask;topology_reported=true;
    }
    for(unsigned cpu=0;cpu<8;cpu++) {
        processor_t p=processor_array[cpu];
        struct processor_cpu_load_info info={0};mach_msg_type_number_t count=PROCESSOR_CPU_LOAD_INFO_COUNT;host_t host;
        uint32_t delta[4]={0};bool valid=false;uint64_t from=cpu_previous_at[cpu];
        int error=p?processor_info(p,PROCESSOR_CPU_LOAD_INFO,&host,(processor_info_t)&info,&count):KERN_INVALID_ARGUMENT;
        if(!error&&count==PROCESSOR_CPU_LOAD_INFO_COUNT) {
            valid=from&&now>from&&lm_metrics_ticks(cpu_previous[cpu],info.cpu_ticks,delta);
            memcpy(cpu_previous[cpu],info.cpu_ticks,sizeof(cpu_previous[cpu]));cpu_previous_at[cpu]=now;
        } else cpu_previous_at[cpu]=0;
        sample.from_ns[cpu]=from;sample.busy_bps[cpu]=valid?lm_metrics_busy_bps(delta,CPU_STATE_IDLE):0;
        if(valid)sample.valid_mask|=1u<<cpu;
        if(report)printf("lancelot-cpu-load: at=%llu from=%llu cpu=%u valid=%u error=%d user=%u system=%u idle=%u nice=%u busy_bps=%u source=scheduler_ticks\n",
            now/1000,from/1000,cpu,valid,error,delta[CPU_STATE_USER],delta[CPU_STATE_SYSTEM],delta[CPU_STATE_IDLE],delta[CPU_STATE_NICE],
            valid?lm_metrics_busy_bps(delta,CPU_STATE_IDLE):0);
    }
    for(unsigned cluster=0;cluster<2;cluster++) {
        uint32_t off=cluster?0x208u:0x218u,a0=0,a1=0,b0=0,b1=0,ca=0,cb=0;uint64_t hz=0;
        bool valid=false;
        if(cpu_clock_bank && !read_bank(NULL,LMP_BANK_APMIXED,off,&a0) && !read_bank(NULL,LMP_BANK_APMIXED,off+4,&a1)) {
            __asm__ volatile("dsb sy":::"memory");ca=*(volatile uint32_t*)(cpu_clock_bank+0x2a0+4*cluster);
            int e0=read_bank(NULL,LMP_BANK_APMIXED,off,&b0),e1=read_bank(NULL,LMP_BANK_APMIXED,off+4,&b1);
            cb=*(volatile uint32_t*)(cpu_clock_bank+0x2a0+4*cluster);__asm__ volatile("dsb sy":::"memory");
            /* Only the verified ARMPLL path is an output-clock measurement.
             * A26MHz/other mux transition is raw evidence, not a guessed rate. */
            valid=!e0&&!e1&&a0==b0&&a1==b1&&ca==cb&&((ca>>9)&3u)==1u&&lm_metrics_cpu_hz(a0,a1,ca,&hz);
        }
        sample.hz[cluster]=valid?hz:0;if(valid)sample.hz_valid_mask|=1u<<cluster;
        if(report)printf("lancelot-cpu-clock: at=%llu cluster=%u valid=%u hz=%llu con0=%x con1=%x cfg=%x source=PLL_mux_derived\n",
            now/1000,cluster,valid,valid?hz:0,a0,a1,ca);
    }
    lck_mtx_lock(&cpu_metrics_lock);cpu_profile=sample;lck_mtx_unlock(&cpu_metrics_lock);
}
static void cpu_metrics_sample(__unused thread_call_param_t a,__unused thread_call_param_t b) {
    if(gpu_is_returning())return;
    cpu_metrics_tick(clock_ns(NULL));
    uint64_t deadline;clock_interval_to_deadline(250,NSEC_PER_MSEC,&deadline);
    if(!gpu_is_returning())thread_call_enter_delayed(cpu_metrics_call,deadline);
}
/* Continue reporting after a renderer failure. Clock readout is separate from
 * the unchanged first-fault snapshot. Trylock never waits behind GPU work. */
static void activity_tick(__unused thread_call_param_t a,__unused thread_call_param_t b) {
    if(gpu_is_returning())return;
    uint64_t now=clock_ns(NULL),deadline;
    if(lck_mtx_try_lock(&mali_lock)) {
        printf("lancelot-activity: at=%llu ready=%u attempts=%llu q=%llu okq=%llu pid=%d e=%d start=%llu done=%llu first=%u\n",
            now/1000,usable&&mali_driver_ready(),activity.attempts,activity.last_sequence,
            activity.success_sequence,activity.pid,activity.error,activity.started_ns/1000,
            activity.done_ns/1000,first_failure.valid);
        report_first_failure_locked(now);
        printf("lancelot-vm-cost: at=%llu maps=%llu unmaps=%llu map_pages=%llu unmap_pages=%llu map_us=%llu unmap_us=%llu errors=%llu cumulative=1\n",
            now/1000,vm_metrics.maps,vm_metrics.unmaps,vm_metrics.map_pages,vm_metrics.unmap_pages,
            vm_metrics.map_ns/1000,vm_metrics.unmap_ns/1000,vm_metrics.errors);
        if(gpu_previous_at&&now>gpu_previous_at) {
            printf("lancelot-gpu-soft-window: from=%llu to=%llu attempts=%llu run_us=%llu maintenance_us=%llu source=adapter_wall_not_HW_utilization\n",
                gpu_previous_at/1000,now/1000,activity.attempts-gpu_previous_jobs,
                (activity.run_ns-gpu_previous_run)/1000,(activity.maintenance_ns-gpu_previous_maintenance)/1000);
        }
        gpu_previous_at=now;gpu_previous_jobs=activity.attempts;gpu_previous_run=activity.run_ns;gpu_previous_maintenance=activity.maintenance_ns;
        uint64_t hz=0;bool fv=false;uint32_t top=0,con0=0,con3=0;
        if(usable&&mali_driver_ready()) {
            struct lmp_state first=platform,second=platform;
            int a=lmp_upstream_clock_ready(&first),b=lmp_upstream_clock_ready(&second);
            top=first.initial_top_mfg_cfg;con0=first.initial_mfgpll_con0;con3=first.initial_mfgpll_con3;
            fv=!a&&!b&&first.source_hz==second.source_hz&&first.initial_top_mfg_cfg==second.initial_top_mfg_cfg&&
               first.initial_mfgpll_con0==second.initial_mfgpll_con0&&first.initial_mfgpll_con3==second.initial_mfgpll_con3;
            if(fv)hz=first.source_hz;
        }
        printf("lancelot-gpu-clock: at=%llu valid=%u hz=%llu top=%x con0=%x con3=%x source=PLL_mux_derived thermal_valid=0 HW_util_valid=0\n",now/1000,fv,hz,top,con0,con3);
        lck_mtx_unlock(&mali_lock);
    } else printf("lancelot-activity: at=%llu lockbusy=1\n",now/1000);
    if(gpu_is_returning())return;
    clock_interval_to_deadline(5,NSEC_PER_SEC,&deadline);
    if(!gpu_is_returning())thread_call_enter_delayed(activity_call,deadline);
}
static void start_activity_observer(void) {
    uint64_t deadline;
    /* MT6768 shipping cpufreq provider: MCUCFG0x0c530000 +0xa2a0/+0xa2a4.
     * Limit the mapping to the always-on CPU divider register page. */
    if(board_matches())cpu_clock_bank=ml_io_map(0x0c53a000,0x1000);
    cpu_metrics_call=thread_call_allocate(cpu_metrics_sample,NULL);
    if(cpu_metrics_call) {
        cpu_metrics_tick(clock_ns(NULL));
        clock_interval_to_deadline(250,NSEC_PER_MSEC,&deadline);
        thread_call_enter_delayed(cpu_metrics_call,deadline);
    }
    activity_call=thread_call_allocate(activity_tick,NULL);
    if(!activity_call) {printf("lancelot-mali: activity observer allocation failed\n");return;}
    printf("lancelot-mali: activity=1 (5s cached first failure; no GPU job/data-bank reads or recovery)\n");
    printf("lancelot-mali: metrics=2 (250ms CPU samples, 1s reports and per-slow-job snapshot; scheduler load and read-only PLL/mux clocks)\n");
    clock_interval_to_deadline(5,NSEC_PER_SEC,&deadline);
    thread_call_enter_delayed(activity_call,deadline);
}
bool lm_gpu_ready(void) {if(gpu_is_returning())return false;lck_mtx_lock(&mali_lock);bool ok=!gpu_is_returning()&&usable&&mali_driver_ready();lck_mtx_unlock(&mali_lock);return ok;}
int lm_gpu_param(uint32_t p,uint64_t*out) {if(gpu_is_returning())return LM_ENODEV;lck_mtx_lock(&mali_lock);int e=gpu_is_returning()?LM_ENODEV:lm_core_get_param(engine,p,out);lck_mtx_unlock(&mali_lock);return e;}
struct lm_vm *lm_gpu_vm_new(void) {if(gpu_is_returning())return NULL;lck_mtx_lock(&mali_lock);struct lm_vm*v=!gpu_is_returning()&&usable?lm_core_vm_new(engine):NULL;lck_mtx_unlock(&mali_lock);return v;}
int lm_gpu_vm_destroy(struct lm_vm*v) {if(gpu_is_returning())return LM_EBUSY;lck_mtx_lock(&mali_lock);int e=gpu_is_returning()?LM_EBUSY:lm_core_vm_destroy(v);lck_mtx_unlock(&mali_lock);return e;}
int lm_gpu_vm_map(struct lm_vm*v,uint64_t va,const struct lm_page*p,uint32_t n,uint32_t flags) {
    if(gpu_is_returning())return LM_ENODEV;
    lck_mtx_lock(&mali_lock);uint64_t start=profile_jobs==2?clock_ns(NULL):0;
    int e=gpu_is_returning()?LM_ENODEV:lm_core_vm_map(v,va,p,n,flags);
    if(start){vm_metrics.maps++;vm_metrics.map_pages+=n;vm_metrics.map_ns+=clock_ns(NULL)-start;vm_metrics.errors+=e!=0;}
    lck_mtx_unlock(&mali_lock);return e;
}
int lm_gpu_vm_unmap(struct lm_vm*v,uint64_t va,uint32_t n) {
    if(gpu_is_returning())return LM_EBUSY;
    lck_mtx_lock(&mali_lock);uint64_t start=profile_jobs==2?clock_ns(NULL):0;
    int e=gpu_is_returning()?LM_EBUSY:lm_core_vm_unmap(v,va,n);
    if(start){vm_metrics.unmaps++;vm_metrics.unmap_pages+=n;vm_metrics.unmap_ns+=clock_ns(NULL)-start;vm_metrics.errors+=e!=0;}
    lck_mtx_unlock(&mali_lock);return e;
}
bool lm_gpu_vm_safe_to_free(const struct lm_vm*v) {if(gpu_is_returning())return false;lck_mtx_lock(&mali_lock);bool ok=!gpu_is_returning()&&lm_core_vm_safe_to_free(v);lck_mtx_unlock(&mali_lock);return ok;}
int lm_gpu_vm_cpu_access(struct lm_vm*v,uint64_t va,uint32_t pages) {
    if(gpu_is_returning())return LM_ENODEV;
    lck_mtx_lock(&mali_lock);int e=gpu_is_returning()?LM_ENODEV:lm_core_vm_cpu_access(v,va,pages);lck_mtx_unlock(&mali_lock);return e;
}
static void snapshot_dvfs_sample(struct lm_job_result *r) {
    if(!r)return;
    r->dvfs_sample.sampled_ns=platform.dvfs.measured_ns;
    r->dvfs_sample.hz=platform.source_hz;r->dvfs_sample.measured_hz=platform.dvfs.measured_hz;
    r->dvfs_sample.vgpu_uv=platform.dvfs.rails.uv[0];r->dvfs_sample.vsram_uv=platform.dvfs.rails.uv[1];
    r->dvfs_sample.target_error=dvfs_target_error;
    r->dvfs_sample.valid=platform.dvfs.attached&&platform.dvfs.meter_valid;
}
static int submit_locked(struct lm_vm*v,uint64_t jc,uint32_t req,uint64_t ns,struct lm_job_result*r,
                          const struct lm_range*ranges,uint32_t count,int32_t owner_pid,uint64_t token) {
    if(gpu_is_returning()) {if(r)bzero(r,sizeof(*r));return LM_ENODEV;}
    uint64_t before=profile_jobs?clock_ns(NULL):0;
    uint64_t cpu_before=profile_jobs==2?thread_ns(NULL):0;
    int cpu_start=profile_jobs==2?cpu_number():-1;
    lck_mtx_lock(&mali_lock);
    bzero(&admission_sample,sizeof(admission_sample));
    uint64_t locked=profile_jobs?clock_ns(NULL):0;
    if(profile_jobs==2) {
        activity.attempts++;activity.pid=owner_pid;activity.started_ns=locked;
    }
    int e;
    if(gpu_is_returning()) {if(r)bzero(r,sizeof(*r));e=LM_ENODEV;}
    else e=ranges?lm_core_submit_ranges(v,jc,req,ns,r,ranges,count):lm_core_submit_wait(v,jc,req,ns,r);
    if(r)r->queue_token=token;
    snapshot_dvfs_sample(r);
    if(r){r->dvfs_after_sample=r->dvfs_sample;r->dvfs_sample=admission_sample.dvfs_sample;}
    /* Keep legacy mode1 output policy. Opt-in mode2 cannot run out during
     * boot: each requirement gets <=1 timestamped pair/s after first192.
     * This policy observes completed jobs only; never changes GPU behavior. */
    uint64_t completed=profile_jobs?clock_ns(NULL):0;
    uint64_t cpu_elapsed=cpu_before?thread_ns(NULL)-cpu_before:0;
    int cpu_end=profile_jobs==2?cpu_number():-1;
    if(profile_jobs==2) {
        activity.done_ns=completed;activity.error=e;activity.last_sequence=r?r->sequence:0;
        if(!e&&r)activity.success_sequence=r->sequence;
        if(r&&r->profile.enabled) {
            activity.run_ns+=r->profile.run_ns;
            activity.maintenance_ns+=r->profile.for_device_ns+r->profile.for_cpu_ns;
            activity.scope_ns+=r->profile.scope_setup_ns+r->profile.scope_restore_ns;
        }
    }
    if(r&&r->profile.enabled&&lm_profile_sample(&profile_policy,profile_jobs,r->sequence,req,completed)) {
        /* Short, independently sequence-keyed records tolerate concurrent
         * bootdiag output. Only complete lines are usable measurements. */
        printf("lancelot-mscope: q=%llu pid=%d s=%u e=%d mode=%u vm=%u p=%u want=%u cp=%u c=%u t=%u dr=%u cr=%u total=%llu lock=%llu setup=%llu restore=%llu\n",
            r->sequence,owner_pid,req,e,r->profile.scope_mode,r->profile.vm_pages,
            r->profile.mapped_pages,r->profile.requested_pages,r->profile.clean_pages,r->profile.cpu_pages,r->profile.table_pages,
            r->profile.device_ranges,r->profile.cpu_ranges,r->elapsed_ns,locked-before,
            r->profile.scope_setup_ns,r->profile.scope_restore_ns);
        printf("lancelot-maccess: q=%llu ro=%u cpu=%u clean=%u\n",r->sequence,
            r->profile.readonly_pages,r->profile.cpu_pages,r->profile.clean_pages);
        printf("lancelot-mcost: q=%llu clean=%llu bind=%llu pre=%llu run=%llu post=%llu detach=%llu inv=%llu\n",
            r->sequence,r->profile.for_device_ns,r->profile.bind_ns,r->profile.preflush_ns,
            r->profile.run_ns,r->profile.postflush_ns,r->profile.detach_ns,r->profile.for_cpu_ns);
        if(profile_jobs==2) {
            printf("lancelot-mpoll: q=%llu hybrid=%u polls=%u spins=%llu sleeps=%llu spin_ns=%llu sleep_ns=%llu gap_ns=%llu busy_ns=%llu seen_ns=%llu clock=mach_absolute_time observed_not_HW\n",
                r->sequence,r->profile.hybrid_poll,r->profile.polls,r->profile.job_wait.spins,r->profile.job_wait.sleeps,
                r->profile.job_wait.spin_ns,r->profile.job_wait.sleep_ns,r->profile.max_poll_gap_ns,r->profile.last_busy_ns,r->profile.done_seen_ns);
            printf("lancelot-cpoll: q=%llu spins=%llu sleeps=%llu spin_ns=%llu sleep_ns=%llu\n",r->sequence,
                r->profile.control_wait.spins,r->profile.control_wait.sleeps,r->profile.control_wait.spin_ns,r->profile.control_wait.sleep_ns);
            printf("lancelot-job-cpu: q=%llu tid=%llu cpu_start=%d cpu_end=%d thread_ns=%llu run_thread_ns=%llu clock=mach_absolute_time runtime=scheduler_not_HW\n",
                r->sequence,thread_tid(current_thread()),cpu_start,cpu_end,cpu_elapsed,r->profile.run_thread_ns);
            printf("lancelot-mclock: q=%llu at=%llu done=%llu\n",r->sequence,before/1000,completed/1000);
            printf("lancelot-jm: q=%llu start_ns=%llu end_ns=%llu launched=%u idle_verified=%u source=launch_to_fresh_idle_wall\n",
                r->sequence,r->jm.start_ns,r->jm.end_ns,r->jm.launched,r->jm.idle_verified);
            printf("lancelot-jobmeta: q=%llu pid=%d req=%u jc=%llx at=%llu done=%llu\n",r->sequence,owner_pid,req,jc,before/1000,completed/1000);
        printf("lancelot-mmask: q=%llu parents=%u entries=%u\n",r->sequence,
                 r->profile.masked_leaf_tables,r->profile.masked_leaf_entries);
            printf("lancelot-mtables: q=%llu total=%u clean=%u dirty_sync=1\n",r->sequence,
                r->profile.table_pages,r->profile.cleaned_table_pages);
        }
    }
    if(e)printf("lancelot-mali: submit=%d seq=%llu status=%#x fault=%#llx reset=%u quarantine=%u stage=%u\n",e,r?r->sequence:0,r?r->job_status:0,r?r->fault_address:0,r?r->reset_completed:0,r?r->quarantined:0,r?r->failed_stage:0);
    if(!mali_driver_ready()) {
        usable=false;
        dvfs_stop_locked();
        /* Snapshot once before a later ENODEV/respawn can obscure the cause.
         * Read only the completed result; do not issue another MMIO/reset. */
        if(lm_first_failure_save(&first_failure,true,e,owner_pid,req,clock_ns(NULL),jc,r))
            report_first_failure_locked(first_failure.at_ns);
    }
    lck_mtx_unlock(&mali_lock);return e;
}
int lm_gpu_submit_wait(struct lm_vm*v,uint64_t jc,uint32_t req,uint64_t ns,struct lm_job_result*r) {
    return submit_locked(v,jc,req,ns,r,NULL,0,proc_selfpid(),0);
}
int lm_gpu_submit_ranges(struct lm_vm*v,uint64_t jc,uint32_t req,uint64_t ns,struct lm_job_result*r,
                         const struct lm_range*ranges,uint32_t count) {
    if(!ranges||!count)return LM_EINVAL;
    return submit_locked(v,jc,req,ns,r,ranges,count,proc_selfpid(),0);
}
int lm_gpu_submit_ranges_owned(struct lm_vm*v,uint64_t jc,uint32_t req,uint64_t ns,struct lm_job_result*r,
                              const struct lm_range*ranges,uint32_t count,int32_t owner_pid,uint64_t token) {
    if(!ranges||!count||owner_pid<=0||!token)return LM_EINVAL;
    return submit_locked(v,jc,req,ns,r,ranges,count,owner_pid,token);
}
/* The synchronous client or queue retains all kernel-owned inputs/references.
 * The originating owner PID is passed explicitly; a queued worker may have a
 * different current PID/TID. No user pointers reach here.
 * Global serialization spans both jobs, including their true completion and
 * original per-stage AS/cache/CPU-handoff sequence. */
int lm_gpu_submit_pair(struct lm_vm *v,uint64_t vt_jc,uint64_t fs_jc,uint64_t ns,
                       struct lm_job_result r[2],const struct lm_range *vt,uint32_t vtc,
                       const struct lm_range *fs,uint32_t fsc,int32_t owner_pid,uint64_t token) {
    if(!r)return LM_EINVAL;
    bzero(r,2*sizeof(*r));
    if(gpu_is_returning())return LM_ENODEV;
    if(!v||!vt||!fs||!vtc||!fsc||owner_pid<=0||!token||!ns||ns>UINT64_C(10000000000))return LM_EINVAL;
    uint64_t before=clock_ns(NULL);
    /* Never block indefinitely behind another client after this transaction's
     * queue budget has been deducted. Short requested sleeps are preemptible,
     * with an iteration bound even if the monotonic clock stops advancing.
     * No core/MMIO access or stage sequence exists until the lock is acquired. */
    bool acquired=false;
    for(uint32_t attempt=0;attempt<200000;attempt++) {
        if(lck_mtx_try_lock(&mali_lock)) {acquired=true;break;}
        uint64_t now=clock_ns(NULL);
        if(now<before)return LM_EIO;
        if(now-before>=ns)return LM_ETIMEDOUT;
        wait_us(NULL,50);
    }
    if(!acquired)return LM_ETIMEDOUT;
    uint64_t locked=clock_ns(NULL);
    bzero(&admission_sample,sizeof(admission_sample));
    int e=LM_ENODEV;
    if(!gpu_is_returning()&&usable&&mali_driver_ready()) {
        /* Caller preparation/queue residence has already been deducted;
         * waiting behind legacy submissions must not restart that budget. */
        if(locked<before)e=LM_EIO;
        else if(locked-before>=ns)e=LM_ETIMEDOUT;
        else e=lm_core_submit_pair(v,vt_jc,fs_jc,ns-(locked-before),r,vt,vtc,fs,fsc);
    }
    uint64_t completed=clock_ns(NULL);
    bool sampled=false;
    for(unsigned s=0;s<2;s++) {
        bool entered=r[s].pair_stage_attempted;
        int stage_error=r[s].pair_stage_error;
        if(profile_jobs==2&&entered) {
            activity.attempts++;activity.pid=owner_pid;activity.started_ns=r[s].started_ns;
            activity.done_ns=r[s].completed_ns;activity.last_sequence=r[s].sequence;activity.error=e;
            if(r[s].sequence&&!stage_error)activity.success_sequence=r[s].sequence;
            if(r[s].profile.enabled) {
                activity.run_ns+=r[s].profile.run_ns;
                activity.maintenance_ns+=r[s].profile.for_device_ns+r[s].profile.for_cpu_ns;
                activity.scope_ns+=r[s].profile.scope_setup_ns+r[s].profile.scope_restore_ns;
            }
        }
        if(!r[s].sequence||!r[s].profile.enabled||
           !lm_profile_sample(&profile_policy,profile_jobs,r[s].sequence,s,r[s].completed_ns))continue;
        sampled=true;
        printf("lancelot-mscope: q=%llu pid=%d s=%u e=%d mode=%u vm=%u p=%u want=%u cp=%u c=%u t=%u dr=%u cr=%u total=%llu lock=%llu setup=%llu restore=%llu\n",
            r[s].sequence,owner_pid,s,stage_error,r[s].profile.scope_mode,r[s].profile.vm_pages,
            r[s].profile.mapped_pages,r[s].profile.requested_pages,r[s].profile.clean_pages,r[s].profile.cpu_pages,r[s].profile.table_pages,
            r[s].profile.device_ranges,r[s].profile.cpu_ranges,r[s].elapsed_ns,s?0:locked-before,
            r[s].profile.scope_setup_ns,r[s].profile.scope_restore_ns);
        printf("lancelot-maccess: q=%llu ro=%u cpu=%u clean=%u\n",r[s].sequence,
            r[s].profile.readonly_pages,r[s].profile.cpu_pages,r[s].profile.clean_pages);
        printf("lancelot-mcost: q=%llu clean=%llu bind=%llu pre=%llu run=%llu post=%llu detach=%llu inv=%llu\n",
            r[s].sequence,r[s].profile.for_device_ns,r[s].profile.bind_ns,r[s].profile.preflush_ns,
            r[s].profile.run_ns,r[s].profile.postflush_ns,r[s].profile.detach_ns,r[s].profile.for_cpu_ns);
        printf("lancelot-mclock: q=%llu at=%llu done=%llu\n",r[s].sequence,r[s].started_ns/1000,r[s].completed_ns/1000);
        printf("lancelot-jm: q=%llu start_ns=%llu end_ns=%llu launched=%u idle_verified=%u source=launch_to_fresh_idle_wall\n",
            r[s].sequence,r[s].jm.start_ns,r[s].jm.end_ns,r[s].jm.launched,r[s].jm.idle_verified);
        printf("lancelot-jobmeta: q=%llu pid=%d req=%u jc=%llx at=%llu done=%llu pair=%llu caller_pid=%d owner_source=retained_client_transaction clock_scope=core_dispatch\n",
            r[s].sequence,owner_pid,s,s?fs_jc:vt_jc,r[s].started_ns/1000,r[s].completed_ns/1000,token,proc_selfpid());
        printf("lancelot-mtables: q=%llu total=%u clean=%u dirty_sync=1\n",r[s].sequence,
            r[s].profile.table_pages,r[s].profile.cleaned_table_pages);
    }
    if(sampled||e)printf("lancelot-pair: token=%llu pid=%d vtq=%llu fsq=%llu at=%llu locked=%llu done=%llu e=%d source=serialized_actual_not_enqueue_completion\n",
        token,owner_pid,r[0].sequence,r[1].sequence,before/1000,locked/1000,completed/1000,e);
    if(!mali_driver_ready()) {
        usable=false;
        dvfs_stop_locked();
        unsigned failed=r[0].pair_stage_attempted&&r[0].pair_stage_error?0u:1u;
        snapshot_dvfs_sample(&r[failed]);
        r[failed].dvfs_after_sample=r[failed].dvfs_sample;r[failed].dvfs_sample=admission_sample.dvfs_sample;
        if(lm_first_failure_save(&first_failure,true,e,owner_pid,failed,completed,failed?fs_jc:vt_jc,&r[failed]))
            report_first_failure_locked(first_failure.at_ns);
    }
    lck_mtx_unlock(&mali_lock);return e;
}

#include "lancelot_mali_queue.inc"

#include "lancelot_hakios_mali.inc"
