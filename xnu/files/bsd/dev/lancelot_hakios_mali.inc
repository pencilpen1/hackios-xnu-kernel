/* SPDX-License-Identifier: APSL-2.0
 * Imported from oosexgang/hakios-mt6768@4f27037146bd147ad15d36329e307a937603cc13.
 * Full GPU/MMU/BO address/submit/worker implementation; lancelot owns power and DVFS.
 */
/*
 * The GPU of the Infinix Hot 20 (X6826B), a Mali-G52 MC2, brought up from the kernel. Only with
 * boot-arg mali=1: every address below belongs to this phone (the other phone's GPU is an Adreno,
 * bsd/dev/redwood_adreno.c).
 *
 * A Bifrost Mali needs no firmware, so bringing it up is power and clocks and nothing else, in the
 * order MediaTek's own kernel uses -- the same sequence the loader's GPU probe proved on the phone
 * (mtkloader/mtkmali.h, where it is written out with its sources):
 *
 *   1  the "mfg_sel" clock mux in topckgen, to the main PLL's third (364 MHz)
 *   2  the four power domains in the SPM, MFG_ASYNC, MFG, MFG_CORE0 and _CORE1, each with its SRAM,
 *      and once MFG has power the bus protection between the GPU block and the rest released
 *   3  the block's own clock gate (BG3D in mfgcfg)
 *   4  the GPU: which one it is, a soft reset, and its L2, tiler and two shader cores powered
 *   5  one job, the smallest there is -- write a value into memory -- through address space 0 in
 *      identity mode (a GPU address is the physical one), and the processor reads the value back
 *   6  the same job again through the GPU's own page tables, at a GPU address of their making
 *
 * The PMIC's two GPU rails, VGPU and VSRAM_GPU, are left on by the preloader; this does not touch
 * them. Afterwards the GPU stays powered, for what is to drive it next. sysctl kern.redwood.mali
 * says how far it got: 6 means every step above worked.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern vm_offset_t phystokv(uint64_t pa);
extern void LancelotHakiosMaliStart(void);
extern uint32_t gHakiosMaliClients, gHakiosMaliBuffers, gHakiosMaliSubmits, gHakiosMaliFailed, gHakiosMaliShown;   /* LancelotHakiosMali.cpp */
extern struct mali_made { uint64_t address, size; uint32_t asked, flags; } gHakiosMaliMade[32];

#define TOPCKGEN_PHYS   0x10000000
#define INFRACFG_PHYS   0x10001000
#define SPM_PHYS        0x10006000
#define MFGCFG_PHYS     0x13000000
#define MALI_PHYS       0x13040000

SYSCTL_NODE(_kern, OID_AUTO, lancelot_hakios, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "Imported MT6768 GPU");

static vm_offset_t mali;
static uint32_t reached, gpu_id, shader_mask;
static bool hkmt_offline;
static uint64_t hkmt_exec_seq,hkmt_failed_seq;
static uint64_t hkmt_reset_count;
static int32_t hkmt_exec_pid;
static struct lm_job_result hkmt_job;
struct hkmt_completed_profile {
    uint64_t sequence,start_ns,end_ns,jm_ns,worker_ns;
    uint32_t hz;int32_t error;
};
static struct hkmt_completed_profile hkmt_profiles[1024];
static uint32_t hkmt_profile_hz;
static int hkmt_dvfs_guard(void);
static bool hkmt_ready(void) {return reached>=6&&!__atomic_load_n(&hkmt_offline,__ATOMIC_ACQUIRE);}
static void hkmt_revoke(void) {__atomic_store_n(&hkmt_offline,true,__ATOMIC_RELEASE);}

SYSCTL_UINT(_kern_lancelot_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 6 is a job run through page tables");
/*
 * What a program said about the GPU (userland/src/mali_test.c writes it): repeated in the log once a
 * minute, so that it lands in the end of a run's log, which is the part that is kept.
 */
static uint32_t user_said;
SYSCTL_UINT(_kern_lancelot_hakios, OID_AUTO, mali_user, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &user_said, 0,
    "what user space found: 1 a job ran, 2 no service, 3 no client, 4 no buffer, 5 no mapping, 6 the job did nothing");

/*
 * What gl_test drew, for the log's end as well: who drew it (1 the Mali, 2 the processor) times 1000
 * plus the share of the picture inside the triangle in percent; and the middle pixel, red in the
 * low byte.
 */
/*
 * kern.redwood.say: a line from a program, into the kernel's log (a job's own output is kept nowhere):
 * sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line)).
 */
#define KEPT_LINES 96
static char kept_lines[KEPT_LINES][200];
static uint32_t kept_next;
static LCK_GRP_DECLARE(kept_lock_group, "redwood-said");
static LCK_MTX_DECLARE(kept_lock, &kept_lock_group);

/* A line from the kernel itself, kept and said again every minute as kern.redwood.say's "!" ones are. */
void lancelot_hakios_keep(const char *format, ...) __printflike(1, 2);
void
lancelot_hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % KEPT_LINES], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

static int
say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 1) {
		length = sizeof(line) - 1;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	line[length] = 0;
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	if (line[0] == '!') {                   /* kept, and said again every minute (mali_thread) */
		lck_mtx_lock(&kept_lock);
		strlcpy(kept_lines[kept_next % KEPT_LINES], line + 1, sizeof(kept_lines[0]));
		kept_next++;
		lck_mtx_unlock(&kept_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern_lancelot_hakios, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    say_line, "A", "");

static uint32_t gl_said, gl_middle;
/*
 * sysctl kern.redwood.bind_cpu=N: the calling thread runs on logical processor N from now on (-1: on
 * any). The scheduler of this kernel does not tell the two Cortex-A75s (logical 1 and 2, cores 7
 * and 6: mtkloader's core_order) from the A55s, and CoreAnimation's render thread spent 14 to 18
 * ms a frame wherever it happened to be (run 147, 26 October: the GPU busy a third of the time).
 * The Metal driver puts that thread and its own worker on the big cores.
 */
/* Keep the donor's caller-controlled affinity helper. Logical CPU numbers
 * come from lancelot's own device tree; no X6826B core ordering is assumed. */
extern unsigned int real_ncpus;
/* BSD compilation hides this OSFMK scheduler declaration in sched_prim.h. */
extern processor_t thread_bind(processor_t processor);
static int
hkmt_bind_cpu(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    int cpu=-1,error=SYSCTL_IN(req,&cpu,sizeof(cpu));
    if(error || req->newptr==USER_ADDR_NULL)return error;
    if(cpu>=(int)real_ncpus)return EINVAL;
    processor_t processor=cpu<0?PROCESSOR_NULL:cpu_to_processor(cpu);
    if(cpu>=0 && processor==PROCESSOR_NULL)return EINVAL;
    (void)thread_bind(processor);
    (void)thread_block(THREAD_CONTINUE_NULL);
    return 0;
}
SYSCTL_PROC(_kern_lancelot_hakios,OID_AUTO,bind_cpu,CTLTYPE_INT|CTLFLAG_WR|CTLFLAG_LOCKED|CTLFLAG_ANYBODY,NULL,0,
    hkmt_bind_cpu,"I","bind the calling thread to a logical processor");
SYSCTL_UINT(_kern_lancelot_hakios, OID_AUTO, mali_gl, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_said, 0, "");
SYSCTL_UINT(_kern_lancelot_hakios, OID_AUTO, mali_gl_middle, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_middle, 0, "");
SYSCTL_UINT(_kern_lancelot_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return base==mali?gpu_read(NULL,at):*(volatile uint32_t *)(base+at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	if(base==mali)gpu_write(NULL,at,value);else *(volatile uint32_t *)(base+at)=value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `ms`. */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t us = 0; us < ms * 1000; us += 10) {
		if ((rd(base, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (rd(base, at) & mask) == want;
}

/* ---- power ---- */

/* SPM, PMIC, clock mux and bus protection are owned by lancelot_mali_platform. */
#define GPU_ID              0x000
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define SHADER_PRESENT      0x100
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define IRQ_RESET_COMPLETED (1u << 8)

#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS1                 0x1880      /* job slot 1, where compute-like jobs go */
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, caches flushed both ends */

#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_STATUS           0x28
#define AS_TRANSCFG_LO      0x30
#define AS_TRANSCFG_HI      0x34

/* ---- one job ---- */

/*
 * The smallest job there is, in a page of the kernel's own: a job header saying "write value", and
 * its payload -- where, what kind of value (a 32-bit immediate), and the value -- the "where"
 * being the same page 0x800 further on. `gpu` is the address the GPU knows the page by: its
 * physical address in identity mode, or wherever the page tables put it. The processor's caches
 * are flushed either side, since the GPU here does not look into them.
 */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page, *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0xc0ffee01u;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);     /* start */
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 100);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = *target == 0xc0ffee01u;
	printf("mali: %s: interrupts %#x, slot status %#x, MMU fault %#x, job status %#x, the word %#x%s\n", what,
	    rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS), rd(mali, AS0 + AS_FAULTSTATUS), job[0], *target,
	    ok ? " -- the GPU ran it" : " -- NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	if(core_mmio_failed || !wait_for(mali,AS0+AS_STATUS,1,0,50)) {
        hkmt_revoke();return;
    }
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	wr(mali, AS0 + AS_TRANSCFG_LO, transcfg);
	wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	wr(mali, AS0 + AS_MEMATTR_LO, 0x88888888u);     /* every attribute: GPU implementation-defined caching */
	wr(mali, AS0 + AS_MEMATTR_HI, 0x88888888u);
	wr(mali, AS0 + AS_COMMAND, 1);                  /* update */
	if (!wait_for(mali, AS0 + AS_STATUS, 1, 0, 50) ||
        (rd(mali,AS0+AS_TRANSTAB_LO)|((uint64_t)rd(mali,AS0+AS_TRANSTAB_HI)<<32))!=transtab ||
        rd(mali,AS0+AS_TRANSCFG_LO)!=transcfg || rd(mali,AS0+AS_TRANSCFG_HI) || core_mmio_failed)
        hkmt_revoke();
}

/* ---- the GPU's own page tables ---- */

/*
 * The format Linux's panfrost gives this GPU: ordinary AArch64 stage-1 tables with 4 KB pages
 * (AS_TRANSCFG mode 6). The MMU says it translates 35 bits of address (MMU_FEATURES 0x2823), which
 * makes three levels: the top table is indexed by bits 34..30, then 29..21, then 20..12. A page's
 * entry is its address, the access flag, inner shareable, readable and writable (AP 01), memory
 * attribute 0, and the low bits 11 ("page").
 */
#define PTE_TABLE           3ull
#define PTE_PAGE            3ull
#define PTE_AF              (1ull << 10)
#define PTE_SH_INNER        (3ull << 8)
#define PTE_AP_RW           (1ull << 6)
#define PTE_ADDRESS         0x0000fffffffff000ull
#define TRANSCFG_WALK_WB    (6u | (20u << 6) | (2u << 24) | (1u << 30))  /* 55 - 35 address bits; walks write-back */
#define TRANSCFG_WALK_NC    (6u | (20u << 6) | (1u << 24))              /* ... walks not cached */
/*
 * Boot-arg mali_fastmap (1 unless =0): the GPU walks its tables without caching them, so an entry
 * the processor has written (and flushed to memory) is what the next walk reads -- and a buffer
 * newly entered needs no command to the MMU at all, an empty entry being one no TLB keeps. Each new
 * buffer used to be an UPDATE of address space 0 and a wait for it: 2.4 ms a buffer, 320 buffers
 * every five seconds in the window server (run 139, 26 October). With mali_fastmap=0, cached
 * walks now use deferred publication before submission and synchronous invalidation on unmap.
 */
static int mali_fastmap = 1;
#define TRANSCFG_AARCH64_4K (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB)

static uint64_t *pt_top;
static uint64_t pt_top_phys;
/* Protected by hkmt_lock once user clients are running. New mappings are
 * published by one invalidation before the next job; unmaps retire immediately. */
static bool hkmt_pt_dirty;

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	FlushPoC_DcacheRegion(page, PAGE_SIZE);  /* its zeroes in memory, where the GPU's walk reads them */
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return (uint64_t *)phystokv(*entry & PTE_ADDRESS);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu` (with the lock held, or before anyone else runs). */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;
	if(!gpu || (gpu&4095) || gpu>=(1ull<<35) || !phys || (phys&4095) || phys>=(1ull<<40))return false;

	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	if ((middle = pt_below(&pt_top[(gpu >> 30) & 31])) == NULL || (bottom = pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & PTE_ADDRESS) | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	hkmt_pt_dirty=true;
	return true;
}

/*
 * Two jobs: first in identity mode, which needs no tables and so says whether the job manager works
 * at all; then the same job at a GPU address the tables give the page, which says whether they do.
 * Afterwards address space 0 stays on the tables, which is how anything that draws will need it.
 */
#define FIRST_JOB_GPU       0x00100000ull

static unsigned
first_jobs(void)
{
	vm_offset_t page = 0;
	unsigned done = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mali: no page for the first jobs\n");
		return 0;
	}
	as0_update(0, 2);                       /* identity: a GPU address is the physical one */
	if (run_write_job(page, kvtophys(page), "first job, identity mode")) {
		done = 1;
		if (map_page(FIRST_JOB_GPU, kvtophys(page))) {
			as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
			if (run_write_job(page, FIRST_JOB_GPU, "second job, through the GPU's page tables")) {
				done = 2;
			}
		} else {
			printf("mali: no memory for page tables\n");
		}
	}
	kmem_free(kernel_map, page, PAGE_SIZE);
	return done;
}

static void
mali_up(void)
{
	uint32_t shaders, tiler, l2;

	mali = banks[LMP_BANK_GPU];
	if (!mali) return;
	reached = 3; /* lancelot native platform already verified power and clocks */

	/* 4: the GPU itself */
	gpu_id = rd(mali, GPU_ID);
	if ((gpu_id>>16)!=0x7402 || rd(mali,0x14)!=0x2823) {
        printf("lancelot-hakios: unexpected GPU/MMU identity; no jobs\n");return;
    }
	shaders = shader_mask = rd(mali, SHADER_PRESENT);
	tiler = rd(mali, TILER_PRESENT);
	l2 = rd(mali, L2_PRESENT);
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, GPU_CMD, 1);                   /* soft reset */
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, 100)) {
		printf("mali: GPU_ID %#x, but it did not finish its reset\n", gpu_id);
		return;
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, L2_PWRON, l2);
	wr(mali, TILER_PWRON, tiler);
	wr(mali, SHADER_PWRON, shaders);
	if (!wait_for(mali, L2_READY, l2, l2, 50) || !wait_for(mali, TILER_READY, tiler, tiler, 50) ||
	    !wait_for(mali, SHADER_READY, shaders, shaders, 50)) {
		printf("mali: GPU_ID %#x, but its cores did not all power up (shaders ready %#x)\n", gpu_id,
		    rd(mali, SHADER_READY));
		return;
	}
	reached = 4;
	printf("mali: GPU_ID %#x (Mali-G52 r%up%u), shader cores %#x, tiler %#x, L2 %#x: all powered\n", gpu_id,
	    (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, shaders, tiler, l2);

	/* 5: something for it to do */
	switch (first_jobs()) {
	case 2:
		reached = 6;
		break;
	case 1:
		reached = 5;
		break;
	default:
		break;
	}
}

/* ---- what the service that programs talk to uses (iokit/Kernel/LancelotHakiosMali.cpp) ---- */

/*
 * Linux's panfrost interface, as far as Mesa's panfrost needs it to draw, is answered by the IOKit
 * service LancelotHakiosMali; this is its half in the hardware: parameters, GPU addresses, pages entered
 * into and taken out of the tables, and jobs. One address space (0) for everyone, and one job at a
 * time, waited for before the call returns -- simple, and enough to see Mesa draw; overlapping
 * jobs and per-client address spaces come after.
 */
static LCK_GRP_DECLARE(hkmt_lock_group, "redwood-mali");
static LCK_MTX_DECLARE(hkmt_lock, &hkmt_lock_group);
/*
 * Held while a job runs, and by what must not change the address space under one (an UPDATE when
 * entries are taken out); taken before hkmt_lock, never after. hkmt_lock itself is no longer held
 * for the length of a job: making a buffer had to wait for the job running to finish (2.5 ms a
 * buffer, 26 October).
 */
static LCK_MTX_DECLARE(mali_job_lock, &hkmt_lock_group);

/*
 * Until a job is done, without keeping a core busy: a moment of polling for the short ones, then
 * asleep 200 us at a time. The wait used to spin (delay(10) in a loop) for as long as the GPU
 * worked -- a whole core of the four lost to every frame's rendering.
 */
static int mali_wait_event;
static LCK_MTX_DECLARE(mali_wait_lock, &hkmt_lock_group);

/* Bound completion by the last not-complete read and first complete read.
 * These are software observations, not GPU execution timestamps. In particular,
 * a delayed timer wakeup must not be mistaken for time the GPU was still busy.
 */
struct hkmt_wait_observation {
    uint64_t last_poll_ns, last_pending_ns, first_complete_ns, max_poll_gap_ns;
    uint64_t sleep_ns, max_sleep_ns;
    uint32_t polls, sleeps;
};

static bool
hkmt_poll_job(uint32_t at, uint32_t mask, uint32_t want,
    struct hkmt_wait_observation *observation)
{
    uint64_t before=clock_ns(NULL);
    uint32_t value=rd(mali,at);
    if(observation->polls && before>=observation->last_poll_ns) {
        uint64_t gap=before-observation->last_poll_ns;
        if(gap>observation->max_poll_gap_ns)observation->max_poll_gap_ns=gap;
    }
    observation->last_poll_ns=before;observation->polls++;
    if((value&mask)==want) {
        observation->first_complete_ns=clock_ns(NULL);
        return true;
    }
    observation->last_pending_ns=before;
    return false;
}

static bool
wait_job(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms,
    struct hkmt_wait_observation *observation)
{
	uint64_t deadline;
	bzero(observation,sizeof(*observation));

	for (int i = 0; i < 20; i++) {
		if (hkmt_poll_job(at,mask,want,observation)) {
			return true;
		}
		delay(10);
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (hkmt_poll_job(at,mask,want,observation)) {
			return true;
		}
		uint64_t nap;

		clock_interval_to_deadline(200, NSEC_PER_USEC, &nap);
		uint64_t before_sleep=clock_ns(NULL);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
		uint64_t slept=clock_ns(NULL)-before_sleep;
		observation->sleeps++;observation->sleep_ns+=slept;
		if(slept>observation->max_sleep_ns)observation->max_sleep_ns=slept;
	}
	return hkmt_poll_job(at,mask,want,observation);
}

#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND_LOCK     2
#define AS_COMMAND_FLUSH_PT 4
#define AS_COMMAND_FLUSH_MEM 5

/* MMU register encodings/ordering checked against Arm r20p0 and Linux v6.12:
 * LOCKADDR encodes log2(region bytes)-1; FLUSH_MEM drains memory, flushes GPU
 * caches and invalidates translations. UPDATE alone only broadcasts AS config.
 * Own implementation; callers hold mali_lock -> mali_job_lock -> hkmt_lock.
 */
static uint64_t hkmt_pt_flushes,hkmt_pt_flush_ns,hkmt_pt_flush_errors;

static bool
hkmt_as_command(uint32_t command)
{
    if(core_mmio_failed || !wait_for(mali,AS0+AS_STATUS,1,0,50))return false;
    wr(mali,AS0+AS_COMMAND,command);
    return !core_mmio_failed && wait_for(mali,AS0+AS_STATUS,1,0,50) && !core_mmio_failed;
}

static bool
hkmt_flush_page_tables(void)
{
    uint64_t began=clock_ns(NULL);unsigned stage=1;
    if(hkmt_dvfs_guard()!=LM_OK)goto failed;
    /* AS0 exposes [0,2^35), including the startup probe below normal BO VAs. */
    wr(mali,AS0+AS_LOCKADDR_LO,34u);
    wr(mali,AS0+AS_LOCKADDR_HI,0);
    stage=2;
    if(core_mmio_failed || rd(mali,AS0+AS_LOCKADDR_LO)!=34u ||
       rd(mali,AS0+AS_LOCKADDR_HI)!=0)goto failed;
    stage=3;if(!hkmt_as_command(AS_COMMAND_LOCK))goto failed;
    stage=4;if(!hkmt_as_command(AS_COMMAND_FLUSH_MEM))goto failed;
    stage=5;if(hkmt_dvfs_guard()!=LM_OK)goto failed;
    hkmt_pt_dirty=false;hkmt_pt_flushes++;
    hkmt_pt_flush_ns+=clock_ns(NULL)-began;
    if(hkmt_pt_flushes<=4 || !(hkmt_pt_flushes&127u))
        printf("lancelot-hakios-ptw-flush: at=%llu count=%llu last_us=%llu total_us=%llu errors=%llu scope=AS0_VA35\n",
            clock_ns(NULL)/1000,hkmt_pt_flushes,(clock_ns(NULL)-began)/1000,
            hkmt_pt_flush_ns/1000,hkmt_pt_flush_errors);
    return true;
failed:
    hkmt_pt_flush_errors++;hkmt_revoke();
    printf("lancelot-hakios-ptw-fault: at=%llu stage=%u errors=%llu dirty=%u action=revoke\n",
        clock_ns(NULL)/1000,stage,hkmt_pt_flush_errors,hkmt_pt_dirty);
    return false;
}

#define JS0                 0x1800      /* fragment jobs; everything else goes to slot 1 */
#define GPU_ADDRESS_FIRST   0x01000000ull
#define GPU_ADDRESS_END     (1ull << 35)

static uint64_t next_address = GPU_ADDRESS_FIRST;

/*
 * The last buffers handed GPU addresses, and whether they have been taken out of the tables
 * since: addresses are never given twice, so a translation fault is a job writing into a
 * buffer already freed or past the end of one, and this says which. With hkmt_lock held.
 */
#define MALI_RECENT 4096
static struct {
	uint64_t address, size;
	uint32_t freed, kind;                   /* kind: 0 not said, 1 a panfrost BO, 2 an IOSurface */
	uint32_t width, height, row_bytes, format, offset;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* What a buffer just made is, for describe_address. */
void
lancelot_hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&hkmt_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0) {
			mali_recent[i].kind = kind;
			mali_recent[i].width = width;
			mali_recent[i].height = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format = format;
			mali_recent[i].offset = offset;
		}
	}
	lck_mtx_unlock(&hkmt_lock);
}

static void
say_kind(int i)
{
	if (mali_recent[i].kind == 2) {
		printf("mali:     an IOSurface %ux%u, %u bytes a row, format %#x, from byte %u\n", mali_recent[i].width,
		    mali_recent[i].height, mali_recent[i].row_bytes, mali_recent[i].format, mali_recent[i].offset);
	} else {
		printf("mali:     %s\n", mali_recent[i].kind == 1 ? "a panfrost BO" : "made by nothing that said");
	}
}

static void
describe_address(uint64_t address)
{
	int below = -1;

	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].size == 0) {
			continue;
		}
		if (address >= mali_recent[i].address && address < mali_recent[i].address + mali_recent[i].size) {
			printf("mali:   the fault is %#llx into a buffer of %#llx bytes at %#llx, %s\n",
			    address - mali_recent[i].address, mali_recent[i].size, mali_recent[i].address,
			    mali_recent[i].freed ? "FREED already" : "still there (a page not entered?)");
			say_kind(i);
			return;
		}
		if (mali_recent[i].address <= address && (below < 0 || mali_recent[i].address > mali_recent[below].address)) {
			below = i;
		}
	}
	if (below >= 0) {
		printf("mali:   the fault is %#llx past the end of a buffer of %#llx bytes at %#llx (%s)\n",
		    address - (mali_recent[below].address + mali_recent[below].size), mali_recent[below].size,
		    mali_recent[below].address, mali_recent[below].freed ? "freed" : "live");
		say_kind(below);
	} else {
		printf("mali:   the fault is in no buffer of the last %u\n", MALI_RECENT);
	}
}

/* The last job that did not finish, to be repeated where the end of the log keeps it. */
static struct {
	uint32_t count, slot, raw, status, mmu, fault, fault_address;
	uint64_t chain, transtab, walk[3];      /* the tables the GPU had, and their entries for the fault's address */
} last_failure;

bool
lancelot_hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool done;

	lck_mtx_lock(&hkmt_lock);
	done = map_page(gpu, phys);
	lck_mtx_unlock(&hkmt_lock);
	return done;
}

void lancelot_hakios_mali_mapped(void);

bool
lancelot_hakios_mali_ready(void)
{
	return hkmt_ready();
}

/* Linux's DRM_PANFROST_PARAM_*, in its order, from the registers they come from. */
bool
lancelot_hakios_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT (none on Bifrost: reads 0) */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!lancelot_hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2:
	case 3:
	case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                /* NR_CORE_GROUPS */
		*value = 1;
		return true;
	case 39:
		/*
		 * THREAD_TLS_ALLOC, which Bifrost has (0x310) and the G52 here needs: answered 0, panfrost
		 * sized the stack of a spilling shader by THREAD_MAX_THREADS instead, and the GPU, which
		 * gives each core more stacks than that, wrote past the end of it (25 October: every write
		 * fault of the uber shaders, up to 0x15870 past a 0x60000 buffer). Never less than that
		 * fallback, whatever the register says.
		 */
		*value = rd(mali, 0x310);
		if (*value < rd(mali, 0x0a0)) {
			*value = rd(mali, 0x0a0);
		}
		{
			static int said;
			if (!said++) {
				printf("mali: THREAD_TLS_ALLOC %#x, THREAD_MAX_THREADS %#x, SHADER_PRESENT %#x\n", rd(mali, 0x310),
				    rd(mali, 0x0a0), rd(mali, 0x100));
			}
		}
		return true;
	case 40:                                /* AFBC_FEATURES: not on this GPU */
		*value = 0;
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

/* `size` bytes of GPU addresses, never given out twice: there are 32 GB of them. 0: none left. */
uint64_t
lancelot_hakios_mali_address(uint64_t size)
{
	uint64_t address;

	lck_mtx_lock(&hkmt_lock);
	address = next_address;
	if (!size || (size&4095) || address>=GPU_ADDRESS_END || size>GPU_ADDRESS_END-address) {
		address = 0;
	} else {
		next_address += (size + 0xffff) & ~0xffffull;   /* 64 KB between buffers: an overrun faults */
		mali_recent[mali_recent_next % MALI_RECENT].address = address;
		mali_recent[mali_recent_next % MALI_RECENT].size = size;
		mali_recent[mali_recent_next % MALI_RECENT].freed = 0;
		mali_recent[mali_recent_next % MALI_RECENT].kind = 0;
		mali_recent_next++;
	}
	lck_mtx_unlock(&hkmt_lock);
	return address;
}

void
lancelot_hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&hkmt_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed = 1;
		}
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096) {        /* the GPU's pages, not the kernel's 16 KB */
		uint64_t *entry = &pt_top[(at >> 30) & 31];

		if (!(*entry & 1)) {
			continue;
		}
		entry = &((uint64_t *)phystokv(*entry & PTE_ADDRESS))[(at >> 21) & 511];
		if (!(*entry & 1)) {
			continue;
		}
		entry = &((uint64_t *)phystokv(*entry & PTE_ADDRESS))[(at >> 12) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
		hkmt_pt_dirty=true;
	}
	/* Storage is released by the client only after this succeeds and readiness
     * remains true. A failed invalidation keeps existing BOs quarantined. */
    if(!mali_fastmap) {
        if(hkmt_pt_dirty)(void)hkmt_flush_page_tables();
    } else {
        as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
    }
	lck_mtx_unlock(&hkmt_lock);
	lck_mtx_unlock(&mali_job_lock);
	lck_mtx_unlock(&mali_lock);
}

/*
 * map_page has already cleaned CPU page-table writes and marked them dirty.
 * With cached walks, publish all such writes by one serialized AS0 invalidation
 * before the next job, so a batch of allocations does not flush for every BO.
 * The uncached walk mode retains the donor's new-map path.
 */
void
lancelot_hakios_mali_mapped(void)
{
    /* Publication is enforced at the job launch boundary below. */
}

/* The entries of each level for GPU address `gpu`, as far as there are tables (with the lock held). */
static void
walk_tables(uint64_t gpu, uint64_t walk[3])
{
	uint64_t *table = pt_top;
	static const unsigned shift[3] = { 30, 21, 12 };

	walk[0] = walk[1] = walk[2] = 0;
	for (int level = 0; level < 3 && table != NULL; level++) {
		walk[level] = table[(gpu >> shift[level]) & (level == 0 ? 31 : 511)];
		table = (walk[level] & 1) && level < 2 ? (uint64_t *)phystokv(walk[level] & PTE_ADDRESS) : NULL;
	}
}

/* The donor resumes after a GPU reset. Restore its L2/tiler/shader/AS0 state,
 * and verify each step before accepting another job or releasing DMA backing. */
static bool
hkmt_recover_gpu(void)
{
    __atomic_add_fetch(&hkmt_reset_count,1u,__ATOMIC_RELAXED);
    wr(mali,GPU_INT_CLEAR,0xffffffffu);wr(mali,GPU_CMD,1);
    hkmt_job.reset_completed=wait_for(mali,GPU_INT_RAWSTAT,IRQ_RESET_COMPLETED,IRQ_RESET_COMPLETED,100);
    if(!hkmt_job.reset_completed || core_mmio_failed)return false;
    uint32_t l2=rd(mali,L2_PRESENT),tiler=rd(mali,TILER_PRESENT);
    wr(mali,L2_PWRON,l2);wr(mali,TILER_PWRON,tiler);wr(mali,SHADER_PWRON,shader_mask);
    if(!wait_for(mali,L2_READY,l2,l2,50) || !wait_for(mali,TILER_READY,tiler,tiler,50) ||
       !wait_for(mali,SHADER_READY,shader_mask,shader_mask,50) || core_mmio_failed)return false;
    as0_update(pt_top_phys,TRANSCFG_AARCH64_4K);
    wr(mali,MMU_INT_CLEAR,0xffffffffu);wr(mali,JOB_INT_CLEAR,0xffffffffu);
    wr(mali,GPU_INT_CLEAR,0xffffffffu);
    return hkmt_dvfs_guard()==LM_OK;
}

uint64_t lancelot_hakios_mali_resets(void);
uint64_t lancelot_hakios_mali_resets(void) {return __atomic_load_n(&hkmt_reset_count,__ATOMIC_RELAXED);}

/*
 * One job chain, from GPU address `chain`, run to its end before this returns: fragment jobs
 * (`fragment`) on slot 0, the rest on slot 1. 0 when it finished; otherwise the slot's status, a
 * fault or 0xff for no answer within a second -- and then the GPU is reset, so the next one finds
 * it working.
 */
uint32_t
lancelot_hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1, js = fragment ? JS0 : JS1, raw, status = 0;
	bool finished;
	struct hkmt_wait_observation wait_observation;
    struct lancelot_cpu_profile cpu_before,cpu_after;
    uint64_t worker_cpu_began=thread_ns(NULL);
    cpu_profile_copy(&cpu_before);

	uint64_t entered=clock_ns(NULL);
    hkmt_profile_hz=0;
	lck_mtx_lock(&mali_lock);
    bzero(&hkmt_job,sizeof(hkmt_job));hkmt_job.sequence=hkmt_exec_seq;hkmt_job.queue_token=hkmt_exec_seq;
    hkmt_job.quarantined=true; /* no stale completion proof on an admission error */
    if(gpu_is_returning() || !usable || !hkmt_ready() || dvfs_before_job(NULL)) {
        hkmt_revoke();lck_mtx_unlock(&mali_lock);return 0xff;
    }
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&hkmt_lock);
    if(!mali_fastmap && hkmt_pt_dirty && !hkmt_flush_page_tables()) {
        lck_mtx_unlock(&hkmt_lock);lck_mtx_unlock(&mali_job_lock);lck_mtx_unlock(&mali_lock);
        return 0xff;
    }
    hkmt_job.quarantined=false;
    hkmt_job.started_ns=clock_ns(NULL);activity.attempts++;activity.pid=hkmt_exec_pid;activity.started_ns=hkmt_job.started_ns;
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	hkmt_job.jm.start_ns=clock_ns(NULL);hkmt_job.jm.launched=true;
    hkmt_profile_hz=platform.source_hz;
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&hkmt_lock);
	finished = wait_job(JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000,&wait_observation);
	lck_mtx_lock(&hkmt_lock);
    wr(mali,JOB_INT_CLEAR,0);
    finished=finished && !(rd(mali,0x1010)&0x70007u) && !rd(mali,MMU_INT_RAWSTAT) && !(rd(mali,GPU_INT_RAWSTAT)&0x81u) && !core_mmio_failed;
    hkmt_job.jm.end_ns=clock_ns(NULL);hkmt_job.jm.idle_verified=finished;
	if (!finished) {
		raw = rd(mali, JOB_INT_RAWSTAT);
		status = (raw & (1u << (slot + 16))) ? rd(mali, js + JS_STATUS) : 0xff;
		if(!status)status=0xff;
		last_failure.count++;
		last_failure.slot = slot;
		last_failure.raw = raw;
		last_failure.status = rd(mali, js + JS_STATUS);
		last_failure.mmu = rd(mali, MMU_INT_RAWSTAT);
		last_failure.fault = rd(mali, AS0 + AS_FAULTSTATUS);
		last_failure.fault_address = rd(mali, AS0 + 0x20);
		last_failure.chain = chain;
		last_failure.transtab = rd(mali, AS0 + AS_TRANSTAB_LO) | ((uint64_t)rd(mali, AS0 + AS_TRANSTAB_HI) << 32);
		walk_tables(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32), last_failure.walk);
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n", slot, raw,
		    last_failure.status, last_failure.mmu, last_failure.fault, last_failure.fault_address);
		describe_address(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32));
        hkmt_job.failed_stage=LM_JOB_WAIT;hkmt_job.job_status=last_failure.status;
        hkmt_job.mmu_fault=last_failure.mmu;hkmt_job.gpu_fault=rd(mali,GPU_INT_RAWSTAT);
        hkmt_job.js_head_lo=rd(mali,js);hkmt_job.js_head_hi=rd(mali,js+4);
        hkmt_job.js_tail_lo=rd(mali,js+8);hkmt_job.js_tail_hi=rd(mali,js+12);
        hkmt_job.dvfs_sample=admission_sample.dvfs_sample;
        printf("lancelot-hakios-fault: seq=%llu pid=%d slot=%u jc=%#llx head=%08x%08x tail=%08x%08x mmu=%#x as_fault=%#x address=%08x%08x at=%llu\n",
          hkmt_exec_seq,hkmt_exec_pid,slot,chain,hkmt_job.js_head_hi,hkmt_job.js_head_lo,hkmt_job.js_tail_hi,hkmt_job.js_tail_lo,last_failure.mmu,last_failure.fault,rd(mali,AS0+0x24),last_failure.fault_address,clock_ns(NULL)/1000);
        hkmt_job.quarantined=!hkmt_recover_gpu();
        if(hkmt_job.quarantined)hkmt_revoke();

	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	lck_mtx_unlock(&hkmt_lock);
	lck_mtx_unlock(&mali_job_lock);
    hkmt_job.completed_ns=clock_ns(NULL);hkmt_job.elapsed_ns=hkmt_job.completed_ns-hkmt_job.started_ns;
    if(hkmt_job.jm.end_ns-hkmt_job.jm.start_ns>=UINT64_C(10000000)) {
        cpu_profile_copy(&cpu_after);
        uint64_t worker_cpu_ended=thread_ns(NULL);
        printf("lancelot-hakios-cpu: seq=%llu at=%llu sample_ns=%llu age_us=%llu valid_mask=%#x hz_valid=%#x ll_hz=%llu big_hz=%llu c0=%u c1=%u c2=%u c3=%u c4=%u c5=%u c6=%u c7=%u worker_cpu_us=%llu cpu_scope=scheduler_window hz_scope=PLL_mux_derived\n",
            hkmt_exec_seq,hkmt_job.completed_ns/1000,cpu_before.sampled_ns,
            cpu_before.sampled_ns&&hkmt_job.started_ns>=cpu_before.sampled_ns?(hkmt_job.started_ns-cpu_before.sampled_ns)/1000:UINT64_MAX,
            cpu_before.valid_mask,cpu_before.hz_valid_mask,cpu_before.hz[0],cpu_before.hz[1],
            cpu_before.busy_bps[0],cpu_before.busy_bps[1],cpu_before.busy_bps[2],cpu_before.busy_bps[3],
            cpu_before.busy_bps[4],cpu_before.busy_bps[5],cpu_before.busy_bps[6],cpu_before.busy_bps[7],
            worker_cpu_ended>=worker_cpu_began?(worker_cpu_ended-worker_cpu_began)/1000:0);
        printf("lancelot-hakios-cpu-after: seq=%llu at=%llu sample_ns=%llu valid_mask=%#x hz_valid=%#x ll_hz=%llu big_hz=%llu c0=%u c1=%u c2=%u c3=%u c4=%u c5=%u c6=%u c7=%u\n",
            hkmt_exec_seq,hkmt_job.completed_ns/1000,cpu_after.sampled_ns,cpu_after.valid_mask,cpu_after.hz_valid_mask,
            cpu_after.hz[0],cpu_after.hz[1],cpu_after.busy_bps[0],cpu_after.busy_bps[1],cpu_after.busy_bps[2],cpu_after.busy_bps[3],
            cpu_after.busy_bps[4],cpu_after.busy_bps[5],cpu_after.busy_bps[6],cpu_after.busy_bps[7]);
        printf("lancelot-hakios-heavy-job: seq=%llu pid=%d slot=%u jc=%#llx at=%llu gate_us=%llu jm_us=%llu wall_us=%llu hz=%u measured_at_ns=%llu scope=JM_wall\n",
            hkmt_exec_seq,hkmt_exec_pid,slot,chain,hkmt_job.started_ns/1000,
            (hkmt_job.started_ns-entered)/1000,(hkmt_job.jm.end_ns-hkmt_job.jm.start_ns)/1000,
            hkmt_job.elapsed_ns/1000,platform.source_hz,admission_sample.dvfs_sample.sampled_ns);
        uint64_t lower=wait_observation.last_pending_ns>hkmt_job.jm.start_ns?
            wait_observation.last_pending_ns-hkmt_job.jm.start_ns:0;
        uint64_t gap=wait_observation.first_complete_ns>=wait_observation.last_pending_ns && wait_observation.last_pending_ns?
            wait_observation.first_complete_ns-wait_observation.last_pending_ns:0;
        printf("lancelot-hakios-poll: seq=%llu polls=%u sleeps=%u pending_us=%llu complete_gap_us=%llu max_poll_gap_us=%llu sleep_us=%llu max_sleep_us=%llu complete=%u scope=SW_observation\n",
            hkmt_exec_seq,wait_observation.polls,wait_observation.sleeps,lower/1000,gap/1000,
            wait_observation.max_poll_gap_ns/1000,wait_observation.sleep_ns/1000,
            wait_observation.max_sleep_ns/1000,wait_observation.first_complete_ns!=0&&!status);
    }
    activity.done_ns=hkmt_job.completed_ns;activity.last_sequence=hkmt_exec_seq;activity.error=status?LM_EIO:0;
    activity.run_ns+=hkmt_job.jm.end_ns-hkmt_job.jm.start_ns;
    if(!status)activity.success_sequence=hkmt_exec_seq;
    if(status && hkmt_job.reset_completed && !hkmt_job.quarantined) {
        /* Reset breaks the utilization interval, not the lancelot AP grant.
         * Re-measure the existing OPP/rails through the native platform. */
        lgd_sampler_break(&dvfs_load,clock_ns(NULL));
        if(platform.dvfs.attached && dvfs_service_locked(false)) {
            hkmt_job.quarantined=true;hkmt_revoke();
        }
        printf("lancelot-hakios-recovery: seq=%llu resets=%llu restored=%u dvfs_attached=%u result=failed_job\n",
            hkmt_exec_seq,hkmt_reset_count,!hkmt_job.quarantined,platform.dvfs.attached);
    } else dvfs_job_done(NULL,&hkmt_job,status?LM_EIO:LM_OK);
    if(status) {
        snapshot_dvfs_sample(&hkmt_job);hkmt_job.dvfs_after_sample=hkmt_job.dvfs_sample;hkmt_job.dvfs_sample=admission_sample.dvfs_sample;
        if(lm_first_failure_save(&first_failure,true,LM_EIO,hkmt_exec_pid,fragment?1:0,hkmt_job.completed_ns,chain,&hkmt_job))report_first_failure_locked(hkmt_job.completed_ns);
    }
    lck_mtx_unlock(&mali_lock);
	return status;
}

/*
 * The jobs in a queue, run by a thread of their own: a submission returns at once with the job's
 * number, and whoever needs a job done waits for that number (lancelot_hakios_mali_wait). Run one by one
 * in the order given, which is the order panfrost needs -- a pass's vertex and tiler chain before
 * its fragment chain, one pass before the next that reads it. Before this every submission ran its
 * job to the end before returning: a frame of CoreAnimation's with its blur passes had the window
 * server waiting for the GPU at every one of them, the processor and the GPU taking turns (24
 * October, 11 to 17 ms a frame of the processor's).
 */
#define MALI_QUEUE      64
typedef int (*hkmt_completion_t)(void *,uint64_t,int,bool *);
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
	int32_t owner_pid;
    hkmt_completion_t completion;
    void *completion_context;
} mali_queue[MALI_QUEUE];
/* boot-arg mali_trace=1: a line for every job, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in us of
 * mach_absolute_time, beside touch_lat's lines of the display's and the Metal driver's */
static uint32_t mali_trace;
#include "lancelot_mali_trace.h"
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &hkmt_lock_group);
struct hkmt_completion_error {
    struct hkmt_completion_error *next;
    uint64_t sequence;
    int result;
    bool dma_safe;
};
/* Rare failures are retained for this boot, so a later BO wait cannot turn an
 * acknowledged failed job into success. No allocation occurs on success. */
static struct hkmt_completion_error *hkmt_completion_errors;

/* Admission may already hold mali_lock. Never wait for the producer lock:
 * unavailable queue feedback is not an empty or measured idle interval. */
static bool hakios_queue_depth(uint32_t *depth)
{
    if(!depth || !lck_mtx_try_lock(&mali_queue_lock))return false;
    *depth=mali_queue_count;lck_mtx_unlock(&mali_queue_lock);return true;
}

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;
        struct hkmt_completed_profile profile={0};
        uint64_t began_profile;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		uint64_t queued = mali_queue[mali_queue_head].queued;
		fragment = mali_queue[mali_queue_head].fragment;
		hkmt_completion_t completion=mali_queue[mali_queue_head].completion;
		void *completion_context=mali_queue[mali_queue_head].completion_context;
		hkmt_exec_seq=seq;hkmt_exec_pid=mali_queue[mali_queue_head].owner_pid;
		lck_mtx_unlock(&mali_queue_lock);
		began_profile=mach_absolute_time();
		{
			/* how busy the GPU is: every five seconds, its share of the time and by which slot */
			static uint64_t busy[2], since, jobs[2], wait_total, wait_max, run_max;
			static struct lm_trace_gate trace_gate;
			uint64_t began = mach_absolute_time(), now, ns;

			int result=lancelot_hakios_mali_submit(chain, fragment)?LM_EIO:LM_OK;
            uint64_t callback_began=mach_absolute_time();
            uint64_t native_jm_ns=hkmt_job.jm.end_ns>=hkmt_job.jm.start_ns?
                hkmt_job.jm.end_ns-hkmt_job.jm.start_ns:0;
            uint64_t native_start_ns=hkmt_job.started_ns;
            uint64_t native_end_ns=hkmt_job.completed_ns;
            uint32_t native_hz=hkmt_profile_hz;
            bool dma_safe=!result || (hkmt_job.reset_completed&&!hkmt_job.quarantined);
            /* The callback uses only its own retained state/lease/fence locks.
             * In particular it must not acquire the client lock: an enqueue
             * or GEM_CLOSE may hold that lock while waiting for queue space. */
            if(completion)result=completion(completion_context,seq,result,&dma_safe);
            uint64_t callback_ended=mach_absolute_time();
            if(!dma_safe && !result)result=LM_EIO;
            profile=(struct hkmt_completed_profile){seq,native_start_ns,native_end_ns,native_jm_ns,0,native_hz,result};
			if (result != LM_OK) {
				gHakiosMaliFailed++;
                struct hkmt_completion_error *error=kalloc_data(sizeof(*error),Z_NOWAIT|Z_ZERO);
                if(error){error->sequence=seq;error->result=result;error->dma_safe=dma_safe;}
                if(!dma_safe || !error)hkmt_revoke();
                lck_mtx_lock(&mali_queue_lock);
                if(error){error->next=hkmt_completion_errors;hkmt_completion_errors=error;}
                if((!dma_safe || !error) && !hkmt_failed_seq)hkmt_failed_seq=seq;
                lck_mtx_unlock(&mali_queue_lock);
			}
			now = mach_absolute_time();
			uint64_t q, b, e;
			absolutetime_to_nanoseconds(queued, &q);
			absolutetime_to_nanoseconds(began, &b);
			absolutetime_to_nanoseconds(now, &e);
			if (lm_trace_job(&trace_gate,mali_trace,q,b,e,result!=LM_OK)) {
				printf("tl: j %llu %c q %llu b %llu e %llu\n", seq, fragment ? 'f' : 'v', q / 1000, b / 1000, e / 1000);
			}
			if(b>=q) {
				uint64_t wait=b-q;
				wait_total=wait>UINT64_MAX-wait_total?UINT64_MAX:wait_total+wait;
				if(wait>wait_max)wait_max=wait;
			}
			if(e>=b&&e-b>run_max)run_max=e-b;
            uint64_t callback_ns;
            absolutetime_to_nanoseconds(callback_ended-callback_began,&callback_ns);
            if(callback_ns>=UINT64_C(100000000))
                printf("lancelot-hakios-callback: seq=%llu pid=%d at=%llu callback_us=%llu jm_us=%llu error=%d\n",
                    seq,hkmt_exec_pid,e/1000,callback_ns/1000,native_jm_ns/1000,result);
			absolutetime_to_nanoseconds(now - began, &ns);
			busy[fragment] += ns;
			jobs[fragment]++;
			if (since == 0) {
				since = now;
			} else {
				absolutetime_to_nanoseconds(now - since, &ns);
				if (ns >= 5ull * NSEC_PER_SEC) {
					printf("mali: GPU busy %llu%% of %llu s: vertex and tiler %llu jobs %llu ms (%llu us each), "
					    "fragment %llu jobs %llu ms (%llu us each)\n", (busy[0] + busy[1]) * 100 / ns,
					    ns / NSEC_PER_SEC, jobs[0], busy[0] / 1000000, jobs[0] ? busy[0] / 1000 / jobs[0] : 0,
					    jobs[1], busy[1] / 1000000, jobs[1] ? busy[1] / 1000 / jobs[1] : 0);
					printf("lancelot-hakios-latency: at=%llu jobs=%llu queue_mean_us=%llu queue_max_us=%llu worker_max_us=%llu trace_mode=%u scope=FIFO_wall\n",
					    e/1000,jobs[0]+jobs[1],wait_total/(jobs[0]+jobs[1])/1000,
					    wait_max/1000,run_max/1000,mali_trace);
					wait_total=wait_max=run_max=0;
					busy[0] = busy[1] = jobs[0] = jobs[1] = 0;
					since = now;
				}
			}
		}

		lck_mtx_lock(&mali_queue_lock);
		absolutetime_to_nanoseconds(mach_absolute_time()-began_profile,&profile.worker_ns);
        hkmt_profiles[seq%1024]=profile;
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);       /* (room in the queue) */
	}
}

/* A job chain into the queue: its number. Completion state is retained by the
 * caller before enqueue and retired before mali_done_seq becomes visible. */
uint64_t lancelot_hakios_mali_enqueue(uint64_t chain, bool fragment);
uint64_t lancelot_hakios_mali_enqueue_complete(uint64_t chain,bool fragment,hkmt_completion_t completion,void *context);
uint64_t
lancelot_hakios_mali_enqueue_complete(uint64_t chain,bool fragment,hkmt_completion_t completion,void *context)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	if(!mali_worker_started || !hkmt_ready()) {lck_mtx_unlock(&mali_queue_lock);return 0;}
	while (mali_queue_count >= lgd_queue_limit(native_dvfs_requested,MALI_QUEUE)) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
		if(!hkmt_ready()){lck_mtx_unlock(&mali_queue_lock);return 0;}
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].owner_pid=proc_selfpid();
    mali_queue[at].completion=completion;
    mali_queue[at].completion_context=context;
	mali_queue[at].chain = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue[at].queued = mach_absolute_time();
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

uint64_t
lancelot_hakios_mali_enqueue(uint64_t chain,bool fragment)
{
    return lancelot_hakios_mali_enqueue_complete(chain,fragment,NULL,NULL);
}

/* Completion and safe retirement are distinct for a job recovered by reset. */
bool lancelot_hakios_mali_wait(uint64_t seq, uint64_t timeout_ns);
int lancelot_hakios_mali_wait_result(uint64_t seq,uint64_t timeout_ns,bool retirement);
bool lancelot_hakios_mali_retired(uint64_t seq,uint64_t timeout_ns);
int
lancelot_hakios_mali_wait_result(uint64_t seq,uint64_t timeout_ns,bool retirement)
{
	uint64_t deadline;
	int result;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;     /* (an "infinite" one, INT64_MAX, would wrap) */
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
    result=seq>mali_queued_seq?LM_EINVAL:mali_done_seq<seq?LM_ETIMEDOUT:LM_OK;
    if(!result && hkmt_failed_seq && seq>=hkmt_failed_seq)result=LM_EIO;
    if(!result)for(struct hkmt_completion_error *e=hkmt_completion_errors;e;e=e->next) {
        if(e->sequence==seq){result=retirement&&e->dma_safe?LM_OK:e->result;break;}
    }
	lck_mtx_unlock(&mali_queue_lock);
	return result;
}
bool lancelot_hakios_mali_wait(uint64_t seq,uint64_t timeout_ns)
{return lancelot_hakios_mali_wait_result(seq,timeout_ns,false)==LM_OK;}
bool lancelot_hakios_mali_retired(uint64_t seq,uint64_t timeout_ns)
{return lancelot_hakios_mali_wait_result(seq,timeout_ns,true)==LM_OK;}

int lancelot_hakios_mali_profile(uint64_t sequence,uint64_t times[4],uint32_t *hz,int32_t *error);
int lancelot_hakios_mali_profile(uint64_t sequence,uint64_t times[4],uint32_t *hz,int32_t *error)
{
    if(!sequence || !times || !hz || !error)return LM_EINVAL;
    lck_mtx_lock(&mali_queue_lock);
    struct hkmt_completed_profile p=hkmt_profiles[sequence%1024];
    int result=sequence>mali_queued_seq?LM_EINVAL:mali_done_seq<sequence?LM_EBUSY:p.sequence!=sequence?LM_EINVAL:LM_OK;
    if(!result){times[0]=p.start_ns;times[1]=p.end_ns;times[2]=p.jm_ns;times[3]=p.worker_ns;*hz=p.hz;*error=p.error;}
    lck_mtx_unlock(&mali_queue_lock);return result;
}

static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t nap;clock_interval_to_deadline(5,NSEC_PER_SEC,&nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock,LCK_SLEEP_DEFAULT,(event_t)&mali_wait_event,THREAD_UNINT,nap);
		lck_mtx_unlock(&mali_wait_lock);
		printf("mali: step %u of 6; user space says %u%s; %u clients, %u buffers, %u job chains (%u failed), %u pictures shown\n",
		    reached, user_said, user_said == 1 ? " -- a program ran a job on the GPU" : "", gHakiosMaliClients, gHakiosMaliBuffers,
		    gHakiosMaliSubmits, gHakiosMaliFailed, gHakiosMaliShown);
		lck_mtx_lock(&kept_lock);
		for (uint32_t i = kept_next > KEPT_LINES ? kept_next - KEPT_LINES : 0; i < kept_next; i++) {
			printf("kept: %s\n", kept_lines[i % KEPT_LINES]);
		}
		lck_mtx_unlock(&kept_lock);
		if (gl_said != 0) {
			printf("mali: gl_test drew on %s; %u%% of the picture inside the triangle (26%% by geometry); "
			    "the middle pixel red %u green %u blue %u alpha %u\n", gl_said / 1000 == 1 ? "THE MALI" : "the processor",
			    gl_said % 1000, gl_middle & 0xff, (gl_middle >> 8) & 0xff, (gl_middle >> 16) & 0xff, gl_middle >> 24);
		}
		if (last_failure.count != 0) {
			printf("mali: the last of %u unfinished: slot %u, chain at %#llx, interrupts %#x, slot status %#x, "
			    "MMU interrupts %#x, fault status %#x at address %#x\n", last_failure.count, last_failure.slot,
			    last_failure.chain, last_failure.raw, last_failure.status, last_failure.mmu, last_failure.fault,
			    last_failure.fault_address);
			printf("mali: the GPU's tables at %#llx (ours at %#llx); for the fault's address: %#llx, %#llx, %#llx\n",
			    last_failure.transtab, pt_top_phys, last_failure.walk[0], last_failure.walk[1], last_failure.walk[2]);
			for (uint32_t i = 0; i < gHakiosMaliBuffers && i < 32; i++) {
				printf("mali:   buffer %u at %#llx..%#llx (asked %#x, flags %#x)%s\n", i, gHakiosMaliMade[i].address,
				    gHakiosMaliMade[i].address + gHakiosMaliMade[i].size, gHakiosMaliMade[i].asked, gHakiosMaliMade[i].flags,
				    last_failure.fault_address >= gHakiosMaliMade[i].address &&
				    last_failure.fault_address < gHakiosMaliMade[i].address + gHakiosMaliMade[i].size ? "  <- the fault" : "");
			}
		}
	}
}

/* Called with the platform mutex held, after lmp_start. No second power path. */
static int hkmt_boot(void)
{
 mali_fastmap=1;
 (void)PE_parse_boot_argn("mali_fastmap",&mali_fastmap,sizeof(mali_fastmap));
 (void)PE_parse_boot_argn("mali_trace",&mali_trace,sizeof(mali_trace));
 mali_up();
 printf("lancelot-hakios: imported MT6768 driver commit=4f270371 step=%u/6 shared_AS0=1 fastmap=%u\n",reached,mali_fastmap);
 if (!lancelot_hakios_mali_ready()) return LM_EIO;
 printf("lancelot-hakios-ptw: cached=%u config=%#x dirty=%u publish=before_job retire=before_free\n",
     !mali_fastmap,rd(mali,AS0+AS_TRANSCFG_LO),hkmt_pt_dirty);
 printf("lancelot-hakios-behavior: fifo=64 async_submit=1 shared_AS0=1 fastmap=%u max_bo_mib=256 recovery=reset_restore submit_window=%u\n",mali_fastmap,lgd_queue_limit(native_dvfs_requested,MALI_QUEUE));
 thread_t thread;
 if(kernel_thread_start(mali_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 return LM_OK;
}

/* The donor leaves shared AS0 installed. Clock changes are allowed only with
 * all JM slots idle, no MMU/GPU fault, stable tables and powered G52 cores.
 * Platform mutex serializes this with donor submits and AS updates. */
static int hkmt_dvfs_guard(void)
{
    if(!hkmt_ready() || core_mmio_failed)return LM_ENODEV;
    wr(mali,JOB_INT_CLEAR,0);
    if((rd(mali,0x1010)&0x70007u) || (rd(mali,JOB_INT_RAWSTAT)&0x70000u) ||
       rd(mali,MMU_INT_RAWSTAT) || (rd(mali,GPU_INT_RAWSTAT)&0x81u))return LM_EIO;
    for(unsigned i=0;i<3;i++) {
        uint32_t js=0x1800+0x80*i;
        if((rd(mali,js+JS_COMMAND_NEXT)&7u) || (rd(mali,js+JS_STATUS)&0xffu)==8u)return LM_EIO;
    }
    uint64_t root=rd(mali,AS0)|((uint64_t)rd(mali,AS0+4)<<32);
    if(root!=pt_top_phys || rd(mali,AS0+AS_TRANSCFG_LO)!=TRANSCFG_AARCH64_4K ||
       rd(mali,AS0+AS_TRANSCFG_HI) || (rd(mali,AS0+AS_STATUS)&1u))return LM_EIO;
    for(unsigned i=1;i<16;i++)if(rd(mali,0x1c)&(1u<<i)) {
        uint32_t as=AS0+0x40*i;
        if(rd(mali,as) || rd(mali,as+4) || (rd(mali,as+AS_STATUS)&1u))return LM_EIO;
    }
    if((rd(mali,SHADER_READY)&shader_mask)!=shader_mask ||
       (rd(mali,L2_READY)&rd(mali,L2_PRESENT))!=rd(mali,L2_PRESENT) ||
       (rd(mali,TILER_READY)&rd(mali,TILER_PRESENT))!=rd(mali,TILER_PRESENT))return LM_EIO;
    return core_mmio_failed?LM_EIO:LM_OK;
}

void lancelot_hakios_cache_sync(void *address,size_t length,bool forCpu)
{
    if(forCpu)for_cpu(NULL,address,length);
    else for_device(NULL,address,length);
}
