/* Lancelot port of oosexgang/hakios-xnu-kernel @ 620c77507a17ad23ac5a40bad900abe9ed4d7740
 * Original source: xnu/files/bsd/dev/redwood_connsys.c; original implementation retained below.
 */
/*
 * CONNSYS, lancelot's MT6768 Wi-Fi/Bluetooth/GPS subsystem (MT6631 radio):
 * docs/wifi-x6826b.md. Steps 1 and 2 read only; 3 and 4 write (the PMIC's supplies, then the power domain).
 *
 * The addresses are the ones Android's device tree gives MediaTek's driver for this SoC
 * (consys@18002000, "mediatek,mt6768-consys", printed by mtkloader/mtkconnsys.h), and what is read
 * is what that driver reads (common_main/platform/mt6768.c and include/mt6768.h in
 * MiCode/MTK_kernel_modules, merlin-r-oss):
 *
 *   SPM (0x10006000)     +0x32C  TOP1_PWR_CTRL  bit 0 reset released, 1 isolation, 2 power on, 3 power on (second),
 *                                               4 clock
 *                        +0x180  PWR_STATUS     bit 1 CONNSYS on (the CCF power acknowledgement)
 *                        +0x184  PWR_STATUS_2ND the same, from the other half of the power switch
 *   MCU_TOP_MISC_OFF (0x180B1000) +0x10  CONSYS_IP_VER: 0x10020501 once CONNSYS runs; +0x1C its configuration
 *   MCU (0x18002000)     +0x00  hardware version, +0x04 firmware (ROM) version
 *
 * The CONNSYS registers (the last two blocks) are read only when SPM says both power-status acknowledgements agree:
 * a read of a powered-off block of this kind can hang the bus. Android powers CONNSYS on at its own
 * start and LK does not, so after a boot of XNU straight from LK it is expected to be off.
 *
 * Boot-arg connsys_step=1; 2 also reads its supplies, 3 turns them on, 4 powers CONNSYS up, 5 puts its firmware in and lets its CPU go, 6 also
 * listens on BTIF, 7 asks WMT for the chip ID through it, 8 puts STP into its full mode (below); 9 reads the MCU's
 * patch and says what would be sent, 10 sends it the PDA way and resets WMT before the full mode, as Android does
 * on this chip (connsys_pda_patch), 11 sends what Android's WMT does after the full mode (connsys_sw_init); 12 and on
 * are the Wi-Fi's own (redwood_mtkwifi.c). What it finds is said once and kept for the end of the log.
 */

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include "lancelot_wifi_internal.h"
#define XNULOADER_NO_LIBC 1
#include "lancelot_sha256.h"
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <kern/kalloc.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* (osfmk/arm64/machine_routines.c: a window kmem_free can take back, out of the kernel map and not the 8 MB I/O map) */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void lancelot_wifi_log(const char *format, ...) __printflike(1, 2);
extern void delay(int usec);

/* The platform retains one mapping per bank/pool until reboot. All aliases of
 * EMI and BTIF/WLAN buffers have the same normal-uncached memory attribute. */
#define ml_io_map(pa, size) ((vm_offset_t)lancelot_wifi_map((pa), (size), LW_MMIO))
#define ml_io_map_unmappable(pa, size, attr) ((vm_offset_t)lancelot_wifi_map((pa), (size), (attr)))
#define ml_io_unmap(va, size) do { (void)(va); (void)(size); } while (0)

#define SPM_PHYS                0x10006000u
#define SPM_TOP1_PWR_CTRL       0x32C
#define SPM_PWR_STATUS          0x180
#define SPM_PWR_STATUS_2ND      0x184
#define PWR_STATUS_CONN         (1u << 1)

#define MCU_TOP_MISC_OFF_PHYS   0x180B1000u
#define CONSYS_IP_VER           0x10
#define CONSYS_CONF_ID          0x1C
#define CONSYS_IP_VER_ID        0x10020501u

#define MCU_PHYS                0x18002000u
#define CONSYS_HW_ID            0x00
#define CONSYS_FW_ID            0x04

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static uint32_t
connsys_ms(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/*
 * Step 2a (connsys_step=2), reading only: CONNSYS's supplies as LK leaves them -- the PMIC MT6358's
 * LDOs VCN18, VCN28 and VCN33 (two enables: Bluetooth's and Wi-Fi's), each its enable register and its
 * analog register with the voltage selection (include/linux/mfd/mt6358/registers.h of the MT6768's
 * vendor kernel), read through the PMIC wrapper's WACS2 channel as redwood_mtkkeys.c reads the PMIC.
 * Android turns VCN18 and VCN33 on (consys_hw_vcn18_ctrl in mt6768.c) before it powers CONNSYS up.
 */
#define PWRAP_PHYS              0x1000d000u
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)

static vm_offset_t connsys_pwrap;

static bool
connsys_pmic_read(uint32_t reg, uint32_t *value)
{
    return lancelot_wifi_pmic_read(reg, value);
}

static void
connsys_power_report(void)
{
	static const struct { const char *name; uint32_t enable, analog; } ldo[] = {
		{ "VCN18", 0x1c58, 0x1e8c },
		{ "VCN28", 0x1d88, 0x1e14 },
		{ "VCN33 (BT)", 0x1d1c, 0x1e28 },
		{ "VCN33 (Wi-Fi)", 0x1d2a, 0x1e28 },
	};

	connsys_pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t enable = 0xdead, analog = 0xdead;
		bool read = connsys_pmic_read(ldo[i].enable, &enable) && connsys_pmic_read(ldo[i].analog, &analog);

		lancelot_wifi_log("connsys: step 2: %s: %s, enable register %#x = %#x, analog %#x = %#x", ldo[i].name,
		    read ? ((enable & 1) ? "ON" : "off") : "NOT READ", ldo[i].enable, enable, ldo[i].analog, analog);
	}
}

static bool
connsys_pmic_write(uint32_t reg, uint32_t value)
{
    return lancelot_wifi_pmic_update(reg, 0xffff, value);
}

static bool
connsys_pmic_update(uint32_t reg, uint32_t mask, uint32_t value)
{
    return lancelot_wifi_pmic_update(reg, mask, value);
}

/*
 * XO_WCN, the PMIC's 26 MHz buffer for CONNSYS in co-clock mode (WMT_SOC.cfg co_clock_flag=1): Android's log at boot
 * has "PMIC_CLK_BUF?_STATUS=2 1 1 2 0 0 0" -- buffer 2, XO_WCN, under software control -- and "DCXO_CW00/01/02/11/
 * 14/16/23=0x4e1d 6d55 ...", and turns it on before CONNSYS's power-on ("co clock type(1),turn on clk buf",
 * mtk_wcn_consys_hw_reg_ctrl -> clk_buf_ctrl(CLK_BUF_CONN, true)): XO_EXTBUF2_EN_M, bit 5 of DCXO_CW00. Until run
 * 428 nothing here did; the MCU dies within 5 ms of entering the patch, awake (HCLK, OSC on, SLEEP off), pc 0 -- as
 * would a patch that starts the radio's die with no clock. DCXO_CW00 is 0x788 and CW01 0x78e in the MT6358's map as
 * I read it: the bit is set only when 0x788 holds what Android printed (bit 5 aside), so that a wrong address is
 * read and said, never written. Boot-arg connsys_xo=0: left as it is.
 */
static bool
connsys_xo_wcn_on(void)
{
    uint32_t before = 0, after = 0;
    if (!connsys_pmic_read(0x788, &before) || !lancelot_wifi_pmic_xo_on() ||
        !connsys_pmic_read(0x788, &after)) return false;
    delay_for_interval(2, NSEC_PER_MSEC);
    lancelot_wifi_log("connsys: XO_WCN enabled through MT6358 SET alias, CW00 %#x -> %#x", before, after);
    return (after & 0x20u) != 0;
}

/*
 * Step 3 (connsys_step=3): the supplies on, as consys_hw_vcn18_ctrl, consys_hw_wifi/bt_vcn33_ctrl and
 * mtk_wcn_consys_hw_efuse_paldo_ctrl do it (mt6768.c, mtk_wcn_consys_hw.c) with values from the MT6358
 * regulator driver: VCN18 (fixed 1.8 V) and VCN33 to software control (their OP_EN registers = 1),
 * VCN33's voltage selection to 3.3 V (VCN33_ANA_CON0 bits 8-9 = 1; LK leaves 3, 3.5 V), its Bluetooth
 * and Wi-Fi enables, and VCN28 (fixed 2.8 V, for the efuse). Then all four read again.
 */
static struct { uint32_t reg, mask, value; } connsys_rails[] = {
    {0x1c5a, 0xffff, 0}, {0x1c58, 1, 0}, {0x1d1e, 0xffff, 0},
    {0x1e28, 0x300, 0}, {0x1d1c, 1, 0}, {0x1d2a, 1, 0},
    {0x1d8a, 2, 0}, {0x1d90, 2, 0}, {0x1d88, 1, 0}, {0x788, 0x20, 0},
};
static bool connsys_rails_saved, connsys_pins_saved;
static uint32_t connsys_pin3e0, connsys_pin3f0, connsys_drive;

static bool
connsys_save_rails(void)
{
    if (connsys_rails_saved) return true;
    for (unsigned i = 0; i < sizeof(connsys_rails) / sizeof(connsys_rails[0]); i++)
        if (!connsys_pmic_read(connsys_rails[i].reg, &connsys_rails[i].value)) return false;
    connsys_rails_saved = true;
    return true;
}

static void
connsys_restore_rails(void)
{
    if (!connsys_rails_saved || lancelot_wifi_stopping()) return;
    for (unsigned i = sizeof(connsys_rails) / sizeof(connsys_rails[0]); i > 0; i--) {
        if (!connsys_pmic_update(connsys_rails[i - 1].reg, connsys_rails[i - 1].mask, connsys_rails[i - 1].value)) {
            lancelot_wifi_log("connsys: rail restore could not complete at %#x", connsys_rails[i - 1].reg);
            return;
        }
    }
    connsys_rails_saved = false;
}

static bool
connsys_supplies_on(void)
{
    if (!connsys_save_rails()) return false;
	bool ok = connsys_pmic_write(0x1c5a, 1) &&              /* LDO_VCN18_OP_EN: SW */
	    connsys_pmic_update(0x1c58, 1, 1) &&                /* LDO_VCN18_CON0: EN */
	    connsys_pmic_write(0x1d1e, 1) &&                    /* LDO_VCN33_OP_EN: SW */
	    connsys_pmic_update(0x1e28, 0x300, 0x100) &&        /* VCN33_ANA_CON0: VOSEL 3.3 V */
	    connsys_pmic_update(0x1d1c, 1, 1) &&                /* LDO_VCN33_CON0_0: EN (Bluetooth's) */
	    connsys_pmic_update(0x1d8a, 2, 0) &&
	    connsys_pmic_update(0x1d90, 2, 0) &&
	    connsys_pmic_update(0x1d88, 1, 1);                  /* LDO_VCN28_CON0: EN */

	delay(1000);
	lancelot_wifi_log("connsys: step 3: VCN18, VCN33 (3.3 V) and VCN28 turned on: %s", ok ? "written" : "THE WRAPPER DID NOT ANSWER");
	connsys_power_report();
	return ok && connsys_xo_wcn_on();
}

/*
 * Step 4 (connsys_step=4): CONNSYS powered up, as consys_set_if_pinmux, consys_hw_reset_bit_set and the
 * hand-made branch of consys_hw_power_ctrl do it (mt6768.c; Android goes through the clock framework's
 * clk_scp_conn_main, which does the same), every wait bounded and said instead of endless: the pins to
 * the radio, the CPU held in reset, SPM's key, the on-domain's two parts powered (each waiting for its
 * ack), the bus clock on, isolation off, the off-domain's acks, the software reset let go, the bus
 * protections (AP to CONNSYS, CONNSYS to AP) taken off, SPM's apsrc/ddr settings; then CONNSYS's IP
 * version polled -- 0x10020501 is the MT6768's and says it lives. The clock buffer of co-clock mode
 * (WMT_SOC.cfg: co_clock_flag=1, Android's clk_buf driver) is not done yet.
 */
static bool
connsys_wait(vm_offset_t base, uint32_t offset, uint32_t bit, const char *what)
{
	for (unsigned n = 0; n < 10000; n++) {
		if (rd(base, offset) & bit) {
			return true;
		}
		delay(10);
	}
	lancelot_wifi_log("connsys: step 4 [%u ms]: %s: no ack after 100 ms (%#x = %#x)", connsys_ms(), what, offset, rd(base, offset));
	return false;
}

static vm_offset_t connsys_spm, connsys_all;
static bool connsys_owned, connsys_boot_running;

static void
connsys_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool
connsys_wait_mask(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t wanted)
{
    for (unsigned i = 0; i < 10000; i++) {
        if ((rd(base, offset) & mask) == wanted) return true;
        delay(10);
    }
    return false;
}

/* MT6768 clk-mt6768-pg.c: the real CCF acknowledgement is bit 1 in both
 * status registers. The legacy TOP2 bit 30/31 guess is not a power contract. */
bool
lancelot_connsys_powered(void)
{
    return connsys_spm && (rd(connsys_spm, SPM_PWR_STATUS) & rd(connsys_spm, SPM_PWR_STATUS_2ND) & 2);
}

static bool
connsys_power_off(void)
{
    vm_offset_t spm = connsys_spm, infra = ml_io_map(0x10001000, 0x1000);
    vm_offset_t rgu = ml_io_map(0x10007000, 0x1000);
    if (!spm || !infra || !rgu) return false;
    connsys_write(rgu, 0x18, rd(rgu, 0x18) | (1u << 12) | (0x88u << 24));
    connsys_write(infra, 0x2a0, 1u << 13);
    if (!connsys_wait_mask(infra, 0x228, 1u << 13, 1u << 13)) return false;
    connsys_write(infra, 0x2a8, 1u << 18);
    if (!connsys_wait_mask(infra, 0x258, 1u << 18, 1u << 18)) return false;
    connsys_write(infra, 0x2a0, (1u << 14) | (1u << 16));
    if (!connsys_wait_mask(infra, 0x228, (1u << 14) | (1u << 16), (1u << 14) | (1u << 16))) return false;
    connsys_write(spm, 0, (rd(spm, 0) & 0xffff) | 0x0b160001);
    uint32_t p = rd(spm, SPM_TOP1_PWR_CTRL);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p |= 2);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p |= 16);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p &= ~1u);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p &= ~4u);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p &= ~8u);
    bool off = connsys_wait_mask(spm, SPM_PWR_STATUS, 2, 0) &&
        connsys_wait_mask(spm, SPM_PWR_STATUS_2ND, 2, 0);
    if (off) connsys_owned = false;
    return off;
}

static bool
connsys_power_on(vm_offset_t spm)
{
    vm_offset_t pinmux = ml_io_map(0x10005000, 0x1000), rgu = ml_io_map(0x10007000, 0x1000);
    vm_offset_t infra = ml_io_map(0x10001000, 0x1000), drive = ml_io_map(0x10002000, 0x1000);
    vm_offset_t misc = ml_io_map(MCU_TOP_MISC_OFF_PHYS, 0x1000);
    if (!pinmux || !rgu || !infra || !drive || !misc || lancelot_wifi_stopping()) return false;
    if (!connsys_pins_saved) {
        connsys_pin3e0 = rd(pinmux, 0x3e0);
        connsys_pin3f0 = rd(pinmux, 0x3f0);
        connsys_drive = rd(drive, 0xc00);
        connsys_pins_saved = true;
    }
    connsys_write(pinmux, 0x3f0, (rd(pinmux, 0x3f0) & 0xffffff88u) | 0x11u);
    connsys_write(pinmux, 0x3e0, (rd(pinmux, 0x3e0) & 0x8888888fu) | 0x11111110u);
    connsys_write(drive, 0xc00, (rd(drive, 0xc00) & 0xf81c0fffu) | 0x01209000u);
    delay(150);
    connsys_write(rgu, 0x18, rd(rgu, 0x18) | (1u << 12) | (0x88u << 24));
    connsys_write(spm, 0, (rd(spm, 0) & 0xffffu) | 0x0b160001u);
    uint32_t p = rd(spm, SPM_TOP1_PWR_CTRL);
    connsys_owned = true;
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p |= 4);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p |= 8);
    if (!connsys_wait_mask(spm, SPM_PWR_STATUS, 2, 2) ||
        !connsys_wait_mask(spm, SPM_PWR_STATUS_2ND, 2, 2)) return false;
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p &= ~16u);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p &= ~2u);
    connsys_write(spm, SPM_TOP1_PWR_CTRL, p |= 1);
    connsys_write(infra, 0x2a4, (1u << 14) | (1u << 16));
    connsys_write(infra, 0x2a4, 1u << 13);
    connsys_write(infra, 0x2ac, 1u << 18);
    delay(10);
    uint32_t version = 0;
    for (unsigned n = 0; n < 10; n++) {
        version = rd(misc, CONSYS_IP_VER);
        if (version == CONSYS_IP_VER_ID) break;
        delay_for_interval(20, NSEC_PER_MSEC);
    }
    if (version != CONSYS_IP_VER_ID) {
        lancelot_wifi_log("connsys: unexpected IP version %#x after power-on", version);
        return false;
    }
    vm_offset_t pdma = ml_io_map(0x18004000, 0x1000);
    connsys_write(pdma, 0x154, rd(pdma, 0x154) & ~2u);
    lancelot_wifi_log("connsys: power and pin configuration ready, IP=%#x", version);
    return true;
}

/*
 * Step 5 (connsys_step=5): the firmware into CONNSYS's memory, as mtk_wcn_soc_rom_patch_dwn
 * (core/wmt_ic_soc.c) does it, and its CPU let go. Each of the phone's soc1_0_*_hdr.bin files
 * (/vendor/firmware, in the system image as /fw/connsys: userland/build.sh) is a 48-byte struct
 * wmt_rom_patch (core/include/wmt_core.h) -- whose u4PatchAddr is where CONNSYS sees it, 0xf0000000 being
 * the start of its memory (CONSYS_EMI_FW_PHY_BASE against CONSYS_EMI_AP_PHY_BASE, include/mt6768.h) --
 * and then its body, copied there: the MCU's RAM code at 0, its ROM patch at 0x1c000, Bluetooth's at
 * 0x80000, Wi-Fi's at 0x140000. The memory is Android's 4 MB consys-reserve-memory; here at 0xbf000000
 * (16 MB aligned, as INFRA's remapping register takes it: bits 24 and up), clear of the loader's and the
 * sensor hub's. Then INFRA+0x380 told where it is and +0x388 the peripheral window
 * (consys_emi_set_remapping_reg), the CPU's software reset let go (consys_hw_reset_bit_set), and
 * MCU+0x600 polled for 0x1D1E -- what Android waits for. In a thread: the files are there only once
 * the root is mounted.
 */
/* kern.redwood.connsys: how step 5 ended, for a job to read (the kept lines run out on a long run) */
static char connsys_said[1000] = "not run";
SYSCTL_NODE(_kern, OID_AUTO, lancelot_wifi, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "Lancelot WLAN");
SYSCTL_STRING(_kern_lancelot_wifi, OID_AUTO, connsys, CTLFLAG_RD | CTLFLAG_LOCKED, connsys_said, sizeof(connsys_said), "");

#define CONNSYS_EMI_PHYS        LW_EMI_PHYS
#define CONNSYS_EMI_SIZE        LW_EMI_SIZE

#include "lancelot_wifi_firmware.inc"

uint8_t *
lancelot_wifi_file(const char *name, uint32_t *size)
{
	return connsys_file(name, size);
}

extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Step 6 (connsys_step=6): BTIF, the bridge WMT talks to CONNSYS through (btif@1100c000 in the device
 * tree, run 313): its clock on (infracfg's IFR2 gate, bit 31: CLR at +0x84 -- clk-mt6768.c, ifr_btif)
 * and the hardware set up as hal_btif_hw_init does it (drivers/misc/mediatek/btif/common/btif_plat.c):
 * normal mode, the new handshake, both FIFOs cleared, no DMA, auto-reset -- before CONNSYS is powered,
 * as Android does it; once CONNSYS runs, whatever it sends is read for a second (PIO: LSR bit 0, RBR).
 */
#define BTIF_PHYS               0x1100c000u
#define BTIF_TX_DMA_PHYS        0x11000b80u
/* (in the Wi-Fi's DMA memory since run 441: at CONNSYS's +0x200000, until then, its firmware's last region goes --
 * WIFI_RAM_CODE's 0xf01fe000-0xf0221570, redwood_mtkwifi.c) */
#define BTIF_TX_VFF_PHYS        LW_DMA_PHYS
#define BTIF_TX_VFF_SIZE        8192u

static vm_offset_t connsys_btif;
static vm_offset_t connsys_dma_tx;
static vm_offset_t connsys_dma_vff;
static uint32_t connsys_dma_wpt;
static uint32_t connsys_dma_wrap;
static uint32_t connsys_dma = 1;

static void
connsys_btif_dma_init(void)
{
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000);

	*(volatile uint32_t *)(infra + 0x8c) = 1u << 18;        /* ifr_ap_dma ungated */
	delay(10);
	if (connsys_dma_tx == 0) {
		connsys_dma_tx = ml_io_map(BTIF_TX_DMA_PHYS, 0x1000);
	}
	if (connsys_dma_vff == 0) {
		connsys_dma_vff = ml_io_map(BTIF_TX_VFF_PHYS, BTIF_TX_VFF_SIZE);
	}
	/* Warm reset */
	*(volatile uint32_t *)(connsys_dma_tx + 0x0c) |= 1;
	for (unsigned i = 0; i < 100 && (*(volatile uint32_t *)(connsys_dma_tx + 0x08) & 1); i++) {
		delay(10);
	}
	if (*(volatile uint32_t *)(connsys_dma_tx + 0x08) & 1) {
		*(volatile uint32_t *)(connsys_dma_tx + 0x0c) |= 2;    /* hard reset */
		delay(10);
		*(volatile uint32_t *)(connsys_dma_tx + 0x0c) &= ~2;
	}
	*(volatile uint32_t *)(connsys_dma_tx + 0x1c) = BTIF_TX_VFF_PHYS;
	*(volatile uint32_t *)(connsys_dma_tx + 0x54) = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x24) = BTIF_TX_VFF_SIZE;
	*(volatile uint32_t *)(connsys_dma_tx + 0x2c) = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x30) = 0;
	connsys_dma_wpt = 0;
	connsys_dma_wrap = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x28) = BTIF_TX_VFF_SIZE - 7;  /* thre = 8185 */
	*(volatile uint32_t *)(connsys_dma_tx + 0x00) = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x04) = 0;
	lancelot_wifi_log("connsys: step 6 [%u ms]: AP DMA TX ready: EN %#x, WPT %#x, RPT %#x, LEFT %u",
	    connsys_ms(), rd(connsys_dma_tx, 0x08), rd(connsys_dma_tx, 0x2c), rd(connsys_dma_tx, 0x30), rd(connsys_dma_tx, 0x40));
}

static void
connsys_dma_copy(vm_offset_t dst, const uint8_t *src, unsigned len)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;

	for (unsigned i = 0; i < len; i++) {
		d[i] = src[i];
	}
}

static bool
connsys_btif_dma_write(const uint8_t *bytes, unsigned length)
{
	unsigned at = 0, stuck = 0;

	/* Wait for any in-flight flush */
	for (unsigned t = 0; t < 50000 && (rd(connsys_dma_tx, 0x14) & 1); t++) {
		delay(10);
	}
	if ((rd(connsys_dma_tx, 0x14) & 1) || lancelot_wifi_stopping()) return false;
	while (at < length && !lancelot_wifi_stopping()) {
		unsigned chunk = length - at;
		unsigned left = rd(connsys_dma_tx, 0x40);

		if (left == 0) {
			unsigned t;
			for (t = 0; t < 50000 && rd(connsys_dma_tx, 0x40) == 0; t++) {
				delay(10);
			}
			if (t >= 50000) {
				stuck = 1;
				break;
			}
			left = rd(connsys_dma_tx, 0x40);
		}
		if (left > BTIF_TX_VFF_SIZE) return false;
		if (chunk > left) {
			chunk = left;
		}
		if (connsys_dma_wpt + chunk >= BTIF_TX_VFF_SIZE) {
			unsigned tail = BTIF_TX_VFF_SIZE - connsys_dma_wpt;
			connsys_dma_copy(connsys_dma_vff + connsys_dma_wpt, bytes + at, tail);
			connsys_dma_copy(connsys_dma_vff, bytes + at + tail, chunk - tail);
			connsys_dma_wpt = connsys_dma_wpt + chunk - BTIF_TX_VFF_SIZE;
			connsys_dma_wrap ^= 0x10000;
		} else {
			connsys_dma_copy(connsys_dma_vff + connsys_dma_wpt, bytes + at, chunk);
			connsys_dma_wpt += chunk;
		}
		at += chunk;
		__asm__ volatile ("dsb sy" ::: "memory");

		*(volatile uint32_t *)(connsys_dma_tx + 0x08) |= 1;             /* enable DMA */
		*(volatile uint32_t *)(connsys_dma_tx + 0x2c) = connsys_dma_wpt | connsys_dma_wrap;

		uint32_t valid = rd(connsys_dma_tx, 0x3c);
		if (valid > 0 && valid < 8) {
			*(volatile uint32_t *)(connsys_dma_tx + 0x14) |= 1;     /* DMA_FLUSH */
		}
	}
	for (unsigned t = 0; !stuck && t < 50000 && rd(connsys_dma_tx, 0x3c) != 0; t++) {
		uint32_t valid = rd(connsys_dma_tx, 0x3c);
		if (valid > 0 && valid < 8) {
			*(volatile uint32_t *)(connsys_dma_tx + 0x14) |= 1;
		}
		delay(10);
	}
	for (unsigned t = 0; !stuck && t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
		delay(10);
	}
	stuck |= at != length || rd(connsys_dma_tx, 0x3c) != 0 || !(rd(connsys_btif, 0x14) & (1u << 6));
	if (stuck) {
		lancelot_wifi_log("connsys: BTIF DMA TX timeout at %u/%u (LSR %#x, DMA_EN %#x, VALID %u, LEFT %u)",
		    at, length, rd(connsys_btif, 0x14), rd(connsys_dma_tx, 0x08),
		    rd(connsys_dma_tx, 0x3c), rd(connsys_dma_tx, 0x40));
	}
	return !stuck && !lancelot_wifi_stopping();
}
static void
connsys_btif_init(void)
{
	static bool asked;

	if (!asked) {
		asked = true;
		(void)PE_parse_boot_argn("connsys_dma", &connsys_dma, sizeof(connsys_dma));
	}
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000);

	*(volatile uint32_t *)(infra + 0x84) = 1u << 31;                /* ifr_btif ungated */
	delay(10);
	connsys_btif = ml_io_map(BTIF_PHYS, 0x1000);
	*(volatile uint32_t *)(connsys_btif + 0x0c) = 0;                /* FAKELCR: normal mode */
	*(volatile uint32_t *)(connsys_btif + 0x6c) |= 1;               /* HANDSHAKE: the new one */
	*(volatile uint32_t *)(connsys_btif + 0x60) = 0x18;             /* TRI_LVL: TX 8 (16/2), RX 1 */
	*(volatile uint32_t *)(connsys_btif + 0x08) = 1u << 1;          /* FIFOCTRL: clear Rx */
	*(volatile uint32_t *)(connsys_btif + 0x08) = 0;
	*(volatile uint32_t *)(connsys_btif + 0x08) = 1u << 2;          /* ... and Tx */
	*(volatile uint32_t *)(connsys_btif + 0x08) = 0;
	if (connsys_dma) {
		connsys_btif_dma_init();
		*(volatile uint32_t *)(connsys_btif + 0x4c) = (1u << 2) | (1u << 1); /* DMA_EN: TX DMA + auto-reset */
	} else {
		*(volatile uint32_t *)(connsys_btif + 0x4c) = 1u << 2;          /* DMA_EN: no DMA, auto-reset */
	}
	*(volatile uint32_t *)(connsys_btif + 0x04) = 0;                /* IER: none (polled) */
	lancelot_wifi_log("connsys: step 6 [%u ms]: BTIF on (%s): clock status %#x, LSR %#x",
	    connsys_ms(), connsys_dma ? "DMA TX" : "PIO", rd(infra, 0x90), rd(connsys_btif, 0x14));
}

static uint32_t connsys_step;

/*
 * The build time of each image in CONNSYS's EMI, where Android puts it (platform/mt6768.c:1058-1065,
 * CONSYS_EMI_*_BUILDTIME_OFFSET of mt6768.h): ram_bt 0x68300, ram_wifi 0x68310, ram_mcu 0x68320 as it copies the
 * RAM images in (wmt_ic_soc.c:3615-3633), the MCU's patch 0x68330 after the patch and before the WMT reset
 * (wmt_ic_soc.c:3285) -- the first 16 bytes of the file's header, the 16th made 0. "For debugging", MediaTek says;
 * runs 356-379 went without it, and the WMT reset after the patch was never answered.
 */
static void
connsys_build_time(const uint8_t *file, uint32_t offset)
{
	static vm_offset_t times;
	static uint32_t wanted = 1, asked;
	uint8_t text[16];

	if (!asked) {
		asked = 1;
		(void)PE_parse_boot_argn("connsys_times", &wanted, sizeof(wanted));    /* connsys_times=0: none written */
	}
	if (wanted == 0) {
		return;
	}
	if (times == 0) {
		times = ml_io_map(CONNSYS_EMI_PHYS + 0x68000u, 0x1000);
	}
	memcpy(text, file, 15);
	text[15] = 0;
	for (unsigned i = 0; i < 16; i += 4) {
		uint32_t word;

		memcpy(&word, text + i, 4);
		*(volatile uint32_t *)(times + (offset - 0x68000u) + i) = word;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
connsys_btif_listen(const char *what)
{
	char line[200];
	size_t used = 0;
	unsigned got = 0;

	if (connsys_btif == 0) {
		return;
	}
	used = (size_t)snprintf(line, sizeof(line), "%s:", what);
	for (unsigned t = 0; t < 1000 && got < 48; t++) {
		while ((rd(connsys_btif, 0x14) & 1) && got < 48) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x", rd(connsys_btif, 0x0) & 0xff);
			got++;
		}
		delay(1000);
	}
	if (got == 0) {
		snprintf(line + used, sizeof(line) - used, " nothing (LSR %#x)", rd(connsys_btif, 0x14));
	}
	lancelot_wifi_log("connsys: [%u ms] %s", connsys_ms(), line);
	strlcat(connsys_said, " | ", sizeof(connsys_said));
	strlcat(connsys_said, line, sizeof(connsys_said));
}

/*
 * Step 7 (connsys_step=7): the first thing WMT asks CONNSYS (wmt_core_hw_check): its chip ID, a register
 * read of 0x80000008 (GEN_HCR) -- WMT_SET_REG_CMD with op 2. Before full mode STP is in its "mandatory"
 * mode on BTIF (wmt_core_stp_init: MTKSTP_BTIF_MAND_MODE): a 4-byte header -- 0x80 + seq << 3, WMT's task
 * (4) << 4 + the length's high bits, its low byte, 0 -- the payload and a CRC of 00 00 (stp_core.c,
 * mtk_wcn_stp_send_data, the mand branch). CONNSYS is woken first as hal_btif_raise_wak_sig does it
 * (BTIF_WAK +0x64 low, ~150 us, high); bytes go to THR 16 at a time, each time the FIFO is empty (LSR bit 6).
 */
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

static bool
connsys_btif_write(const uint8_t *bytes, unsigned length)
{
	if (connsys_dma && connsys_dma_tx != 0) {
		return connsys_btif_dma_write(bytes, length);
	}
	/*
	 * A frame goes out in bursts of 16, the FIFO emptied in between; taken away from the CPU between two (a busy
	 * start-up: runs 381-385 lost the first frame every time, and their user space came up 3-9 s later than in
	 * 373-380), the ROM would see a frame with a gap in it. Android's AP DMA sends a frame without one.
	 * Boot-arg connsys_irqs_off=1 sends each with interrupts off; run 387 did, and the first frame was lost as in
	 * 381-385 -- so it is off by default.
	 */
	static uint32_t quiet = 0, asked;
	boolean_t was = TRUE;

	if (!asked) {
		asked = 1;
		(void)PE_parse_boot_argn("connsys_irqs_off", &quiet, sizeof(quiet));
	}
	if (quiet) {
		was = ml_set_interrupts_enabled(FALSE);
	}
	unsigned at = 0, stuck = 0;

	while (at < length && !lancelot_wifi_stopping()) {
		unsigned t;
		for (t = 0; t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
			delay(10);
		}
		if (t >= 50000) {
			stuck = 1;
			break;
		}
		for (unsigned n = 0; n < 16 && at < length; n++, at++) {
			*(volatile uint8_t *)(connsys_btif + 0x0) = bytes[at];
		}
	}
	for (unsigned t = 0; !stuck && t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
		delay(10);
	}
	if (quiet) {
		(void)ml_set_interrupts_enabled(was);
	}
	if (stuck) {
		lancelot_wifi_log("connsys: BTIF write timeout at %u/%u (LSR %#x)", at, length, rd(connsys_btif, 0x14));
	}
	return at == length && (rd(connsys_btif, 0x14) & (1u << 6)) && !lancelot_wifi_stopping();
}
static void
connsys_btif_wake(void)
{
	*(volatile uint32_t *)(connsys_btif + 0x64) &= ~1u;
	delay(150);
	*(volatile uint32_t *)(connsys_btif + 0x64) |= 1u;
	delay_for_interval(20, NSEC_PER_MSEC);
}


/*
 * Step 8 (connsys_step=8): STP into its full mode, as _wmt_soc_mode_ctrl (wmt_ic_soc.c) does it: the chip's STP
 * defaults asked for (init_table_1_2) and set (init_table_4) in the mandatory mode, the host switched, and
 * the defaults asked for again (init_table_5) in full mode -- where a frame's header is 0x80 + seq << 3 + ack,
 * the task << 4 + the length's high bits, its low byte and the sum of those three; then the payload and its
 * CRC-16 (the kernel's crc16, 0xA001 reflected, from 0), low byte first. A frame with a payload is answered by
 * a header-only one (stp_send_ack). Each answer is said.
 *
 * The ack is the sequence number of the last frame taken -- "txack (=last rx seq)", stp_core.c:716; 7 before any,
 * :632 -- and a frame that is not the one expected is dropped and the last ack sent again (:1338, :1434). Until run
 * 438 ours was that number + 1, an ack CONNSYS took for none of its frames: with the window full it said 7f 7f 7f 7f
 * (its resync, :708) and sent its first frame of the full mode again, in the middle of the RF calibration.
 */
static bool connsys_stp_full;
static uint8_t connsys_stp_seq, connsys_stp_ack, connsys_stp_expect;
static vm_offset_t connsys_hif;         /* CONN_HIF_ON (0x18007000): CPUPCR at +0x104, the MCU's program counter */
static vm_offset_t connsys_mcu;         /* MCU (0x18002000): mapped once for register dumps */
static vm_offset_t connsys_rgu;         /* RGU (0x10007000): software reset at +0x18 */
static vm_offset_t connsys_wbg;         /* WBG (0x180B3000): AFE PLL at +0x34 */
static uint8_t *connsys_patch_file;
static uint32_t connsys_patch_size;
static bool connsys_chip_id_done;
static unsigned connsys_doing = 7;     /* the step a WMT exchange is said under */

static bool
connsys_mcu_restart(void)
{
	uint32_t version = 0;

	if (connsys_rgu == 0 || connsys_mcu == 0) {
		return false;
	}
	lancelot_wifi_log("connsys: [%u ms] MCU restart via RGU reset...", connsys_ms());
	*(volatile uint32_t *)(connsys_rgu + 0x18) = (rd(connsys_rgu, 0x18) | (1u << 12)) | (0x88u << 24);
	delay_for_interval(10, NSEC_PER_MSEC);
	if (connsys_wbg != 0) {
		*(volatile uint32_t *)(connsys_wbg + 0x34) = (rd(connsys_wbg, 0x34) & 0xffe7ffffu) | 0x80000u;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	*(volatile uint32_t *)(connsys_rgu + 0x18) = (rd(connsys_rgu, 0x18) & ~(1u << 12)) | (0x88u << 24);
	for (unsigned n = 0; n < 50 && version != 0x1D1E; n++) {
		delay_for_interval(20, NSEC_PER_MSEC);
		version = rd(connsys_mcu, 0x600);
	}
	for (unsigned n = 0; n < 100 && (connsys_hif != 0 && rd(connsys_hif, 0x104) != 0xb04); n++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	delay_for_interval(20, NSEC_PER_MSEC);
	if (connsys_dma && connsys_dma_tx != 0) {
		*(volatile uint32_t *)(connsys_dma_tx + 0x0c) |= 1;
		for (unsigned i = 0; i < 100 && (*(volatile uint32_t *)(connsys_dma_tx + 0x08) & 1); i++) {
			delay(10);
		}
		connsys_dma_wpt = 0;
		connsys_dma_wrap = 0;
		*(volatile uint32_t *)(connsys_dma_tx + 0x2c) = 0;
		*(volatile uint32_t *)(connsys_dma_tx + 0x30) = 0;
	}
	connsys_btif_init();
	lancelot_wifi_log("connsys: [%u ms] MCU restarted: 0x600=%#x, CPUPCR=%#x",
	    connsys_ms(), rd(connsys_mcu, 0x600), connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	return version == 0x1D1E;
}

static uint16_t
connsys_crc16(const uint8_t *bytes, unsigned length)
{
	uint16_t crc = 0;

	for (unsigned i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static int
connsys_btif_byte(unsigned wait_ms)
{
    uint32_t began = connsys_ms();
    for (unsigned polls = 0; polls < 100000 && !lancelot_wifi_stopping(); polls++) {
        if (rd(connsys_btif, 0x14) & 1) return (int)(rd(connsys_btif, 0) & 0xff);
        if (connsys_ms() - began >= wait_ms) return -1;
        if (polls % 10 == 9) delay_for_interval(1, NSEC_PER_MSEC);
        else delay(50);
    }
    return -1;
}

static bool
connsys_stp_send(const uint8_t *payload, unsigned length)
{
    uint8_t frame[4 + 64 + 2];
    size_t bytes = lw_stp_encode(frame, sizeof(frame), payload, length,
        connsys_stp_full, connsys_stp_seq, connsys_stp_ack);
    if (!bytes || lancelot_wifi_stopping() || !connsys_btif_write(frame, (unsigned)bytes)) return false;
    if (connsys_stp_full) connsys_stp_seq = (connsys_stp_seq + 1) & 7;
    return true;
}

/* Validate the complete STP header/body before advancing its sequence or ACK.
 * A fixed transaction deadline also bounds unrelated channels and repeated ACKs. */
static int
connsys_stp_receive(uint8_t *payload, unsigned room, char *heard, size_t heard_room, unsigned first_ms)
{
    uint32_t began = connsys_ms(), budget = first_ms + 250;
    (void)heard;
    (void)heard_room;
    for (unsigned frames = 0; frames < 64 && !lancelot_wifi_stopping(); frames++) {
        uint32_t elapsed = connsys_ms() - began;
        if (elapsed >= budget) return -1;
        int byte = connsys_btif_byte(budget - elapsed);
        if (byte < 0) return -1;
        if ((byte & 0xc0) != 0x80) continue;
        uint8_t h[4] = { (uint8_t)byte, 0, 0, 0 };
        for (unsigned i = 1; i < 4; i++) {
            byte = connsys_btif_byte(50);
            if (byte < 0) return -1;
            h[i] = (uint8_t)byte;
        }
        if (!lw_stp_header_valid(h, connsys_stp_full)) return -1;
        unsigned length = ((h[1] & 15u) << 8) | h[2];
        if (!length) continue;
        uint16_t crc = 0, received = 0;
        for (unsigned i = 0; i < length + 2; i++) {
            if (connsys_ms() - began >= budget) return -1;
            byte = connsys_btif_byte(50);
            if (byte < 0) return -1;
            if (i < length) {
                crc = lw_crc16_step(crc, (uint8_t)byte);
                if (i < room) payload[i] = (uint8_t)byte;
            } else received |= (uint16_t)((uint16_t)byte << (8 * (i - length)));
        }
        if (!connsys_stp_full && received != 0) return -1;
        if (connsys_stp_full) {
            if (received != crc) return -1;
            uint8_t seq = (h[0] >> 3) & 7;
            bool duplicate = seq == ((connsys_stp_expect + 7) & 7);
            if (seq != connsys_stp_expect && !duplicate) return -1;
            if (!duplicate) {
                connsys_stp_ack = seq;
                connsys_stp_expect = (seq + 1) & 7;
            }
            uint8_t ack[4] = { (uint8_t)(0x80 | connsys_stp_ack), 0, 0, (uint8_t)(0x80 | connsys_stp_ack) };
            if (!connsys_btif_write(ack, sizeof(ack))) return -1;
            if (duplicate) continue;
        }
        if (((h[1] >> 4) & 7) != 4) continue;
        return length <= room ? (int)length : -1;
    }
    return -1;
}

static void
connsys_btif_dump_state(const char *tag)
{
	uint32_t lsr, iir, ier, fakelcr, sleep_en, dma_en, tri_lvl, wak, handshake;
	uint32_t cpupcr = 0, mcu600 = 0, mcu16c = 0;
	vm_offset_t mcu = ml_io_map(MCU_PHYS, 0x1000);

	if (connsys_btif != 0) {
		lsr = rd(connsys_btif, 0x14);
		iir = rd(connsys_btif, 0x08);
		ier = rd(connsys_btif, 0x04);
		fakelcr = rd(connsys_btif, 0x0c);
		sleep_en = rd(connsys_btif, 0x48);
		dma_en = rd(connsys_btif, 0x4c);
		tri_lvl = rd(connsys_btif, 0x60);
		wak = rd(connsys_btif, 0x64);
		handshake = rd(connsys_btif, 0x6c);
	} else {
		lsr = iir = ier = fakelcr = sleep_en = dma_en = tri_lvl = wak = handshake = 0xdead;
	}
	if (connsys_hif != 0) {
		cpupcr = rd(connsys_hif, 0x104);
	}
	if (mcu != 0) {
		mcu600 = rd(mcu, 0x600);
		mcu16c = rd(mcu, 0x16c);
	}

	lancelot_wifi_log("connsys: BTIF [%s] [%u ms]: LSR %#x (DR=%u OE=%u PE=%u FE=%u THRE=%u TEMT=%u) IIR %#x IER %#x DMA_EN %#x SLEEP %#x TRI %#x WAK %#x HSHK %#x FLCR %#x | MCU pc %#x 600=%#x 16c=%#x",
	    tag, connsys_ms(), lsr, lsr & 1, (lsr >> 1) & 1, (lsr >> 2) & 1, (lsr >> 3) & 1, (lsr >> 5) & 1, (lsr >> 6) & 1,
	    iir, ier, dma_en, sleep_en, tri_lvl, wak, handshake, fakelcr, cpupcr, mcu600, mcu16c);
}

static unsigned connsys_answer_ms;      /* when not 0, how long the next answer may take in full mode (else 1 s) */

static bool
connsys_wmt(const char *what, const uint8_t *command, unsigned length, const uint8_t *expected, unsigned expected_length)
{
	char line[220];
	uint8_t answer[1024];
	int got = -1;
	size_t used;
	static bool woken;
	const unsigned wait_ms = connsys_stp_full ? (connsys_answer_ms != 0 ? connsys_answer_ms : 1000) : 3000;

	used = (size_t)snprintf(line, sizeof(line), "%s: (MCU pc %#x)", what, connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	/*
	 * CONNSYS is woken (BTIF_WAK) before each command and again before a resend.
	 * In run 364, connsys_btif_wake had a 1 ms delay which was too short on cold start;
	 * with 20 ms settle time and fresh FIFOs, CONNSYS enters STP receive reliably.
	 */
	/* boot-arg connsys_wake=N: 0 never (Android, which raises it only out of sleep), 1 before each command and a
	 * resend (the default), 2 before every frame (runs 356-358) */
	static uint32_t wake = 2;
	static bool asked;
	if (!asked) {
		asked = true;
		(void)PE_parse_boot_argn("connsys_wake", &wake, sizeof(wake));
	}
	for (unsigned try = 0; try < (connsys_stp_full ? 1 : 3) && !lancelot_wifi_stopping(); try++) {
		if (try > 0) {
			connsys_btif_dump_state("before retry resend");
			if (!connsys_stp_full && !connsys_chip_id_done) {
				connsys_mcu_restart();
			}
		}
		if (wake == 2 || (wake == 1 && (!woken || try > 0))) {
			connsys_btif_wake();
			woken = true;
		}
		/* (connsys_wake=3, the command only after the wakeup event, and 4, a 0x00 ahead of it: runs 373-377, gone) */
		if (!connsys_stp_send(command, length)) return false;
		got = connsys_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms);
		/*
		 * CONNSYS's power-saving events -- 02 03 02 00 00 then 01 sleep, 02 host awake, 03 wakeup (WMT_*_EVT,
		 * core/wmt_core.c:184-191) -- are not answers: Android's STP layer takes them (wmt_ctrl_evt_parser) and
		 * the answer comes after. Run 373: the WMT reset after the patch was "answered" 02 03 02 00 00 03, the
		 * wakeup BTIF_WAK asked for.
		 */
		for (unsigned more = 0; more < 4 && got == 6 && memcmp(answer, "\x02\x03\x02\x00\x00", 5) == 0 &&
		    !((unsigned)got >= expected_length && memcmp(answer, expected, expected_length) == 0); more++) {
			used = strlen(line);
			snprintf(line + used, sizeof(line) - used, " [event %02x]", answer[5]);
			got = connsys_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms);
		}
		if (got >= 0) {
			break;
		}
		used = strlen(line);
		snprintf(line + used, sizeof(line) - used, " . (pc %#x)", connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	}
	used = strlen(line);
	if (got < 0) {
		snprintf(line + used, sizeof(line) - used, " no answer (LSR %#x)", rd(connsys_btif, 0x14));
	}
	for (int i = 0; i < got && i < 16 && used + 4 < sizeof(line); i++) {
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x", answer[i]);
	}
	bool matched = got >= 0 && lw_wmt_reply_matches(command, length, answer, (unsigned)got,
	    expected, expected_length);
	if (got >= 0) {
		snprintf(line + used, sizeof(line) - used, matched ?
		    " (as expected)" : " (NOT as expected)");
	}
	lancelot_wifi_log("connsys: step %u [%u ms]: %s", connsys_doing, connsys_ms(), line);
	strlcat(connsys_said, " | ", sizeof(connsys_said));
	strlcat(connsys_said, line, sizeof(connsys_said));
	if (matched && length == 5 && command[1] == 2 && command[4] == 0x12) {
		uint16_t revision = 0;
		(void)lw_wmt_adie(answer, (unsigned)got, &revision);
		lancelot_wifi_log("connsys: A-die chip=0x6631 revision=%#x, packet and status verified", revision);
	}
	return matched && !lancelot_wifi_stopping();
}

/*
 * Steps 9 and 10 (connsys_step=9, 10): the MCU's patch, which the MT6768 takes over BTIF and not in its memory
 * (pda_dl_patch_flag = 1 in platform/mt6768.c): in STP's mandatory mode, before the full one, as
 * mtk_wcn_soc_sw_init does it for a chip with that flag -- patch, WMT reset, and only then _wmt_soc_mode_ctrl.
 * The options of this chip (wmt_core_hw_check, "options = 3180da8" in Android's log) have no DLM table and no
 * MCU clock tables around it, and OPT_PATCH_CHECKSUM: the patch is the file less its 28-byte header (WMT_PATCH)
 * and 2 bytes of checksum, 25680 bytes of soc1_0_patch_mcu_1a_1_hdr.bin -- the "frag(26, 680)" Android logs.
 *
 * Where it goes is what Android's wmt_launcher gives the driver (WMT_IOCTL_SET_PATCH_INFO, addRess[4]): the
 * phone's /vendor/bin/wmt_launcher (sha256 39772aec...) reads 16 bytes, seeks to 22, reads 2 (the sequence,
 * its second byte compared with the patch's number) and then the 4 at offset 24 -- 11 c0 01 00 in this file,
 * whose upper half is the "PhVer=0x0001" the driver prints of the same field. mtk_wcn_soc_pda_patch_dwn
 * then sends, all in the mandatory mode:
 *   WMT_PATCH_PDA_CFG_CMD  01 01 11 00 00, the address (4), the size (4, little-endian), a scramble key of 0,
 *                          2f 02 02 00 -- answered 02 01 01 00 00;
 *   a 12-byte header       the size + 12, little-endian, then zeros -- raw, no STP frame around it;
 *   the patch              raw, in pieces of 1000 bytes;
 * and then init_table_3: WMT reset, 01 07 01 00 04, answered 02 07 01 00 00.
 * Step 9 reads the file and says what would be sent; step 10 sends it. True when that went as it should.
 */
/*
 * What CONNSYS's firmware says of itself in its EMI: the control area at EMI+0x68000 (CONSYS_EMI_COREDUMP_OFFSET;
 * ENUM_EMI_CTRL_STATE_OFFSET in platform/include/mtk_wcn_consys_hw.h: state +0, host and chip sync +4..+0x18, print
 * buffer start/len/index +0x1c/+0x20/+0x24, interrupt status +0x28, assert flag +0x100) and the printable text of its
 * print buffer and of the paged trace (+0x400) -- what Android's coredump reads. Run 430: the MCU dies the moment the
 * ROM hands over to the RAM image; whatever it managed to write first is here.
 */
static void
connsys_firmware_said(const char *when)
{
	vm_offset_t ctl = ml_io_map_unmappable(CONNSYS_EMI_PHYS + 0x68000u, 0x4000, 0x7 /* VM_WIMG_IO */);
	char text[170];

	if (ctl == 0) {
		return;
	}
#define C(o) rd(ctl, (o))
	lancelot_wifi_log("connsys: %s: EMI ctl state %#x sync %#x/%#x %#x/%#x/%#x/%#x print %#x+%#x@%#x int %#x dump_end %#x dbglog %#x "
	    "pages %#x sleep %#x assert %#x", when, C(0x0), C(0x4), C(0x8), C(0xc), C(0x10), C(0x14), C(0x18), C(0x1c), C(0x20), C(0x24),
	    C(0x28), C(0x2c), C(0x40), C(0x44), C(0x4c), C(0x100));
	/* the paged trace's first bytes, as text */
	for (unsigned part = 0; part < 3; part++) {
		unsigned n = 0;

		for (unsigned i = 0; i < 160; i++) {
			uint8_t c = *(volatile uint8_t *)(ctl + 0x400 + part * 160 + i);

			text[n++] = c >= 0x20 && c < 0x7f ? (char)c : (c == 0 ? '_' : '.');
		}
		text[n] = 0;
		lancelot_wifi_log("connsys: %s: trace %u: %s", when, part, text);
	}
	/* and the print buffer, where the control area says it is (CONNSYS's 0xf0000000 = EMI+0) */
	uint32_t start = C(0x1c), length = C(0x20);
#undef C
	ml_io_unmap(ctl, 0x4000);
	if (start >= 0xf0000000u && start < 0xf0200000u && length != 0 && length <= 0x10000u) {
		uint32_t at = start - 0xf0000000u, page = at & ~0x3fffu;
		vm_offset_t buf = ml_io_map_unmappable(CONNSYS_EMI_PHYS + page, 0x8000, 0x7);

		if (buf != 0) {
			for (unsigned part = 0; part < 3; part++) {
				unsigned n = 0;

				for (unsigned i = 0; i < 160 && (at - page) + part * 160 + i < 0x8000; i++) {
					uint8_t c = *(volatile uint8_t *)(buf + (at - page) + part * 160 + i);

					text[n++] = c >= 0x20 && c < 0x7f ? (char)c : (c == 0 ? '_' : '.');
				}
				text[n] = 0;
				lancelot_wifi_log("connsys: %s: print %u: %s", when, part, text);
			}
			ml_io_unmap(buf, 0x8000);
		}
	}
}

static bool
connsys_pda_patch(void)
{
	static const uint8_t pda_done[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
	static const uint8_t reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
	static const uint8_t reset_done[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
	uint8_t pda[21] = {
		0x01, 0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f, 0x02, 0x02, 0x00,
	};
	uint8_t head[12] = { 0 };
	uint32_t size = connsys_patch_size, body, pieces = 0;
	uint8_t *file = connsys_patch_file;
	bool free_file = false;
	bool ok;

	connsys_doing = 9;
	if (file == NULL) {
		file = connsys_file("soc1_0_patch_mcu_1a_1_hdr.bin", &size);
		free_file = true;
	}
	if (file == NULL || size <= 28 + 2) {
		lancelot_wifi_log("connsys: step 9 [%u ms]: MCU patch file could not be verified", connsys_ms());
		if (file != NULL && free_file) {
			kfree_data(file, size);
		}
		return false;
	}
	body = size - 28 - 2;
	/*
	 * Where the patch goes: the file's u4PatchVer, 0x0001c011, without its low byte -- 0x1c000. The patch is linked to run
	 * from 0x1c000: 28 of the 30 replacement addresses of its ROM patch table (0x3fffffff, 30 ROM addresses, then at
	 * +0x108 the 30 replacements: ROM 0x5200 -> 0x1c808 ...) begin a function only if its first byte is at 0x1c000, none if
	 * it is at 0x1c010 (nds32le-elf-objdump of the file, 2 November). Sent to 0x1c011 as it is, every hook jumped into the
	 * middle of other code: the MCU's exception in ROM 0x52A2, inside the hooked 0x5200, runs 398-436. To 0x1c000, run
	 * 437: the MCU back at 0xb04, WMT reset answered, STP in its full mode. Boot-arg connsys_pda_addr=0xN: N instead.
	 */
	{
		uint32_t target = 0;

		memcpy(&target, file + 24, 4);
		target &= ~0xffu;

		memcpy(&pda[5], &target, 4);
	}
	for (unsigned i = 0; i < 4; i++) {
		pda[9 + i] = (uint8_t)(body >> (8 * i));
		head[i] = (uint8_t)((body + sizeof(head)) >> (8 * i));
	}
	lancelot_wifi_log("connsys: step 9 [%u ms]: patch %.16s (%.4s, versions %02x%02x/%02x%02x): %u bytes, %u pieces of 1000, the last %u; "
	    "to %02x %02x %02x %02x", connsys_ms(), (const char *)file, (const char *)file + 16, file[20], file[21], file[22], file[23], body,
	    (body + 999) / 1000, body - (body - 1) / 1000 * 1000, pda[5], pda[6], pda[7], pda[8]);
	if (connsys_step < 10) {
		if (free_file) {
			kfree_data(file, size);
		}
		return false;
	}
	connsys_doing = 10;
	connsys_firmware_said("before the patch");
	/*
	 * BTIF_WAK, the AP's wake-up line to CONNSYS, down before the patch and left down. connsys_btif_wake pulses it
	 * low then high and leaves it HIGH (run 428: "WAK was 0x1"); after the patch the interrupts go to the RAM image's
	 * handlers, and a wake-up held asserted is an interrupt pending at once -- run 431's exception is in the handler
	 * of interrupt 8, before the RAM image has set anything up. (Run 432: the ROM does not read through WMT_SET_REG,
	 * all zeros at 0x5280-0x52bc; the patch's target 0x1c010 keeps a word; SPM+0x6f8/0x6fc are not it.)
	 * Boot-arg connsys_wakelow=0: left as it is.
	 */
	{
		uint32_t low = 1, before = rd(connsys_btif, 0x64);

		(void)PE_parse_boot_argn("connsys_wakelow", &low, sizeof(low));
		if (low != 0) {
			*(volatile uint32_t *)(connsys_btif + 0x64) = before & ~1u;
			delay_for_interval(5, NSEC_PER_MSEC);
		}
		lancelot_wifi_log("connsys: step 10 [%u ms]: BTIF_WAK %#x -> %#x before the patch", connsys_ms(), before,
		    rd(connsys_btif, 0x64));
	}
	ok = connsys_wmt("PDA configuration", pda, sizeof(pda), pda_done, sizeof(pda_done));
	if (ok) {
		/* Stream head then 26 pieces of the patch continuously; the MCU's pc after each piece and, every 5 ms,
		 * after the last -- when it leaves the ROM's 0xb04 for the 0 of a CPU in reset (run 398) */
		if (!connsys_btif_write(head, sizeof(head))) { ok = false; goto patch_done; }
		for (uint32_t at = 0; at < body; at += 1000, pieces++) {
			if (!connsys_btif_write(file + 28 + 2 + at, body - at < 1000 ? body - at : 1000)) {
				ok = false; goto patch_done;
			}
		}
		for (unsigned t = 0; t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
			delay(10);
		}

		lancelot_wifi_log("connsys: step 10 [%u ms]: %u pieces of the patch sent, LSR %#x", connsys_ms(), pieces, rd(connsys_btif, 0x14));
		connsys_firmware_said("after the patch");
		/* (run 428: then CONN_HIF_ON 0x30001 -- HCLK, OSC on, SLEEP off: awake, not asleep -- and BTIF_WAK raised and let
		 * fall changes nothing; the pc stays 0) */
		/* (the AP's own reset of CONNSYS's CPU: TOPRGU+0x18 bit 12, connsys_mcu_restart -- set would be ours) */
		lancelot_wifi_log("connsys: step 10: TOPRGU+0x18 %#x (CONNSYS CPU reset by the AP: %s)", rd(connsys_rgu, 0x18),
		    (rd(connsys_rgu, 0x18) & (1u << 12)) ? "HELD" : "not held");
		connsys_build_time(file, 0x68330u);
		delay_for_interval(50, NSEC_PER_MSEC);
		connsys_btif_dump_state("before WMT reset");
		ok = connsys_wmt("WMT reset", reset, sizeof(reset), reset_done, sizeof(reset_done));
	}
patch_done:
	if (free_file) {
		kfree_data(file, size);
	}
	return ok;
}

/*
 * Step 11 (connsys_step=11): what mtk_wcn_soc_sw_init sends after the full mode (core/wmt_ic_soc.c), for this chip's
 * options (0x3180da8) and this phone's /vendor/firmware/WMT_SOC.cfg (coex_wmt_ant_mode=1, co_clock_flag=1,
 * bt_tssi_from_wifi=3, bt_tssi_target=3719, wifi_ant_swap_mode=0; no wifi_config, no elna_gain_p1_support), in its
 * order -- Android's dmesg of run 354 (logs/android-dmesg-354.txt, 7.266-8.349 s) agrees with each branch taken:
 *   set chip ID          01 02 05 00 11 68 67 00 00       02 02 01 00 00
 *   A-die                01 02 01 00 12                   02 02 05 00 00 + its ID (OPT_QUERY_ADIE)
 *   LTE coex, table 3    01 10 02 00 15 01                02 10 01 00 00 (coex_wmt_filter_mode 0; 5 ms before it)
 *   BT TSSI from Wi-Fi   01 02 04 00 10 03 87 0e          02 02 01 00 00 (the new op ID 0x10; 3719 = 0x0e87)
 *   RF calibration       01 14 01 00 01                   02 14 + data -- 1.08 s on Android and here (439), 3 s allowed
 *   coex adjust (3)      01 10 04 00 1e|1f|20 00 00 00    02 10 01 00 00 (its new flags; Android's values all 0)
 *   FM strap             01 05 02 00 02 02                02 05 02 00 00 02 (WMT_FM_COMM)
 *   UTC time             01 f0 09 00 02, s, us            02 f0 02 00 02 00 (opfunc_utc_time_sync, after a function on)
 * It stops where Android's does (A-die, calibration, FM strap) and goes on past the rest, which Android only logs.
 * Not sent, as Android does not for these options and this file: Wi-Fi's config, the external component, EPA, eLNA,
 * coex before the calibration, antenna swap, the oscillator type, the core-dump level. The BT and Wi-Fi PALDO it
 * raises around the calibration are #if 0 on this chip (platform/mt6768.c): step 3's VCN33 is all there is.
 * Wi-Fi is not a WMT command on a SoC: mtk_wcn_wmt_func_on(WIFI) calls the Wi-Fi driver's probe (gen4m, chips/connac
 * on AXI), which loads WIFI_RAM_CODE itself -- the next step.
 */
static void
connsys_sw_init(void)
{
	static const uint8_t chip_id[] = { 0x01, 0x02, 0x05, 0x00, 0x11, 0x68, 0x67, 0x00, 0x00 };
	static const uint8_t a_die[] = { 0x01, 0x02, 0x01, 0x00, 0x12 };
	static const uint8_t a_die_said[] = { 0x02, 0x02, 0x05, 0x00, 0x00 };
	static const uint8_t lte_coex[] = { 0x01, 0x10, 0x02, 0x00, 0x15, 0x01 };
	static uint8_t tssi[] = { 0x01, 0x02, 0x04, 0x00, 0x10, 0, 0, 0 };
	static const uint8_t calibration[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
	/* (its answer is the opcode and data -- run 439: 450 bytes, 00 01 and Bluetooth's 448 of calibration, Android's
	 * gBTCalResultSize -- and Android compares only that it is 0x14's, wmt_core_init_script_retry: "do not care") */
	static const uint8_t calibrated[] = { 0x02, 0x14 };
	static const uint8_t adjust_bt[] = { 0x01, 0x10, 0x04, 0x00, 0x1e, 0x00, 0x00, 0x00 };
	static const uint8_t adjust_opp[] = { 0x01, 0x10, 0x04, 0x00, 0x1f, 0x00, 0x00, 0x00 };
	static const uint8_t adjust_ble[] = { 0x01, 0x10, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00 };
	static const uint8_t fm_strap[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
	static const uint8_t fm_strap_said[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };
	static const uint8_t wmt_done[] = { 0x02, 0x02, 0x01, 0x00, 0x00 };
	static const uint8_t coex_done[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
	static const uint8_t utc_said[] = { 0x02, 0xf0, 0x02, 0x00, 0x02, 0x00 };
	static const struct {
		const char *what;
		const uint8_t *command, *expected;
		uint8_t length, expected_length;
		bool must;
		unsigned wait_ms;
	} script[] = {
#define S(what, c, e, must, wait) { what, c, e, sizeof(c), sizeof(e), must, wait }
		S("set chip ID 0x6768", chip_id, wmt_done, false, 0),
		S("A-die", a_die, a_die_said, true, 0),
		S("LTE coex (table 3)", lte_coex, coex_done, false, 0),
		S("BT TSSI from Wi-Fi", tssi, wmt_done, false, 0),
		S("RF calibration", calibration, calibrated, true, 3000),
		S("coex: BT control", adjust_bt, coex_done, false, 0),
		S("coex: opp time ratio", adjust_opp, coex_done, false, 0),
		S("coex: BLE scan time ratio", adjust_ble, coex_done, false, 0),
		S("FM strap (COMM)", fm_strap, fm_strap_said, true, 0),
#undef S
	};
	uint8_t utc[] = { 0x01, 0xf0, 0x09, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0 };
	clock_sec_t secs;
	clock_usec_t usecs;
	unsigned answered = 0;

	const struct lw_board *board = lancelot_wifi_board();
	if (!board) return;
	tssi[5] = board->bt_tssi_from_wifi;
	tssi[6] = (uint8_t)board->bt_tssi;
	tssi[7] = (uint8_t)(board->bt_tssi >> 8);
	connsys_doing = 11;
	for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		bool ok;

		if (script[i].command == lte_coex) {
			delay(5000);                    /* (wmt_stp_wifi_lte_coex's osal_sleep_ms(5)) */
		}
		connsys_answer_ms = script[i].wait_ms;
		ok = connsys_wmt(script[i].what, script[i].command, script[i].length, script[i].expected, script[i].expected_length);
		connsys_answer_ms = 0;
		answered += ok;
		if (ok && script[i].command == calibration && !connsys_pmic_update(0x1d88, 1, 0)) {
			lancelot_wifi_log("connsys: could not retire the calibration-only VCN28 supply");
			return;
		}
		if (!ok && script[i].must) {
			lancelot_wifi_log("connsys: step 11 [%u ms]: stopped at \"%s\", as Android would", connsys_ms(), script[i].what);
			connsys_firmware_said("step 11 stopped");
			return;
		}
	}
	clock_get_calendar_microtime(&secs, &usecs);
	for (unsigned i = 0; i < 4; i++) {
		utc[5 + i] = (uint8_t)((uint32_t)secs >> (8 * i));
		utc[9 + i] = (uint8_t)((uint32_t)usecs >> (8 * i));
	}
	answered += connsys_wmt("UTC time", utc, sizeof(utc), utc_said, sizeof(utc_said));
	lancelot_wifi_log("connsys: step 11 [%u ms]: %u of %u answered as Android's are -- WMT is up; Wi-Fi itself is its driver's (gen4m)",
	    connsys_ms(), answered, (unsigned)(sizeof(script) / sizeof(script[0])) + 1);
	connsys_firmware_said("after step 11");
	if (connsys_step >= 12) {
		if (!connsys_pmic_update(0x1d2a, 1, 1)) {
			lancelot_wifi_log("connsys: Wi-Fi VCN33 supply did not enable");
			return;
		}
		lancelot_wifi_start(connsys_step);
	}
}

static void
connsys_stp_full_mode(void)
{
	static const uint8_t query[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const uint8_t query_default[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00 };
	static const uint8_t set[] = { 0x01, 0x04, 0x05, 0x00, 0x03, 0xDB, 0x0E, 0x68, 0x01 };
	static const uint8_t set_done[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
	static const uint8_t query_full[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xDB, 0x0E, 0x68, 0x01 };

	static const uint8_t chip_id[] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const uint8_t chip_id_read[] = { 0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x80 };

	/*
	 * Can CONNSYS's CPU read its EMI (0xf0000000 = EMI+0 through the remap of step 5)? The RAM image it jumps to after
	 * the patch's WMT reset is there; runs 371-375 sent the patch whole and the reset went unanswered, the MCU staying
	 * at the ROM's 0xb04. Read with the same WMT_SET_REG_CMD as the chip ID: soc1_0_ram_mcu_1a_1_hdr.bin begins
	 * (byte 48) 46 1f 00 05, so the answer should end 46 1f 00 05.
	 */
	static const uint8_t emi_read[] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const uint8_t emi_read_done[] = { 0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x46, 0x1f, 0x00, 0x05 };

	connsys_chip_id_done = false;
	if (!connsys_wmt("WMT read 0x80000008", chip_id, sizeof(chip_id), chip_id_read, sizeof(chip_id_read))) {
		return;
	}
	connsys_chip_id_done = true;
	delay_for_interval(10, NSEC_PER_MSEC);
	if (connsys_step >= 9) {
		connsys_wmt("WMT read 0xf0000000 (EMI+0)", emi_read, sizeof(emi_read), emi_read_done, sizeof(emi_read_done));
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	/*
	 * (Runs 426, 427: CONNSYS's own writes by WMT_SET_REG, read by the AP -- EMI+0x60000 and +0x1f0000 written,
	 * +0x300000 not: it writes its memory in the first 2 MB, so no EMI MPU stands between it and the patch.)
	 */

	/* (boot-arg connsys_lpbk=N, WMT loopbacks before the patch, was here until run 431: by PIO 0 of 20 came back
	 * whole, run 392; by AP DMA 20 of 20, runs 398 and 425) */
	if (connsys_step < 8 || (connsys_step >= 9 && !connsys_pda_patch())) {
		return;
	}
	connsys_doing = 8;
	if (!connsys_wmt("STP defaults", query, sizeof(query), query_default, sizeof(query_default)) ||
	    !connsys_wmt("STP set", set, sizeof(set), set_done, sizeof(set_done))) {
		return;
	}
	connsys_stp_full = true;
	connsys_stp_seq = 0;
	connsys_stp_ack = 7;                    /* (none taken yet) */
	connsys_stp_expect = 0;
	delay(10000);
	if (connsys_wmt("full mode, STP asked again", query, sizeof(query), query_full, sizeof(query_full)) && connsys_step >= 11) {
		connsys_sw_init();
	}
}

/*
 * CONNSYS's memory zeroed, as Android has it before the first power-on (mtk_wmt_probe, "Clearing Connsys EMI ...
 * 4194304 bytes", platform/mtk_wcn_consys_hw.c) and its control and paged-dump areas again at every power-on
 * (consys_reset_emi_coredump: EMI+0x68000-0x68400, +0x70400-0x78400). Ours, at 0xbf000000, holds whatever the RAM
 * held before -- and that is where CONNSYS's firmware keeps its state, its assert flags and its print buffer
 * (EXP_APMEM_CTRL_*, platform/mt6768.c), and its data. Run 398: the patch whole (DMA), then the MCU's pc 0, as in
 * reset, and the WMT reset unanswered. Through windows the kernel can take back (512 KB at a time: the 8 MB I/O map
 * did not have 4 MB, run 308), with 64-bit stores (device memory: no DC ZVA). How many words of the control area
 * were not zero is said, to know whether that is what it was.
 */
static bool
connsys_emi_clear(void)
{
	const uint32_t chunk = 512u * 1024u;
	const uint32_t began = connsys_ms();
	unsigned dirty = 0;

	for (uint32_t at = 0; at < CONNSYS_EMI_SIZE; at += chunk) {
		vm_offset_t window = ml_io_map_unmappable(CONNSYS_EMI_PHYS + at, chunk, 0x7 /* VM_WIMG_IO */);
		volatile uint64_t *words = (volatile uint64_t *)window;

		if (window == 0) {
			lancelot_wifi_log("connsys: step 5 [%u ms]: EMI+%#x could not be mapped to clear it", connsys_ms(), at);
			return false;
		}
		if (at <= 0x68000u && 0x68400u <= at + chunk) {
			for (uint32_t i = (0x68000u - at) / 8; i < (0x68400u - at) / 8; i++) {
				dirty += words[i] != 0;
			}
		}
		for (uint32_t i = 0; i < chunk / 8; i++) {
			words[i] = 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		ml_io_unmap(window, chunk);
	}
	lancelot_wifi_log("connsys: step 5 [%u ms]: EMI cleared, 4 MB in %u ms (the control area EMI+0x68000 had %u of 128 "
	    "words not zero)", connsys_ms(), connsys_ms() - began, dirty);
	return true;
}
/*
 * The firmware's dedicated log path, as Android sets it up before the first power-on (consys_dedicated_log_path_init ->
 * connsys_dedicated_log_path_apsoc_init, common/debug_utility/connsys_debug_utility.c, with mt6768.c's
 * connsys_fw_log_parameter: EMI+0x36500, 192 KB -- MCU 16 KB, Wi-Fi and BT 64 KB, GPS 32 KB): the area zeroed (done by
 * connsys_emi_clear), at its start each ring's offset and size (connlog_set_ring_buffer_base_addr: base 64, then each
 * ring after the last + 32 reserved + 32 for alignment) and at +56 the ready pattern "EMIFWLOG" (connlog_set_ring_ready).
 * Ours had zeros there through run 433, and the firmware dies in an interrupt handler as soon as it runs from EMI.
 * Boot-arg connsys_log=0: not written.
 */
static void
connsys_log_ring(void)
{
	static const uint32_t header[8] = {
		0x40, 16 * 1024,                                        /* MCU: base 64 */
		0x40 + 16 * 1024 + 64, 64 * 1024,                       /* Wi-Fi: 0x4080 */
		0x40 + 16 * 1024 + 64 + 64 * 1024 + 64, 64 * 1024,      /* BT: 0x140c0 */
		0x40 + 16 * 1024 + 64 + 2 * (64 * 1024 + 64), 32 * 1024, /* GPS: 0x24100 */
	};
	uint32_t wanted = 1;
	vm_offset_t log;

	(void)PE_parse_boot_argn("connsys_log", &wanted, sizeof(wanted));
	/* (the kernel's pages are 16 KB: a window and its release on 16 KB, or ml_io_unmap writes half a page's PTEs and
	 * pmap panics -- run 434, "write_pte_fast: PTE write is unaligned", a 4 KB window at EMI+0x36000) */
	if (wanted == 0 || (log = ml_io_map_unmappable(CONNSYS_EMI_PHYS + 0x34000u, 0x4000, 0x7 /* VM_WIMG_IO */)) == 0) {
		return;
	}
	log += 0x2000;          /* EMI+0x36000 */
	for (unsigned i = 0; i < 8; i++) {
		*(volatile uint32_t *)(log + 0x500 + 4 * i) = header[i];
	}
	*(volatile uint32_t *)(log + 0x500 + 56) = 0x46494d45u;    /* "EMIF" */
	*(volatile uint32_t *)(log + 0x500 + 60) = 0x474f4c57u;    /* "WLOG" */
	__asm__ volatile ("dsb sy" ::: "memory");
	lancelot_wifi_log("connsys: step 5 [%u ms]: log rings at EMI+0x36500: %#x+%#x %#x+%#x %#x+%#x %#x+%#x, ready %.8s", connsys_ms(),
	    rd(log, 0x500), rd(log, 0x504), rd(log, 0x508), rd(log, 0x50c), rd(log, 0x510), rd(log, 0x514), rd(log, 0x518),
	    rd(log, 0x51c), (const char *)(log + 0x500 + 56));
	ml_io_unmap(log - 0x2000, 0x4000);
}

static void
connsys_firmware_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const files[] = {
		"soc1_0_ram_mcu_1a_1_hdr.bin", "soc1_0_patch_mcu_1a_1_hdr.bin", "soc1_0_ram_bt_1a_1_hdr.bin",
		"soc1_0_ram_wifi_1a_1_hdr.bin",
	};
	vm_offset_t emi, infra = ml_io_map(0x10001000u, 0x1000), rgu = ml_io_map(0x10007000u, 0x1000);
	vm_offset_t mcu = ml_io_map(MCU_PHYS, 0x1000), hif = ml_io_map(0x18007000u, 0x1000);
	uint32_t version = 0, copied = 0;

	connsys_hif = hif;
	connsys_mcu = mcu;
	connsys_rgu = rgu;

    /* An inherited or partially powered domain must be normalized before RAM
     * is cleared. Only always-on registers are read in a partial power state. */
    uint32_t status = rd(connsys_spm, SPM_PWR_STATUS), status2 = rd(connsys_spm, SPM_PWR_STATUS_2ND);
    if ((status | status2) & 2) {
        connsys_owned = true;
        if ((status & status2) & 2) {
            vm_offset_t pdma = ml_io_map(0x18004000, 0x1000);
            connsys_write(pdma, 0x154, rd(pdma, 0x154) | 2u);
            connsys_write(pdma, 0x204, 0);
            connsys_write(pdma, 0x208, rd(pdma, 0x208) & ~5u);
            if (!connsys_wait_mask(pdma, 0x208, 0xau, 0)) goto boot_failed;
        }
        if (!connsys_power_off()) goto boot_failed;
    }
    connsys_files_begin();
    if (lancelot_wifi_stopping() || (connsys_step >= 5 && !connsys_firmware_preflight())) goto boot_failed;
    if (lancelot_wifi_stopping() || !connsys_supplies_on()) goto boot_failed;
    connsys_owned = true;
    if (connsys_step == 3) goto boot_done;
    if (!connsys_power_on(connsys_spm)) goto boot_failed;
    if (connsys_step == 4) goto boot_done;

	/* boot-arg connsys_clear=0: CONNSYS's memory left as it was found, as until run 398 */
	{
		uint32_t clear = 1;

		(void)PE_parse_boot_argn("connsys_clear", &clear, sizeof(clear));
		if (clear != 0) {
			if (!connsys_emi_clear()) goto boot_failed;
		}
	}
	connsys_log_ring();
	/* (each file's own window: 4 MB at once was more than the kernel's I/O map had, run 308) */
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		uint32_t size = 0, address, offset;
		uint8_t *file;

		/* from step 9 the MCU's patch goes over BTIF (connsys_pda_patch), not into EMI: Android puts only the RAM
		 * images there ("[Rom Patch]" ram_bt, ram_wifi, ram_mcu in its log) */
		if (strcmp(files[i], "soc1_0_patch_mcu_1a_1_hdr.bin") == 0) {
			continue;
		}
		file = connsys_file(files[i], &size);

		if (file == NULL) {
			lancelot_wifi_log("connsys: step 5 [%u ms]: %s could not be verified", connsys_ms(), files[i]);
			goto boot_failed;
		}
		memcpy(&address, file + 0x18, 4);
		uint32_t wanted = i == 0 ? 0 : i == 2 ? 0x80000 : 0x140000;
		if (!lw_rom_image(file, size, wanted, &offset)) {
			lancelot_wifi_log("connsys: step 5 [%u ms]: %s: %u bytes at %#x do not fit", connsys_ms(), files[i], size - 48, offset);
			kfree_data(file, size);
			goto boot_failed;
		}
		emi = ml_io_map(CONNSYS_EMI_PHYS + offset, round_page(size - 48));
		for (uint32_t at = 48; at < size; at += 4) {
			uint32_t word = 0;

			memcpy(&word, file + at, size - at >= 4 ? 4 : size - at);
			*(volatile uint32_t *)(emi + (at - 48)) = word;
		}
		/* (not unmapped: ml_io_map's entries are permanent, ml_io_unmap panics on them -- run 310; the windows,
		 * ~900 KB in all, and the sensor hub's 256 KB FIFO fit the I/O map together) */
		lancelot_wifi_log("connsys: step 5 [%u ms]: %s (%.14s): %u bytes at EMI+%#x (its address %#x)", connsys_ms(), files[i], (const char *)file,
		    size - 48, offset, address);
		copied++;
		if (connsys_step >= 9) {
			connsys_build_time(file, strcmp(files[i], "soc1_0_ram_bt_1a_1_hdr.bin") == 0 ? 0x68300u :
			    strcmp(files[i], "soc1_0_ram_wifi_1a_1_hdr.bin") == 0 ? 0x68310u : 0x68320u);
		}
		kfree_data(file, size);
	}
	if (connsys_step >= 9 && connsys_patch_file == NULL) {
		connsys_patch_file = connsys_file("soc1_0_patch_mcu_1a_1_hdr.bin", &connsys_patch_size);
		if (connsys_patch_file != NULL && connsys_patch_size > 28 + 2) {
			lancelot_wifi_log("connsys: step 5 [%u ms]: patch file preloaded (%u bytes)", connsys_ms(), connsys_patch_size);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(infra + 0x380) = (rd(infra, 0x380) & 0xfffff000u) | ((CONNSYS_EMI_PHYS >> 24) & 0xfffu);
	*(volatile uint32_t *)(infra + 0x388) = (rd(infra, 0x388) & 0xf000ffffu) | (((0x10003000u >> 24) & 0xfffu) << 16);
	/*
	 * The radio PLL's setting Android makes just before it lets CONNSYS's CPU go: consys_afe_reg_setting of
	 * platform/mt6768.c (CONSYS_AFE_REG_SETTING 1 in mt6768.h, "update WPLL setting for WPLL issue@LT"),
	 * RG_WBG_PLL_03 at CONSYS_AFE_REG_BASE 0x180B3000 + 0x34, bits 19-20 to 01; called from
	 * mtk_wcn_consys_hw_reg_ctrl after acr_reg_setting (empty here) and before hw_reset_bit_set(DISABLE).
	 * Boot-arg connsys_afe=0 leaves it as it is.
	 */
	{
		uint32_t afe = 1;

		(void)PE_parse_boot_argn("connsys_afe", &afe, sizeof(afe));
		if (afe != 0) {
			vm_offset_t wbg = ml_io_map(0x180B3000u, 0x1000);
			connsys_wbg = wbg;
			uint32_t before = rd(wbg, 0x34);

			*(volatile uint32_t *)(wbg + 0x34) = (before & 0xffe7ffffu) | 0x80000u;
			__asm__ volatile ("dsb sy" ::: "memory");
			lancelot_wifi_log("connsys: step 5 [%u ms]: WBG PLL_03 (0x180b3034) %#x -> %#x", connsys_ms(), before, rd(wbg, 0x34));
		}
	}
	/*
	 * What polling_consys_chipid (platform/mt6768.c) does once the chip ID is read and the RAM images are in, before
	 * the CPU is let go -- none of which was done here until run 430, and the MCU died as soon as the ROM handed over
	 * to the RAM image in EMI after the patch (run 429: last pc 0xf000d73e, then 0):
	 *   - co-clock mode (co_clock_flag=1): MCU_TOP_MISC_ON+0x200 (CONSYS_COCLOCK_STABLE_TIME_BASE 0x180C1200) low half
	 *     0x708 -- "vcore ready stable time", "XO initial stable time", "XO bg stable time" -- and +0x204 bits 0 and
	 *     8-15 to 0x600: "clear 'source clock enable ack to XO state' mask";
	 *   - "EMI control CR setting (hw mode)": MCU+0x150 (CONSYS_SW_IRQ_OFFSET) bit 21 cleared;
	 *   - "toppose_restore_done rollback": MCU_TOP_MISC_ON+0x600 bit 23 set.
	 * Boot-arg connsys_post=0: none of it.
	 */
	{
		uint32_t post = 1;

		(void)PE_parse_boot_argn("connsys_post", &post, sizeof(post));
		if (post != 0) {
			/* (a window taken back after: one more permanent one filled the kernel's 8 MB I/O map, and another driver's
			 * later mapping panicked, "kmem_alloc ... failed with 3" -- runs 430-435; 16 KB aligned) */
			vm_offset_t misc_on_window = ml_io_map_unmappable(0x180C0000u, 0x4000, 0x7 /* VM_WIMG_IO */);
			vm_offset_t misc_on = misc_on_window + 0x1000;
			uint32_t stable = rd(misc_on, 0x200), ack = rd(misc_on, 0x204), emi = rd(mcu, 0x150), toppose = rd(misc_on, 0x600);

			*(volatile uint32_t *)(misc_on + 0x200) = (stable & 0xffff0000u) | 0x708u;
			*(volatile uint32_t *)(misc_on + 0x204) = (ack & 0xffff00feu) | 0x600u;
			*(volatile uint32_t *)(mcu + 0x150) = emi & ~(1u << 21);
			*(volatile uint32_t *)(misc_on + 0x600) = (toppose & 0xff7fffffu) | (1u << 23);
			__asm__ volatile ("dsb sy" ::: "memory");
			lancelot_wifi_log("connsys: step 5 [%u ms]: as polling_consys_chipid: XO stable time %#x -> %#x, ack mask %#x -> %#x, "
			    "EMI control (MCU+0x150) %#x -> %#x, toppose restore %#x -> %#x", connsys_ms(), stable, rd(misc_on, 0x200), ack,
			    rd(misc_on, 0x204), emi, rd(mcu, 0x150), toppose, rd(misc_on, 0x600));
			ml_io_unmap(misc_on_window, 0x4000);
		}
	}
	uint32_t ms_rel = connsys_ms();
	*(volatile uint32_t *)(rgu + 0x18) = (rd(rgu, 0x18) & ~(1u << 12)) | (0x88u << 24);     /* the CPU let go */
	for (unsigned n = 0; n < 50 && version != 0x1D1E; n++) {
		delay_for_interval(20, NSEC_PER_MSEC);
		version = rd(mcu, 0x600);
	}
	snprintf(connsys_said, sizeof(connsys_said), "step 5 [%u ms, rel at %u ms]: %u files in; remap %#x/%#x; MCU+0x600 %#x (%s), CPUPCR %#x then %#x",
	    connsys_ms(), ms_rel, copied, rd(infra, 0x380), rd(infra, 0x388), version, version == 0x1D1E ? "CONNSYS'S ROM TOOK IT" : "not 0x1D1E",
	    rd(hif, 0x104), (delay_for_interval(10, NSEC_PER_MSEC), rd(hif, 0x104)));
	lancelot_wifi_log("connsys: %s", connsys_said);
	if (version != 0x1d1e) goto boot_failed;
	for (unsigned n = 0; n < 100 && (connsys_hif != 0 && rd(connsys_hif, 0x104) != 0xb04); n++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	lancelot_wifi_log("connsys: step 5 [%u ms]: MCU startup PC %#x", connsys_ms(), connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	if (connsys_step >= 7) {
		delay_for_interval(20, NSEC_PER_MSEC);          /* Android's msleep(20) after the reset bit */
		connsys_btif_init();
		connsys_stp_full_mode();
		if (connsys_step >= 16 && !lancelot_wifi_online()) goto boot_failed;
	} else {
		connsys_btif_listen("step 6: BTIF in a second");
	}
boot_done:
    __atomic_store_n(&connsys_boot_running, false, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
    return;
boot_failed:
    lancelot_wifi_log("connsys: initialization stopped before exposing a usable radio");
    lancelot_wifi_stop();
    bool isolated = !connsys_owned || connsys_power_off();
    if (isolated && !(rd(connsys_spm, SPM_PWR_STATUS) & 2) && !(rd(connsys_spm, SPM_PWR_STATUS_2ND) & 2)) {
        if (connsys_pins_saved) {
            vm_offset_t pins = ml_io_map(0x10005000, 0x1000), drive = ml_io_map(0x10002000, 0x1000);
            connsys_write(pins, 0x3e0, (rd(pins, 0x3e0) & 0x8888888f) | (connsys_pin3e0 & 0x77777770));
            connsys_write(pins, 0x3f0, (rd(pins, 0x3f0) & 0xffffff88) | (connsys_pin3f0 & 0x77));
            connsys_write(drive, 0xc00, (rd(drive, 0xc00) & 0xf81c0fff) | (connsys_drive & ~0xf81c0fffu));
            connsys_pins_saved = false;
        }
        connsys_restore_rails();
    } else {
        lancelot_wifi_log("connsys: power isolation failed; rails and reserved memory retained");
    }
    goto boot_done;
}

void
lancelot_connsys_init(void)
{
    uint32_t step = 0, wifi = 0;
    (void)PE_parse_boot_argn("wifi", &wifi, sizeof(wifi));
    if (!PE_parse_boot_argn("connsys_step", &step, sizeof(step))) step = wifi == 1 ? 17 : 0;
    if (!step || !lancelot_touch_platform()) return;
    if (step > 17 || wifi > 1 || connsys_boot_running) {
        lancelot_wifi_log("lancelot-wifi: rejected invalid startup request");
        return;
    }
    connsys_step = step;
    if (step >= 3 && !lancelot_wifi_resources()) {
        lancelot_wifi_log("lancelot-wifi: missing verified Mu memory/board handoff");
        return;
    }
    connsys_spm = ml_io_map(SPM_PHYS, 0x1000);
    if (!connsys_spm) return;
    lancelot_wifi_log("connsys: initial power acknowledgements=%#x/%#x, request=%u",
        rd(connsys_spm, SPM_PWR_STATUS), rd(connsys_spm, SPM_PWR_STATUS_2ND), step);
    if (step == 1) return;
    if (step == 2) { connsys_power_report(); return; }
    connsys_all = ml_io_map(0x18000000, 0x100000);
    connsys_rgu = ml_io_map(0x10007000, 0x1000);
    if (!connsys_all || !connsys_rgu) return;
    if (step >= 16) lancelot_wifi_net_early();
    thread_t worker;
    __atomic_store_n(&connsys_boot_running, true, __ATOMIC_RELEASE);
    if (kernel_thread_start(connsys_firmware_thread, NULL, &worker) != KERN_SUCCESS) {
        __atomic_store_n(&connsys_boot_running, false, __ATOMIC_RELEASE);
        return;
    }
    thread_deallocate(worker);
}

/* Final return path: no allocation, mutex, scheduler wait or PMIC transaction.
 * The two static carveouts remain reserved by Mu even after a failed stop. */
void
lancelot_connsys_quiesce(void)
{
    lancelot_wifi_set_stopping();
    if (connsys_step < 3 || !connsys_spm) return;
    lancelot_wifi_quiesce();
    if (connsys_all && lancelot_connsys_powered()) {
        connsys_write(connsys_all, 0x4204, 0);
        connsys_write(connsys_all, 0x4208, rd(connsys_all, 0x4208) & ~5u);
        for (unsigned i = 0; i < 10000 && (rd(connsys_all, 0x4208) & 0xa); i++) delay(1);
    }
    if (connsys_dma_tx) {
        connsys_write(connsys_dma_tx, 0x04, 0);
        connsys_write(connsys_dma_tx, 0x10, 1);
        for (unsigned i = 0; i < 10000 && (rd(connsys_dma_tx, 0x08) & 1); i++) delay(1);
    }
    if (connsys_btif) connsys_write(connsys_btif, 0x04, 0);
    if (connsys_rgu) connsys_write(connsys_rgu, 0x18, rd(connsys_rgu, 0x18) | (1u << 12) | (0x88u << 24));
}

void
lancelot_connsys_stop(void)
{
    lancelot_wifi_set_stopping();
    for (unsigned i = 0; i < 3000 && __atomic_load_n(&connsys_boot_running, __ATOMIC_ACQUIRE); i++)
        delay_for_interval(1, NSEC_PER_MSEC);
    if (__atomic_load_n(&connsys_boot_running, __ATOMIC_ACQUIRE)) {
        lancelot_wifi_log("connsys: boot cancellation pending; final return will stop CPUs before DMA teardown");
        lancelot_wifi_revoke();
        return;
    }
    lancelot_wifi_stop();
    lancelot_connsys_quiesce();
    /* Do not access WACS2 here: GPU's established return path may already
     * have frozen that shared channel. Power gating never frees the pools. */
}

/* Ordinary return must retire the producer and both DMA engines before the
 * GPU/CPU return protocol freezes shared services. Panic uses final quiesce. */
bool
lancelot_connsys_return_ready(void)
{
    if (connsys_step < 3 || !ml_get_interrupts_enabled() || ml_at_interrupt_context()) return true;
    lancelot_connsys_stop();
    if (__atomic_load_n(&connsys_boot_running, __ATOMIC_ACQUIRE)) return false;
    if (connsys_all && lancelot_connsys_powered() && (rd(connsys_all, 0x4208) & 0xf)) return false;
    if (connsys_dma_tx && (rd(connsys_dma_tx, 0x08) & 1)) return false;
    if (connsys_rgu && !(rd(connsys_rgu, 0x18) & (1u << 12))) return false;
    return true;
}
