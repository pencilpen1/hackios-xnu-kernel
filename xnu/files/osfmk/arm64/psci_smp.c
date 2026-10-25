/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>
#include "../../bsd/dev/lancelot_smp.h"

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");

/* MTK entry handoff, supplied as PSCI context_id (physical). Firmware CPU_ON
 * enters EL2 on this device; XNU's reset vector expects EL1. No stack, virtual
 * pointers or MMIO before transition. Two dedicated cache lines per CPU. */
struct psci_smp_entry_note {
	uint64_t entered, mpidr, el, sctlr, vector, reset_data, entries, cpu_data;
	uint64_t final_el, hcr, cntvoff, sctlr_el2, fault_esr, fault_elr, fault_far, reserved;
};
static struct psci_smp_entry_note psci_smp_entry_notes[MAX_CPUS] __attribute__((aligned(128)));
_Static_assert(sizeof(struct psci_smp_entry_note) == 128, "two physical evidence cache lines");
_Static_assert(__builtin_offsetof(struct psci_smp_entry_note, vector) == 32 &&
    __builtin_offsetof(struct psci_smp_entry_note, reset_data) == 40 &&
    __builtin_offsetof(struct psci_smp_entry_note, entries) == 48 &&
    __builtin_offsetof(struct psci_smp_entry_note, final_el) == 64 &&
    __builtin_offsetof(struct psci_smp_entry_note, fault_esr) == 96, "entry assembly note offsets");
static bool psci_smp_mtk;

__attribute__((naked)) static void
psci_smp_mtk_entry(void)
{
	__asm__ volatile (
	    "msr daifset, #0xf\n"
	    "mov x18, x0\n" /* physical context retained across ERET */
	    "mov x9, #1\n"
	    "str x9, [x0]\n"
	    "mrs x9, mpidr_el1\n"
	    "str x9, [x0, #8]\n"
	    "mrs x9, CurrentEL\n"
	    "str x9, [x0, #16]\n"
	    "mrs x9, sctlr_el1\n"
	    "str x9, [x0, #24]\n"
	    "dc cvac, x0\n"
	    "dsb sy\n"
	    "ldr x9, [x0, #40]\n"
	    "ldr x10, [x9, #8]\n" /* ResetHandlerData.cpu_data_entries */
	    "str x10, [x0, #48]\n"
	    "mrs x9, CurrentEL\n"
	    "cmp x9, #4\n"
	    "b.eq 70f\n" /* firmware may already supply EL1 on other revisions */
	    "cmp x9, #8\n"
	    "b.ne 95f\n" /* only observed EL2 or explicit EL1 is supported */
	    "adr x9, 80f\n"
	    "msr vbar_el2, x9\n" /* actual EL2 exception evidence, before setup */
	    "isb\n"
	    "mrs x9, sctlr_el2\n"
	    "str x9, [x18, #88]\n"
	    "tbnz x9, #0, 95f\n" /* PSCI CPU_ON must arrive with MMU disabled */
	    "movz x9, #0x0830\n"
	    "movk x9, #0x30c5, lsl #16\n" /* SCTLR_EL2 RES1, little endian/MMU off */
	    "msr sctlr_el2, x9\n"
	    "mov x9, #0x80000000\n" /* HCR.RW: EL1 AArch64, no stage2/VM/traps */
	    "msr hcr_el2, x9\n"
	    "str x9, [x18, #72]\n"
	    "isb\n"
	    "mrs x9, cnthctl_el2\n"
	    "orr x9, x9, #3\n" /* Linux el2_setup: EL1 physical timer/counter */
	    "msr cnthctl_el2, x9\n"
	    "msr cntvoff_el2, xzr\n" /* same physical timebase as boot EL1 */
	    "mrs x9, cntvoff_el2\n"
	    "str x9, [x18, #80]\n"
	    "mrs x9, id_aa64pfr0_el1\n"
	    "ubfx x9, x9, #24, #4\n"
	    "cbz x9, 95f\n" /* Lancelot requires the real GICv3 system interface */
	    "mrs x9, ICC_SRE_EL2\n"
	    "orr x9, x9, #1\n"
	    "orr x9, x9, #8\n" /* SRE + Enable lower-EL system register access */
	    "msr ICC_SRE_EL2, x9\n"
	    "isb\n"
	    "mrs x9, ICC_SRE_EL2\n"
	    "tbz x9, #0, 95f\n"
	    "msr ICH_HCR_EL2, xzr\n"
	    "mrs x9, midr_el1\n"
	    "msr vpidr_el2, x9\n"
	    "mrs x9, mpidr_el1\n"
	    "msr vmpidr_el2, x9\n"
	    "msr hstr_el2, xzr\n"
	    "msr vttbr_el2, xzr\n"
	    "mov x9, #0x33ff\n" /* CPTR_EL2 RES1; no FP/ASIMD/debug trap */
	    "msr cptr_el2, x9\n"
	    "mov x11, xzr\n"
	    "mrs x9, id_aa64dfr0_el1\n"
	    "sbfx x9, x9, #8, #4\n"
	    "cmp x9, #1\n"
	    "b.lt 60f\n"
	    "mrs x9, pmcr_el0\n"
	    "ubfx x11, x9, #11, #5\n" /* Linux MDCR.HPMN, no EL1 debug traps */
	    "60: msr mdcr_el2, x11\n"
	    "movz x9, #0x0800\n"
	    "movk x9, #0x30d0, lsl #16\n" /* XNU's pre-MMU EL1 RES1 state */
	    "msr sctlr_el1, x9\n"
	    "msr sp_el1, xzr\n" /* original reset flow supplies stacks before use */
	    "mov x9, #0x3c5\n" /* DAIF masked, AArch64 EL1h */
	    "msr spsr_el2, x9\n"
	    "adr x9, 70f\n"
	    "msr elr_el2, x9\n"
	    "isb\n"
	    "eret\n"
	    "70: mrs x9, CurrentEL\n"
	    "str x9, [x18, #64]\n"
	    "cmp x9, #4\n"
	    "b.ne 95f\n"
	    "mov x9, #2\n"
	    "str x9, [x18]\n"
	    "dc cvac, x18\n"
	    "add x9, x18, #64\n"
	    "dc cvac, x9\n"
	    "dsb sy\n"
	    "ldr x16, [x18, #32]\n"
	    "br x16\n" /* original reset_vector still validates MPIDR and handler */
	    ".balign 2048\n"
	    "80:\n.rept 16\n b 90f\n.balign 128\n.endr\n"
	    "90: mrs x9, esr_el2\n"
	    "str x9, [x18, #96]\n"
	    "mrs x9, elr_el2\n"
	    "str x9, [x18, #104]\n"
	    "mrs x9, far_el2\n"
	    "str x9, [x18, #112]\n"
	    "95: mov x9, #255\n"
	    "str x9, [x18]\n"
	    "dc cvac, x18\n"
	    "add x9, x18, #64\n"
	    "dc cvac, x9\n"
	    "dsb sy\n"
	    "mov x0, #2\n"
	    "movk x0, #0x8400, lsl #16\n"
	    "smc #0\n" /* transition rejected/faulted: stop only this CPU */
	    "96: wfi\n b 96b\n");
}

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
	psci_smp_mtk = lancelot_smp_boot_allowed(8);
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
	/* initCPU runs on the secondary with interrupts disabled. Publish the
	 * atomic stage only; the CPU_ON caller prints after it observes RUNNING. */
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	uint64_t context = 0;
	int64_t status;

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	if (psci_smp_mtk) {
		struct psci_smp_entry_note *note = &psci_smp_entry_notes[cpu];
		*note = (struct psci_smp_entry_note){.vector=entry,
		    .reset_data=ml_static_vtop((vm_offset_t)&ResetHandlerData),
		    .cpu_data=(uint64_t)(uintptr_t)CpuDataEntries[cpu].cpu_data_paddr};
		context = ml_static_vtop((vm_offset_t)note);
		printf("psci: cpu %d reset-data=0x%llx entries=0x%llx expected=0x%llx cpu-data=0x%llx handler=0x%llx\n",
		    cpu, note->reset_data, (uint64_t)ResetHandlerData.cpu_data_entries,
		    (uint64_t)ml_static_vtop((vm_offset_t)CpuDataEntries), note->cpu_data, (uint64_t)cdp->cpu_reset_handler);
		entry = ml_static_vtop((vm_offset_t)&psci_smp_mtk_entry);
	}
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	CleanPoC_DcacheRegion((vm_offset_t)&ResetHandlerData, sizeof(ResetHandlerData));
	CleanPoC_DcacheRegion((vm_offset_t)CpuDataEntries, sizeof(CpuDataEntries));
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	printf("psci: CPU_ON cpu %d MPIDR 0x%x entry=0x%llx\n", cpu, phys_id, entry);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, context);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage == PSCI_SMP_CPU_RUNNING) {
			printf("psci: cpu %d (MPIDR 0x%x) joined scheduler\n", cpu, phys_id);
			if (psci_smp_mtk) {
				struct psci_smp_entry_note *note = &psci_smp_entry_notes[cpu];
				FlushPoC_DcacheRegion((vm_offset_t)note, sizeof(*note));
				printf("psci: cpu %d transition initial-EL=%llu final-EL=%llu stage=%llu HCR=%llx CNTVOFF=%llx\n",
				    cpu, note->el >> 2, note->final_el >> 2, note->entered, note->hcr, note->cntvoff);
			}
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		if (psci_smp_mtk) {
			FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_entry_notes[cpu], sizeof(psci_smp_entry_notes[cpu]));
			if (psci_smp_entry_notes[cpu].entered == 255) break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);
	if (psci_smp_mtk) {
		struct psci_smp_entry_note *note = &psci_smp_entry_notes[cpu];
		FlushPoC_DcacheRegion((vm_offset_t)note, sizeof(*note));
		printf("psci: cpu %d entry-stage=%llu actual-MPIDR=0x%llx initial-EL=%llu final-EL=%llu HCR=%llx CNTVOFF=%llx reset-entries=%llx fault=%llx/%llx/%llx\n",
		    cpu, note->entered, note->mpidr, note->el >> 2, note->final_el >> 2, note->hcr, note->cntvoff,
		    note->entries, note->fault_esr, note->fault_elr, note->fault_far);
	}

	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}

#endif /* defined(GENERIC_PSCI_SMP) */
