/* Observation stages: no power/clock/PMIC/GPU writes, no GPU mapping.
 * mode=1 is SPM-only; lancelot_gpu_ao=1 additionally reads AO state only. */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include "lancelot_storage.h"
#include "lancelot_pwrap.h"
#include "lancelot_mali_observe.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static vm_offset_t banks[LMAO_BANK_COUNT];

static int
observe_read(__unused void *cookie, enum lmao_bank bank, uint32_t offset, uint32_t *value)
{
	const vm_offset_t pa[LMAO_BANK_COUNT] = {0x10006000,0x10000000,0x1000c000,0x10001000,0x1000d000};
	if (!value || !lmao_read_allowed(bank, offset)) return -22;
	if (!banks[bank]) banks[bank] = ml_io_map(pa[bank], 0x1000);
	if (!banks[bank]) return -12;
	__asm__ volatile("dsb sy" ::: "memory");
	*value = *(const volatile uint32_t *)(banks[bank] + offset);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
static void
observe_delay(__unused void *cookie, uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static bool
board_matches(void)
{
	DTEntry chosen; const void *prop; unsigned bytes; struct lancelot_storage s = {0};
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "lancelot-emmc", &prop, &bytes) != kSuccess ||
	    (bytes != LANCELOT_STORAGE_V1_SIZE && bytes != sizeof(s))) return false;
	memcpy(&s, prop, bytes);
	return s.magic == LANCELOT_STORAGE_MAGIC && s.size == bytes &&
	    ((s.version == 1 && bytes == LANCELOT_STORAGE_V1_SIZE) || (s.version == 2 && bytes == sizeof(s))) &&
	    s.mmio == 0x11230000 && s.partition_bytes == LANCELOT_UBUNTU_BYTES &&
	    s.partition_offset && !(s.partition_offset & 511) &&
	    s.partition_offset < (UINT64_C(1) << 41) - LANCELOT_UBUNTU_BYTES;
}

void
lancelot_mali_observe(void)
{
	uint32_t mode = 0, ao = 0;
	if (!PE_parse_boot_argn("lancelot_mali", &mode, sizeof(mode)) || !mode) return;
	PE_parse_boot_argn("lancelot_gpu_ao", &ao, sizeof(ao));
	if (mode != 1) { printf("lancelot-mali: mode=%u not admitted by observation build; no MMIO\n", mode); return; }
	if (ao > 1) { printf("lancelot-mali: unknown AO mode=%u; no MMIO\n", ao); return; }
	if (!board_matches()) { printf("lancelot-mali: board rejected; no MMIO\n"); return; }
	/* Only SPM always-on status/control registers. Reading PWR_CON does not
	 * release reset/isolation. No inference that an ACK alone permits GPU access. */
	struct lmao_io io = {.read32=observe_read, .delay_us=observe_delay};
	struct lmao_snapshot s = {0};
	int e = lmao_capture_spm(&io, &s);
	if (e) { printf("lancelot-mali: SPM snapshot failed=%d\n", e); return; }
	uint32_t *value = s.spm;
	uint32_t mask = 0x7800;
	bool stable = value[0] == value[6] && value[1] == value[7];
	bool match = (((value[0] ^ value[1]) | (value[6] ^ value[7])) & mask) == 0;
	printf("lancelot-mali: SPM-only mode=1 status=%#x/%#x final=%#x/%#x mask=%#x stable=%u doubleACK-match=%u\n",
	    value[0], value[1], value[6], value[7], mask, stable, match);
	printf("lancelot-mali: SPM PWR_CON MFG=%#x CORE0=%#x CORE1=%#x ASYNC=%#x; no MFG/GPU/PWRAP access, no DMA, no service\n",
	    value[2], value[3], value[4], value[5]);
	printf("lancelot-mali: SPM four-domain state=%s; rails/firmware access unverified, power ACK alone never permits a GPU read\n",
	    !stable || !match ? "unresolved" : (value[6] & mask) == 0 ? "all-off" :
	    (value[6] & mask) == mask ? "all-on" : "partial");
	printf("lancelot-mali: shared-WACS2 available=%u (firmware channel ownership unverified); ACK alone is NOT GPU-access proof\n",
	    lancelot_pwrap_available());
	if (!ao) return;
	e = lmao_capture_ao(&io, &s);
	if (e) { printf("lancelot-mali: AO snapshot failed=%d; no commands or power change\n", e); return; }
	printf("lancelot-mali: AO TOP mfg_cfg=%#x parent=%u gated=%u INFRA protect=%#x ACK=%#x\n",
	    s.top, s.top & 3, !!(s.top & 128), s.infra[0], s.infra[1]);
	for (unsigned i = 0; i < 3; i++)
		printf("lancelot-mali: AO PLL=%s CON0=%#x CON1=%#x PWR_CON=%#x\n",
		    i == 0 ? "MFG" : i == 1 ? "MAIN" : "UNIV", s.pll[3*i], s.pll[3*i+1], s.pll[3*i+2]);
	for (unsigned i = 0; i < 23; i++)
		printf("lancelot-mali: AO PWRAP[%#x]=%#x (read-only snapshot)\n", lmao_wrap_offsets[i], s.wrap[i]);
	printf("lancelot-mali: AO WACS2 samples=%u own-channel-active=%u shared-sync-busy=%u changed=%u no-init=%u first=%#x last=%#x; idle window is NOT ownership proof\n",
	    s.samples, s.wacs2_fsm_or_req, s.wacs2_sync_busy, s.wacs2_changes, s.wacs2_no_init, s.wacs2_first, s.wacs2_last);
	printf("lancelot-mali: AO WACS2 own-FSM-or-REQ=%u sync-busy=%u; SYNC_BUSY alone may be another PWRAP channel, NOT evidence of a WACS2 command\n",
	    s.wacs2_fsm_or_req, s.wacs2_sync_busy);
	for (unsigned i = 0; i < s.samples; i++) {
		if (i && s.wacs2_trace[i] == s.wacs2_trace[i-1]) continue;
		uint32_t v = s.wacs2_trace[i];
		printf("lancelot-mali: AO WACS2 trace=%u raw=%#x FSM=%u REQ=%u SYNC_IDLE=%u INIT=%u\n",
		    i, v, (v >> 16) & 7, (v >> 19) & 1, (v >> 20) & 1, (v >> 21) & 1);
	}
	printf("lancelot-mali: AO observation completed: no CMD/VLDCLR, no MFG/GPU access, no power/PLL/bus changes, no DMA/service\n");
}
