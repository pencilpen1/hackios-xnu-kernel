/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
#include "lancelot_cpufreq.h"
#include "lancelot_cpufreq_thermal.h"
#include "lancelot_cpufreq_sspm.h"
#include "lancelot_mali_pmic.h"
#include "lancelot_pwrap.h"
#include "lancelot_storage.h"

/* No CPU thermal backend exists here yet. Report this honestly; neither an
 * inherited CPU-DVFS repository nor Android's calibration LOG is a live XNU
 * thermal provider. A future provider supplies these three bounded callbacks. */
__attribute__((weak)) int
lancelot_cpu_thermal_start(uint64_t deadline_ns __unused)
{
	return -19; /* ENODEV; no MMIO/IPI/resource acquisition. */
}

__attribute__((weak)) int
lancelot_cpu_thermal_read(struct lcf_thermal_reading *reading)
{
	bzero(reading, sizeof(*reading));
	return -19;
}

__attribute__((weak)) void
lancelot_cpu_thermal_stop(void)
{
}

static vm_offset_t lcf_sram, lcf_pll, lcf_div;
static uint32_t lcf_original[2], lcf_owned[2];
static uint32_t lcf_pvt[3][16], lcf_method[3][16];
static bool lcf_active;
static bool lcf_once;
static struct lcs_state lcf_startup;
static vm_offset_t lcf_mailbox[LCS_BANKS], lcf_wrap;
static struct lmp_pmic_state lcf_cpu_pmic;
static uint32_t lcf_boot_backup[LCF_SRAM_SIZE/4];
static struct lmp_cpu_rails lcf_last_rails;
static uint64_t lcf_last_rails_ns;

static uint64_t
lcf_now_ns(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static const char *
lcf_thermal_name(enum lcf_thermal_policy policy)
{
	return policy == LCF_THERMAL_OPTIONAL_OFF ? "unavailable_optional" :
	    policy == LCF_THERMAL_LIVE ? "live" : "limit_no_boost";
}

static enum lcf_thermal_policy
lcf_thermal_eval(bool started, struct lcf_thermal_reading *reading, int *error)
{
	bzero(reading, sizeof(*reading));
	*error = started ? lancelot_cpu_thermal_read(reading) : 0;
	return lcf_thermal_classify(started, *error, reading, lcf_now_ns());
}

static bool
lcf_thermal_begin(void)
{
	uint64_t now = lcf_now_ns(), deadline;
	int error = -22;
	if (now <= UINT64_MAX - LCF_THERMAL_START_NS) {
		deadline = now + LCF_THERMAL_START_NS;
		error = lancelot_cpu_thermal_start(deadline);
		/* Do not publish late initialization as success, even if its provider
		 * violated the deadline contract. The provider itself must bound waits. */
		if (!error && lcf_now_ns() > deadline) error = -60;
	}
	if (error) lancelot_cpu_thermal_stop(); /* also release partial startup */
	printf("lancelot-cpufreq-thermal: start_error=%d started=%u optional=1 blocks_cpufreq=0 startup_budget_ms=100\n",
	    error, error == 0);
	return error == 0;
}

static uint32_t
lcf_read(unsigned offset)
{
	uint32_t value = *(volatile uint32_t *)(lcf_sram + offset);
	__asm__ volatile("dsb sy" ::: "memory");
	return value;
}

static void
lcf_snapshot(struct lcf_snapshot *s)
{
	bzero(s, sizeof(*s));
	for (unsigned i=0;i<4;i++) s->guard[i]=lcf_read(i*4);
	s->guard[4]=lcf_read(0x2a0);
	for (unsigned c=0;c<3;c++) {
		for (unsigned i=0;i<16;i++) {
			s->pvt[c][i]=lcf_read(0x10+c*0x90+i*4);
			s->method[c][i]=lcf_read(0x50+c*0x90+i*4);
		}
		s->sentinel[c]=lcf_read(0x90+c*0x90);
		s->current[c]=lcf_read(LCF_CURRENT+c*4);
	}
	for (unsigned c=0;c<2;c++) {
		s->request[c]=lcf_read(LCF_REQUEST+c*4);
		s->limit[c]=lcf_read(LCF_LIMIT+c*4);
		s->sched[c]=lcf_read(0x3a4+c*4);
	}
	s->sched_disabled=lcf_read(0x2bc); s->stress=lcf_read(0x2c0); s->pause=lcf_read(0x11d8);
	for (unsigned i=0;i<sizeof(s->cci)/sizeof(s->cci[0]);i++) s->cci[i]=lcf_read(LCF_CCI_MAP+i*4);
	for (unsigned i=0;i<LCF_LOG_COUNT;i++) {
		unsigned off=LCF_LOG+i*20;
		uint32_t hi=lcf_read(off+4), lo=lcf_read(off);
		if (hi!=lcf_read(off+4) || lo!=lcf_read(off)) continue;
		uint64_t stamp=(uint64_t)hi<<32 | lo;
		if (stamp>s->latest_log) s->latest_log=stamp;
	}
	for (unsigned i=0;i<4;i++) s->guard_end[i]=lcf_read(i*4);
	s->guard_end[4]=lcf_read(0x2a0);
}

/* Accept only the live ARMPLL mux and explicitly defined dividers.
 * Samples spanning an active firmware transition are retried, not interpreted
 * as a frequency. The existing Mali observer continues the independent log. */
static bool
lcf_clock_registers(unsigned c, uint32_t raw[3])
{
	if (c>=3 || !raw) return false;
	unsigned off=c==0?0x218:c==1?0x208:0x228;
	unsigned divoff=c<2?0x2a0+4*c:0x2e0;
	uint32_t con0=*(volatile uint32_t *)(lcf_pll+off);
	uint32_t con1=*(volatile uint32_t *)(lcf_pll+off+4);
	uint32_t cfg=*(volatile uint32_t *)(lcf_div+divoff);
	__asm__ volatile("dsb sy" ::: "memory");
	if (con0!=*(volatile uint32_t *)(lcf_pll+off) || con1!=*(volatile uint32_t *)(lcf_pll+off+4) ||
	    cfg!=*(volatile uint32_t *)(lcf_div+divoff)) return false;
	raw[0]=con0;raw[1]=con1;raw[2]=cfg;
	return true;
}

static bool
lcf_clock_observe(unsigned c, uint64_t *hz)
{
	uint32_t raw[3];
	return lcf_clock_registers(c,raw) && lcf_clock_hz(raw[0],raw[1],raw[2],hz);
}

static bool
lcf_clocks_match(const struct lcf_snapshot *s)
{
	for (unsigned c=0;c<3;c++) {
		uint64_t hz;
		if (s->current[c]>=16 || !lcf_clock_observe(c,&hz) ||
		    lcf_read(LCF_CURRENT+c*4)!=s->current[c]) return false;
		uint64_t want=(uint64_t)lcf_mhz[c][s->current[c]]*1000000;
		if (hz+1000000<want || hz>want+1000000) return false;
	}
	return true;
}

static int
lcf_mail_read(void *cookie __unused, enum lcs_bank bank, unsigned off, uint32_t *value)
{
	if (!value || !lcs_access(bank,off) || !lcf_mailbox[bank]) return LCS_EINVAL;
	*value=*(volatile uint32_t *)(lcf_mailbox[bank]+off);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static int
lcf_mail_write(void *cookie __unused, enum lcs_bank bank, unsigned off, uint32_t value)
{
	if (!lcs_access(bank,off) || !lcf_mailbox[bank]) return LCS_EINVAL;
	/* The portable core enforces the narrower state/CPU-pin write allowlist. */
	*(volatile uint32_t *)(lcf_mailbox[bank]+off)=value;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static uint64_t lcf_io_now(void *cookie __unused) {return lcf_now_ns();}
static void lcf_io_wait(void *cookie __unused, unsigned us) {delay_for_interval(us,NSEC_PER_USEC);}
static int
lcf_wrap_read(void *cookie __unused, uint32_t off, uint32_t *value)
{
	if (!lcf_wrap || !value || off>0xffc || (off&3)) return LMP_EINVAL;
	*value=*(volatile uint32_t *)(lcf_wrap+off);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
static int
lcf_wrap_write(void *cookie __unused, uint32_t off, uint32_t value)
{
	/* Read transactions require CMD and VLDCLR only; no PWRAP setup writes. */
	if (!lcf_wrap || (off!=LMP_PMIC_WACS2_CMD && off!=LMP_PMIC_WACS2_VLDCLR) ||
	    (off==LMP_PMIC_WACS2_CMD && (value&0x80000000u)) ||
	    (off==LMP_PMIC_WACS2_VLDCLR && value!=1)) return LMP_EINVAL;
	*(volatile uint32_t *)(lcf_wrap+off)=value;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
static void lcf_wrap_poison(void *cookie __unused) {(void)lancelot_pwrap_poison();}

static int
lcf_rail_read(struct lmp_cpu_rails *rails)
{
	if (!lcf_wrap) return LMP_ENODEV;
	if (!lcf_cpu_pmic.validated) {
		struct lmp_pmic_registers regs;
		lmp_pmic_mt6358_registers(&regs);
		const struct lmp_pmic_io io={.read32=lcf_wrap_read,.write32=lcf_wrap_write,
		    .now_ns=lcf_io_now,.delay_us=lcf_io_wait,.timeout_ns=LMP_PMIC_WACS_TIMEOUT_NS,
		    .acquire=lancelot_pwrap_acquire,.release=lancelot_pwrap_release,.poison=lcf_wrap_poison};
		int e=lmp_pmic_probe(&lcf_cpu_pmic,&io,&regs);
		if (e) return e;
	}
	return lmp_pmic_cpu_rails(&lcf_cpu_pmic,rails);
}

static bool
lcf_board_profile(void)
{
	/* The existing lancelot boot contract plus the audited CPU_LEVEL_6 profile
	 * for this device. Not a guessed efuse MMIO address or bootarg voltage table. */
	DTEntry chosen;const void *prop;unsigned bytes;struct lancelot_storage offer={0};
	if (SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
	    SecureDTGetProperty(chosen,"lancelot-emmc",&prop,&bytes)!=kSuccess ||
	    (bytes!=LANCELOT_STORAGE_V1_SIZE && bytes!=sizeof(offer))) return false;
	memcpy(&offer,prop,bytes);
	if (offer.magic!=LANCELOT_STORAGE_MAGIC || offer.size!=bytes ||
	    !((offer.version==1 && bytes==LANCELOT_STORAGE_V1_SIZE) || (offer.version==2 && bytes==sizeof(offer))) ||
	    offer.mmio!=0x11230000 || offer.partition_bytes!=LANCELOT_UBUNTU_BYTES ||
	    !offer.partition_offset || (offer.partition_offset&511) ||
	    offer.partition_offset>=(UINT64_C(1)<<41)-LANCELOT_UBUNTU_BYTES) return false;
	const ml_topology_info_t *t=ml_get_topology_info();unsigned seen=0;
	if (t->num_cpus!=8) return false;
	for (unsigned i=0;i<8;i++) {
		unsigned p=t->cpus[i].phys_id;
		if ((p&~0x700u) || (seen&(1u<<(p>>8)))) return false;
		seen|=1u<<(p>>8);
	}
	return seen==255;
}

static bool
lcf_voltage_matches(const struct lcf_snapshot *s)
{
	struct lmp_cpu_rails rails;
	if (lcf_rail_read(&rails)) return false;
	for (unsigned c=0;c<2;c++) {
		if (s->current[c]>=16 || s->current[2]>=16) return false;
		unsigned selector=s->pvt[c][s->current[c]]>>16;
		/* CCI shares the LL(VPROC12) rail, not the large-core rail. */
		if (!c && (s->pvt[2][s->current[2]]>>16)>selector) selector=s->pvt[2][s->current[2]]>>16;
		if (selector>96 || rails.vproc_uv[c]<500000u+selector*6250u) return false;
	}
	if (!lcf_clocks_match(s)) return false; /* reject a transition spanning the rail read */
	lcf_last_rails=rails;
	lcf_last_rails_ns=lcf_now_ns();
	return true;
}

static int
lcf_bootstrap(void)
{
	if (!lcf_board_profile()) return LCS_EPROTO;
	/* GPU init registers the existing shared AP WACS2 handoff after the CPU
	 * thread is launched. Wait boundedly, without changing the user's input file. */
	uint64_t begin=lcf_now_ns();
	for (unsigned i=0;!lancelot_pwrap_available();i++) {
		uint64_t now=lcf_now_ns();
		if (i>=100 || now<begin || now-begin>=UINT64_C(5000000000)) return LCS_ETIMEDOUT;
		delay_for_interval(50,NSEC_PER_MSEC);
	}
	lcf_wrap=ml_io_map(LMP_PMIC_WRAP_BASE_PHYS,0x1000);
	struct lmp_cpu_rails rails;
	int e=lcf_rail_read(&rails);
	printf("lancelot-cpufreq-rails: probe=%d chip=%x routing=LL12_L11 read_transactions=1 CPU_rail_writes=0\n",e,lcf_cpu_pmic.chip_id);
	if (e) return e;
	printf("lancelot-cpufreq-rails: initial_vproc_uv=%u/%u vsram_uv=%u/%u\n",
	    rails.vproc_uv[0],rails.vproc_uv[1],rails.vsram_uv[0],rails.vsram_uv[1]);
	unsigned initial[2]={16,16};
	for (unsigned c=0;c<2;c++) {
		uint32_t raw[3]={0};bool frequency_known=false;
		bool stable=lcf_clock_registers(c,raw);
		bool valid=stable && lcf_initial_request(c,raw[0],raw[1],raw[2],&initial[c],&frequency_known);
		printf("lancelot-cpufreq-initial: cluster=%u stable=%u con0=%x con1=%x cfg=%x divider=%u request=%u initial_frequency_known=%u valid=%u\n",
		    c,stable,raw[0],raw[1],raw[2],(raw[2]>>17)&31,initial[c],frequency_known,valid);
		if (!valid) return LCS_EPROTO;
	}
	/* Live FDT-confirmed v1 mailboxes. No cfg/reset/clock/MPU writes. */
	static const uint64_t pa[LCS_BANKS]={LCF_SRAM_PA,0x10450000,0x10451000,0x10460000,0x10461000,0x10470000,0x10471000};
	for (unsigned b=0;b<LCS_BANKS;b++) {
		lcf_mailbox[b]=b==LCS_REPO?lcf_sram:ml_io_map(pa[b],b==LCS_CTRL0||b==LCS_CTRL1||b==LCS_CTRL2?8:128);
		if (!lcf_mailbox[b]) return LCS_EIO;
	}
	lcf_startup.io=(struct lcs_io){.read32=lcf_mail_read,.write32=lcf_mail_write,.now_ns=lcf_io_now,.wait_us=lcf_io_wait};
	e=lcs_cold_start(&lcf_startup,initial,lcf_boot_backup);
	printf("lancelot-cpufreq-init: error=%d stage=%u commands=%u ack=%d poisoned=%u bad_mbox=%u slot=%u expected=%x got=%x profile=CPU_LEVEL_6 initial=%u/%u\n",
	    e,lcf_startup.stage,lcf_startup.commands,lcf_startup.ack,lcf_startup.poisoned,
	    lcf_startup.bad_bank,lcf_startup.bad_slot,lcf_startup.bad_expected,lcf_startup.bad_value,initial[0],initial[1]);
	if (lcf_startup.backed_up) {
		printf("lancelot-cpufreq-before: guard=%x/%x current=%x/%x/%x request=%x/%x bytes=%u\n",
		    lcf_boot_backup[0],lcf_boot_backup[0x2a0/4],lcf_boot_backup[LCF_CURRENT/4],
		    lcf_boot_backup[LCF_CURRENT/4+1],lcf_boot_backup[LCF_CURRENT/4+2],
		    lcf_boot_backup[LCF_REQUEST/4],lcf_boot_backup[LCF_REQUEST/4+1],LCF_SRAM_SIZE);
	}
	if (e) return e;
	/* Guard/table publication and four ACKs are not hardware admission. */
	struct lcf_snapshot first, now;
	lcf_snapshot(&first);begin=lcf_now_ns();
	for (unsigned i=0;i<40;i++) {
		delay_for_interval(50,NSEC_PER_MSEC);
		if ((e=lcs_receive(&lcf_startup))) break;
		lcf_snapshot(&now);
		/* lcs_cold_start readback-proved a zero log before INIT. A new nonzero
		 * record is sufficient at an unchanged OPP; it need not tick periodically. */
		if (lcf_snapshot_valid(&now) && now.latest_log && now.latest_log>=first.latest_log &&
		    now.request[0]==initial[0] && now.request[1]==initial[1] &&
		    now.current[0]==initial[0] && now.current[1]==initial[1] &&
		    !memcmp(first.pvt,now.pvt,sizeof(now.pvt)) && !memcmp(first.method,now.method,sizeof(now.method)) &&
		    lcf_clocks_match(&now) && lcf_voltage_matches(&now)) {
			lcf_startup.stage=7;
			printf("lancelot-cpufreq-init: live_feedback=1 log=%llu current=%u/%u/%u rx=%u independent_PLL_and_rails=1\n",
			    now.latest_log,now.current[0],now.current[1],now.current[2],lcf_startup.rx_count);
			return 0;
		}
		uint64_t at=lcf_now_ns();if (at<begin || at-begin>=LCS_ACK_NS) break;
	}
	/* Preserve the actual failed feedback snapshot. The outer pre-init sample
	 * must not be printed as if it were the firmware's post-command state. */
	lcf_snapshot(&now);
	struct lmp_cpu_rails failed_rails={0};int rail_error=lcf_rail_read(&failed_rails);
	printf("lancelot-cpufreq-init-failure: error=%d reject=%x log=%llu/%llu current=%u/%u/%u request=%u/%u rail_error=%d vproc_uv=%u/%u vsram_uv=%u/%u\n",
	    e?e:LCS_ETIMEDOUT,lcf_snapshot_reject(&now),first.latest_log,now.latest_log,
	    now.current[0],now.current[1],now.current[2],now.request[0],now.request[1],rail_error,
	    failed_rails.vproc_uv[0],failed_rails.vproc_uv[1],failed_rails.vsram_uv[0],failed_rails.vsram_uv[1]);
	for (unsigned c=0;c<3;c++) {
		uint32_t raw[3]={0};bool stable=lcf_clock_registers(c,raw);
		printf("lancelot-cpufreq-failed-clock: cluster=%u stable=%u con0=%x con1=%x cfg=%x\n",c,stable,raw[0],raw[1],raw[2]);
	}
	lcf_startup.poisoned=true;
	return e?e:LCS_ETIMEDOUT;
}

static bool
lcf_write_request(unsigned c, uint32_t value)
{
	/* AP owns only these two request words. A changed owner or foreign writer
	 * stops control; never overwrite a firmware table or a PPM constraint. */
	if (c>=2 || !lcf_request_allowed(value,lcf_read(LCF_LIMIT+c*4)) ||
	    lcf_read(LCF_REQUEST+c*4)!=lcf_owned[c]) return false;
	if (value==lcf_owned[c]) return true; /* No repeated MMIO writes for an unchanged vote. */
	*(volatile uint32_t *)(lcf_sram+LCF_REQUEST+c*4)=value;
	__asm__ volatile("dsb sy" ::: "memory");
	lcf_owned[c]=value;
	return lcf_read(LCF_REQUEST+c*4)==value;
}

static void
lcf_quiesce(void)
{
	if (!os_atomic_xchg(&lcf_active, false, acq_rel)) return;
	if (lcf_startup.poisoned || !lcf_startup.claimed) return;
	/* A firmware reset or table replacement revokes the old request contract.
	 * In that case even restoration would write into unowned state. */
	struct lcf_snapshot now;
	lcf_snapshot(&now);
	if (!lcf_snapshot_valid(&now)) return;
	for (unsigned c=0;c<3;c++) for (unsigned i=0;i<16;i++)
		if (now.pvt[c][i]!=lcf_pvt[c][i] || now.method[c][i]!=lcf_method[c][i]) return;
	for (unsigned c=0;c<2;c++) (void)lcf_write_request(c, lcf_original[c]);
}

static void
lcf_thread(void *arg __unused, wait_result_t wr __unused)
{
	struct lcf_snapshot first, now;
	uint64_t previous[MAX_CPUS]={0}, then;
	const ml_topology_info_t *topology=ml_get_topology_info();
	/* Thermal failure never returns from this thread or changes CPU admission. */
	bool thermal_started = lcf_thermal_begin();
	struct lcf_thermal_reading thermal;
	int thermal_error;
	enum lcf_thermal_policy thermal_policy = lcf_thermal_eval(thermal_started, &thermal, &thermal_error);
	printf("lancelot-cpufreq-thermal: policy=%s read_error=%d calibrated=%u at_ns=%llu max_mC=%d blocks_cpufreq=0\n",
	    lcf_thermal_name(thermal_policy), thermal_error, thermal.calibrated,
	    (unsigned long long)thermal.at_ns, thermal.max_mC);
	lcf_snapshot(&first);
	delay_for_interval(200, NSEC_PER_MSEC);
	lcf_snapshot(&now);
	/* Published SRAM is not proof that this boot owns its CPU command channel.
	 * Do not vote through the former inherited path without mailbox/rail proof.
	 * Fresh cold startup remains the only admitted owner, never reset a live repo. */
	bool valid=false;
	if (!(lcf_snapshot_reject(&first)&LCF_REJECT_GUARD) || !(lcf_snapshot_reject(&now)&LCF_REJECT_GUARD))
		printf("lancelot-cpufreq: inherited_repository_refused ownership_unproved=1 no_votes=1\n");
	int init_error=0;
	if (!valid && (lcf_snapshot_reject(&first)&LCF_REJECT_GUARD) &&
	    (lcf_snapshot_reject(&now)&LCF_REJECT_GUARD) &&
	    !memcmp(first.guard,now.guard,sizeof(now.guard)) && first.latest_log==now.latest_log) {
		init_error=lcf_bootstrap();
		if (init_error) lcf_snapshot(&now);
		if (!init_error) {
			lcf_snapshot(&first);delay_for_interval(200,NSEC_PER_MSEC);
			init_error=lcs_receive(&lcf_startup);
			lcf_snapshot(&now);
			valid=!init_error && lcf_snapshot_valid(&first) && lcf_snapshot_valid(&now) &&
			    now.latest_log && now.latest_log>=first.latest_log && !memcmp(first.pvt,now.pvt,sizeof(now.pvt)) &&
			    !memcmp(first.method,now.method,sizeof(now.method)) &&
			    !memcmp(first.request,now.request,sizeof(now.request)) &&
			    now.current[0]==now.request[0] && now.current[1]==now.request[1] &&
			    lcf_clocks_match(&now) && lcf_voltage_matches(&now);
		}
	}
	printf("lancelot-cpufreq: admission=%u guard=%x/%x current=%u/%u/%u request=%u/%u log=%llu/%llu backend=SSPM_WFI_index cap_MHz=1800/2000\n",
	    valid,now.guard[0],now.guard[4],now.current[0],now.current[1],now.current[2],now.request[0],now.request[1],first.latest_log,now.latest_log);
	printf("lancelot-cpufreq-contract: reject=%x/%x advancing=%u init_stage=%u init_error=%d SRAM_PA=%llx no_raw_PLL_or_CPU_rail_writes=1\n",
	    lcf_snapshot_reject(&first),lcf_snapshot_reject(&now),now.latest_log>first.latest_log,
	    lcf_startup.stage,init_error,(unsigned long long)LCF_SRAM_PA);
	if (!valid) {
		if (thermal_started) lancelot_cpu_thermal_stop();
		return;
	}
	for (unsigned c=0;c<2;c++) lcf_original[c]=lcf_owned[c]=now.request[c];
	memcpy(lcf_pvt,now.pvt,sizeof(lcf_pvt));
	memcpy(lcf_method,now.method,sizeof(lcf_method));
	os_atomic_store(&lcf_active,true,release);
	for (unsigned i=0;i<topology->num_cpus&&i<MAX_CPUS;i++) (void)ml_cpu_idle_time(topology->cpus[i].cpu_id,&previous[i]);
	then=mach_absolute_time(); /* Idle baselines and the first load window share the same origin. */
	unsigned stale=0, pending=0, ticks=0;
	unsigned stop_reason=0;
	struct lcf_load_policy load_policy={0};
	uint64_t vote_at=0,clock_bad_at=0;
	uint64_t last_log=now.latest_log, vote_log=now.latest_log;
	bool vote_pending=false;
	uint32_t last_voltage_opp[3]={now.current[0],now.current[1],now.current[2]};
	while (os_atomic_load(&lcf_active,acquire)) {
		delay_for_interval(100,NSEC_PER_MSEC);
		if (!os_atomic_load(&lcf_active,acquire)) break;
		enum lcf_thermal_policy policy = lcf_thermal_eval(thermal_started, &thermal, &thermal_error);
		if (policy != thermal_policy) {
			printf("lancelot-cpufreq-thermal: policy=%s read_error=%d max_mC=%d blocks_cpufreq=0\n",
			    lcf_thermal_name(policy), thermal_error, thermal.max_mC);
			thermal_policy = policy;
		}
		uint64_t at=mach_absolute_time(),span=at-then;
		unsigned busy[2]={0}; bool load_valid=span!=0;
		for (unsigned i=0;i<topology->num_cpus&&i<MAX_CPUS;i++) {
			uint64_t idle=0;unsigned phys=topology->cpus[i].phys_id;
			if ((phys&~0x700u)||!ml_cpu_idle_time(topology->cpus[i].cpu_id,&idle)||idle<previous[i]) {load_valid=false;continue;}
			uint64_t delta=idle-previous[i];previous[i]=idle;
			unsigned use=span&&delta<span?(unsigned)((span-delta)*100/span):0,c=phys>=0x600;
			if (use>busy[c]) busy[c]=use;
		}
		then=at;
		if (lcf_startup.claimed && lcs_receive(&lcf_startup)) {stop_reason=1;break;}
		lcf_snapshot(&now);
		if (!lcf_snapshot_valid(&now)||memcmp(first.pvt,now.pvt,sizeof(now.pvt))||
		    memcmp(first.method,now.method,sizeof(now.method))||!load_valid) {stop_reason=2;break;}
		if (now.latest_log<last_log) {stop_reason=3;break;}
		last_log=now.latest_log;
		if (!lcf_clocks_match(&now)) {
			uint64_t ns=lcf_now_ns();if (!clock_bad_at) clock_bad_at=ns;
			if (++stale>=10 || ns<clock_bad_at || ns-clock_bad_at>=UINT64_C(1000000000)) {stop_reason=4;break;}
			continue;
		}
		clock_bad_at=0;
		stale=0;
		/* A readable request word is not completion. Wait for the firmware's
		 * current OPP and the independent PLL observation before another vote. */
		if (now.current[0]!=lcf_owned[0] || now.current[1]!=lcf_owned[1] ||
		    (vote_pending && now.latest_log<=vote_log)) {
			uint64_t ns=lcf_now_ns();
			if (++pending>=10 || (vote_pending && (ns<vote_at || ns-vote_at>=UINT64_C(1000000000)))) {stop_reason=5;break;}
			continue;
		}
		pending=0;
		if (vote_pending) printf("lancelot-cpufreq-complete: at_ns=%llu vote_ns=%llu current=%u/%u event=%llu\n",
		    lcf_now_ns(),vote_at,now.current[0],now.current[1],now.latest_log);
		vote_pending=false;
		if (lcf_startup.claimed &&
		    (memcmp(last_voltage_opp,now.current,sizeof(last_voltage_opp)) || ticks%10==0)) {
			if (!lcf_voltage_matches(&now)) {stop_reason=6;break;}
			memcpy(last_voltage_opp,now.current,sizeof(last_voltage_opp));
		}
		bool ok=true;
		/* Decide both targets before issuing either vote. A conflicting PPM
		 * floor in the second cluster must not leave a partial first vote. */
		unsigned target[2];
		for (unsigned c=0;c<2;c++) {
			target[c]=lcf_policy_target(&load_policy,c,busy[c],now.current[c],now.limit[c],lcf_now_ns(),thermal_policy!=LCF_THERMAL_LIMIT);
			if (target[c]>=16 || now.request[c]!=lcf_owned[c]) ok=false;
			else if (thermal_policy == LCF_THERMAL_LIMIT) target[c] = now.limit[c] & 0xffff;
		}
		if (!ok) {stop_reason=7;break;}
		/* A changed vote must produce NEW transition feedback before another
		 * request. An unchanged vote needs neither an MMIO rewrite nor new log. */
		vote_pending=target[0]!=lcf_owned[0] || target[1]!=lcf_owned[1];
		if (vote_pending) {vote_log=now.latest_log;vote_at=lcf_now_ns();}
		for (unsigned c=0;c<2;c++) {
			if (!lcf_write_request(c,target[c])) {ok=false;break;}
		}
		if (!ok) {stop_reason=8;break;}
		if (vote_pending || ticks%10==0) printf("lancelot-cpufreq-policy: at_ns=%llu span_ticks=%llu busy=%u/%u current=%u/%u request=%u/%u vote=%u hold_ns=%llu/%llu mode=load_steps_v2\n",
		    lcf_now_ns(),span,busy[0],busy[1],now.current[0],now.current[1],target[0],target[1],vote_pending,load_policy.hold_until[0],load_policy.hold_until[1]);
		if (++ticks%10==0) printf("lancelot-cpufreq: busy=%u/%u request=%u/%u current=%u/%u limit=%x/%x log=%llu\n",
		    busy[0],busy[1],lcf_owned[0],lcf_owned[1],now.current[0],now.current[1],now.limit[0],now.limit[1],now.latest_log);
		if (lcf_startup.claimed && ticks%10==0) printf("lancelot-cpufreq-feedback: current_MHz=%u/%u/%u vproc_uv=%u/%u vsram_uv=%u/%u rail_at_ns=%llu rx=%u stage=%u\n",
		    lcf_mhz[0][now.current[0]],lcf_mhz[1][now.current[1]],lcf_mhz[2][now.current[2]],
		    lcf_last_rails.vproc_uv[0],lcf_last_rails.vproc_uv[1],lcf_last_rails.vsram_uv[0],lcf_last_rails.vsram_uv[1],
		    (unsigned long long)lcf_last_rails_ns,lcf_startup.rx_count,lcf_startup.stage);
	}
	lcf_quiesce();
	if (thermal_started) lancelot_cpu_thermal_stop();
	printf("lancelot-cpufreq: controller stopped reason=%u reject=%x poison=%u stage=%u bad_bank=%u slot=%u got=%x; restore only while full profile and ownership remain valid\n",
	    stop_reason,lcf_snapshot_reject(&now),lcf_startup.poisoned,lcf_startup.stage,lcf_startup.bad_bank,lcf_startup.bad_slot,lcf_startup.bad_value);
}

static void
lcf_init(void)
{
	uint32_t on=1;thread_t thread;
	PE_parse_boot_argn("lancelot_cpufreq",&on,sizeof(on));
	if (on!=1) {
		printf("lancelot-cpufreq: disabled bootarg=%u\n", on);
		return;
	}
	if (os_atomic_xchg(&lcf_once,true,acq_rel)) return; /* no duplicate/poisoned-boot retries */
	lcf_sram=ml_io_map(LCF_SRAM_PA,LCF_SRAM_SIZE);
	lcf_pll=ml_io_map(0x1000c000,0x1000);lcf_div=ml_io_map(0x0c53a000,0x1000);
	if (!lcf_sram||!lcf_pll||!lcf_div) {
		printf("lancelot-cpufreq: map_failed SRAM=%u PLL=%u divider=%u; no votes\n",
		    lcf_sram != 0, lcf_pll != 0, lcf_div != 0);
		return;
	}
	kern_return_t error = kernel_thread_start(lcf_thread,NULL,&thread);
	if (error==KERN_SUCCESS) {
		thread_set_thread_name(thread,"lancelot cpufreq");thread_deallocate(thread);
	} else {
		printf("lancelot-cpufreq: thread_start_failed error=%d; no votes\n", error);
	}
}

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (redwood_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               0x18591000UL    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
	}
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		redwood_tl("%s\n", line);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			if (entry[i].length >= sizeof(aux) && slave->data_offset + entry[i].offset + sizeof(aux) < CMD_DB_SIZE - 0x40) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                0x18220000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (redwood_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see redwood_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as bsd/dev/redwood_goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void redwood_ddr_quiesce(void);

void
redwood_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;
	lcf_quiesce();

	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

void
redwood_cpufreq_init(void)
{
	if (lancelot_touch_platform()) {
		lcf_init();
		return; /* No Qualcomm EPSS/RPMh access on MediaTek. */
	}
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	if (PE_parse_boot_argn("membench", &enabled, sizeof(enabled)) && enabled == 1) {
		memory_bench();
	} else if (enabled == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
