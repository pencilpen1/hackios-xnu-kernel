/* SPDX-License-Identifier: GPL-2.0-only
 * Same-CPU XNU log transport. The loader explicitly offers an initialized FS
 * MUSB controller; XNU detaches/re-enumerates it after firmware has stopped.
 */
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include "lancelot_musb.h"
#include "lancelot_smp.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern size_t loader_log_snapshot(uint64_t, uint64_t *, uint8_t *, size_t);
extern void loader_log_write(const char *, size_t);
extern size_t lancelot_mmc_format_io(char *,size_t);
extern size_t lancelot_mmc_format_irq(char *,size_t);
extern size_t lancelot_mmc_format_sgi(unsigned,char *,size_t);
extern size_t lancelot_debug_format(char *,size_t);
void lancelot_usb_stop(void);
void lancelot_usb_fatal_log(void) __attribute__((noreturn));
void lancelot_usb_user_fault(uint64_t pc, uint64_t lr, uint64_t sp, uint64_t fp,
    uint32_t esr, uint64_t far, uint64_t code);
static struct lancelot_musb usb;
static vm_offset_t registers;
static thread_call_t poll_call;
static bool stopped, attached, fatal_polling, poll_busy;
static uint64_t frequency, attach_after;

static uint8_t usb_r8(__unused void *ctx, unsigned reg) { return *(volatile uint8_t *)(registers + reg); }
static uint16_t usb_r16(__unused void *ctx, unsigned reg) { return *(volatile uint16_t *)(registers + reg); }
static void usb_w8(__unused void *ctx, unsigned reg, uint8_t value)
{
	*(volatile uint8_t *)(registers + reg) = value; __asm__ volatile ("dsb sy" ::: "memory");
}
static void usb_w16(__unused void *ctx, unsigned reg, uint16_t value)
{
	*(volatile uint16_t *)(registers + reg) = value; __asm__ volatile ("dsb sy" ::: "memory");
}
static uint64_t usb_ticks(void) { return __builtin_arm_rsr64("CNTVCT_EL0"); }
static void usb_schedule(unsigned ms)
{
	uint64_t deadline; clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(poll_call, deadline);
}
static void usb_service(void)
{
	if (stopped || !registers) return;
	if (!attached) {
		if (usb_ticks() < attach_after) return;
		lu_reset(&usb);
		usb_w8(NULL, LU_INTRUSB, 255); usb_w16(NULL, LU_INTRTX, 65535); usb_w16(NULL, LU_INTRRX, 65535);
		usb_w16(NULL, LU_TXE, 3); usb_w16(NULL, LU_RXE, 2); usb_w8(NULL, LU_USBE, 4);
		usb_w8(NULL, LU_POWER, 0x40); attached = true;
		if (__atomic_load_n(&fatal_polling, __ATOMIC_ACQUIRE)) {
			static const char message[] = "lancelot-usb: KLOG2 fatal log service attached; FS EP01/81\n";
			loader_log_write(message, sizeof(message) - 1);
		} else {
			printf("lancelot-usb: XNU runtime KLOG2 attached; FS EP01/81, continuous boot\n");
		}
	}
	uint64_t begin = usb_ticks();
	/* Service at most 250 us per tick, only burst while a reply is outstanding.
	 * A disconnected host cannot turn this into an unbounded wait. */
	do {
		lu_poll(&usb);
		if (usb.reply == LU_NONE) break;
	} while (!stopped && usb_ticks() - begin < frequency / 4000);
}
static void usb_poll(__unused thread_call_param_t a, __unused thread_call_param_t b)
{
	if (stopped || __atomic_load_n(&fatal_polling, __ATOMIC_ACQUIRE)) return;
	bool expected = false;
	if (__atomic_compare_exchange_n(&poll_busy, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		usb_service();
		__atomic_store_n(&poll_busy, false, __ATOMIC_RELEASE);
	}
	if (!stopped && !__atomic_load_n(&fatal_polling, __ATOMIC_ACQUIRE)) usb_schedule(1);
}
/* Fatal EFI return can be refused while backing remains live. Service only the
 * existing log protocol on this CPU; allocate nothing, release no GPU resource,
 * and do not depend on thread calls after interrupts have been masked. */
__attribute__((noreturn))
void lancelot_usb_fatal_log(void)
{
	__atomic_store_n(&fatal_polling, true, __ATOMIC_RELEASE);
	for (;;) {
		if (!registers || !frequency) __builtin_arm_wfi();
		bool expected = false;
		if (__atomic_compare_exchange_n(&poll_busy, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			usb_service();
			__atomic_store_n(&poll_busy, false, __ATOMIC_RELEASE);
		}
	}
}
void lancelot_usb_stop(void)
{
	stopped = true;
	if (!registers) return;
	usb_w8(NULL, LU_POWER, 0); usb_w16(NULL, LU_TXE, 0);
	usb_w16(NULL, LU_RXE, 0); usb_w8(NULL, LU_USBE, 0);
}
__startup_func
static void lancelot_usb_prepare(void)
{
	DTEntry chosen; const void *property; unsigned size;
	uint64_t offer[3]; uint32_t enabled = 1, cpus = 0;
	PE_parse_boot_argn("lancelot_usb_log", &enabled, sizeof(enabled));
	PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
	if (registers || !enabled || !lancelot_smp_boot_allowed(cpus) || SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "lancelot-usb-log", &property, &size) != kSuccess || size != sizeof(offer)) return;
	memcpy(offer, property, sizeof(offer));
	if (offer[0] != 0x31474f4c4253554cULL || offer[1] != 0x11200000ULL) return;
	frequency = __builtin_arm_rsr64("CNTFRQ_EL0");
	if (frequency < 1000000 || frequency > 1000000000) return;
	registers = ml_io_map((vm_offset_t)offer[1], 4096);
	if (!registers) return;
	usb.r8 = usb_r8; usb.r16 = usb_r16; usb.w8 = usb_w8; usb.w16 = usb_w16;
	usb.snapshot = loader_log_snapshot;
	usb.io_snapshot = lancelot_mmc_format_io;
	usb.irq_snapshot = lancelot_mmc_format_irq;
	usb.sgi_snapshot = lancelot_mmc_format_sgi;
	usb.debug_snapshot = lancelot_debug_format;
	/* Distinct cursor domain from the preceding firmware LLOG1 ring. */
	usb.boot = offer[2] ^ 0x8000000000000000ULL;
	if (!(usb_r8(NULL, LU_POWER) & 0x40) || !usb_r16(NULL, 0x6c) || usb_r16(NULL, 0x6c) == 0xffff) {
		registers = 0; return;
	}
	/* Firmware already stopped DMA/QMU and masked MUSB's external IRQ. Keep
	 * its calibrated PHY, clocks and role; restart only the USB protocol state. */
	usb_w8(NULL, LU_POWER, 0); stopped = false;
	attach_after = usb_ticks() + frequency * 30 / 1000;
}
/* VM is ready here. Prepare only MMIO/protocol context before IOKit; thread
 * call allocation and scheduling keep their previously working boot phase. */
STARTUP(SYSCTL, STARTUP_RANK_LAST, lancelot_usb_prepare);
__startup_func
static void lancelot_usb_init(void)
{
	if (!registers) lancelot_usb_prepare();
	if (!registers || poll_call || __atomic_load_n(&fatal_polling, __ATOMIC_ACQUIRE)) return;
	poll_call = thread_call_allocate_with_options(usb_poll, NULL, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
	if (!poll_call) return;
	usb_schedule(30);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, lancelot_usb_init);

/* Called only after VM recovery has failed, immediately before normal Mach
 * exception delivery. Records original registers without dereferencing user
 * memory, replacing exception handlers or changing the fault result. */
void lancelot_usb_user_fault(uint64_t pc, uint64_t lr, uint64_t sp, uint64_t fp,
    uint32_t esr, uint64_t far, uint64_t code)
{
	if (!attached || stopped) return;
	char name[64] = {0};
	proc_selfname(name, sizeof(name));
	printf("user-fault: %s[%d] pc=%llx lr=%llx sp=%llx fp=%llx esr=%x far=%llx code=%llu\n",
	    name, proc_selfpid(), pc, lr, sp, fp, esr, far, code);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, usb_log_polls, CTLFLAG_RD | CTLFLAG_LOCKED, &usb.polls, "MUSB runtime service steps");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, usb_log_resets, CTLFLAG_RD | CTLFLAG_LOCKED, &usb.resets, "USB bus resets");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, usb_log_commands, CTLFLAG_RD | CTLFLAG_LOCKED, &usb.commands, "host log commands");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, usb_log_sent, CTLFLAG_RD | CTLFLAG_LOCKED, &usb.sent, "USB bytes queued");
static int sysctl_usb_log(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	if (!req->newptr || !req->newlen || req->newlen > LU_SNAPSHOT) return EINVAL;
	if (!attached || stopped) return ENXIO;
	char *buffer = kalloc_data(req->newlen, Z_WAITOK);
	if (!buffer) return ENOMEM;
	int error = SYSCTL_IN(req, buffer, req->newlen);
	if (!error) loader_log_write(buffer, req->newlen);
	kfree_data(buffer, req->newlen);
	return error;
}
/* Root-only write, copied before touching the console lock, bounded per call. */
SYSCTL_PROC(_kern_redwood, OID_AUTO, usb_log, CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_LOCKED,
    0, 0, sysctl_usb_log, "S", "append user-space file logs to the runtime console buffer");
