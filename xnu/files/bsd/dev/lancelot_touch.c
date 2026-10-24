/* Redmi 9 / lancelot NT36672A, explicit staged opt-in only.
 * lancelot_touch=1: read inherited GPIO/clock snapshot, no hardware writes.
 * lancelot_touch=2: own SPI0, read trim ID, then restore resources; no NVT reset.
 * lancelot_touch=3: hash-pinned RAM firmware boot and real report -> input queue.
 * Firmware fw/nvt_dj_fw.bin is private, never embedded in tracked source.
 * This driver starts with 8ms polled reports; EINT/GIC touch IRQ is not enabled.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include "lancelot_storage.h"
#include "lancelot_spi.h"
#include "lancelot_touch_platform.h"
#include "lancelot_nt36672_hw.h"
#include "lancelot_nt36672_protocol.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void gic_v3_spi_mask(uint32_t);
extern const uint8_t *redwood_image_file(const char *, const char *, uint64_t *, int *);

static vm_offset_t banks[4], spi_regs;
static struct lancelot_spi spi;
static struct ln_device nvt;
static struct lancelot_touch_resources resources;
static struct lancelot_nt36672_state contacts;
static bool started;

bool
lancelot_touch_platform(void)
{
	DTEntry chosen;
	const void *property;
	unsigned size;
	struct lancelot_storage offer = {0};
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "lancelot-emmc", &property, &size) != kSuccess ||
	    (size != LANCELOT_STORAGE_V1_SIZE && size != sizeof(offer))) return false;
	memcpy(&offer, property, size);
	return offer.magic == LANCELOT_STORAGE_MAGIC && offer.size == size &&
	    ((offer.version == 1 && size == LANCELOT_STORAGE_V1_SIZE) || (offer.version == 2 && size == sizeof(offer))) &&
	    offer.mmio == 0x11230000 && offer.partition_bytes == LANCELOT_UBUNTU_BYTES &&
	    offer.partition_offset && !(offer.partition_offset & 511) &&
	    offer.partition_offset < (1ULL << 41) - LANCELOT_UBUNTU_BYTES;
}

static uint64_t
touch_ticks(__unused void *ctx)
{
	return __builtin_arm_rsr64("CNTVCT_EL0");
}

static uint32_t
touch_read_bank(__unused void *ctx, unsigned bank, uint32_t offset)
{
	return *(volatile uint32_t *)(banks[bank] + offset);
}

static void
touch_write_bank(__unused void *ctx, unsigned bank, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(banks[bank] + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t
touch_spi_read(__unused void *ctx, uint32_t offset)
{
	return *(volatile uint32_t *)(spi_regs + offset);
}

static void
touch_spi_write(__unused void *ctx, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(spi_regs + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static int
touch_transfer(__unused void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	return lancelot_spi_transfer(&spi, tx, rx, length);
}

static void
touch_delay(__unused void *ctx, uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
touch_emit(__unused void *ctx, uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	/* The state machine is in physical panel units; normalize exactly once. */
	redwood_touch_post(type, finger, x * 65535u / 1080u, y * 65535u / 2340u);
}

static void
touch_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t mode = (uint32_t)(uintptr_t)parameter;
	uint64_t size = 0, frequency = __builtin_arm_rsr64("CNTFRQ_EL0");
	const uint8_t *firmware = NULL;
	struct ln_firmware plan;
	int error = 0;
	if (frequency < 1000 || frequency > 1000000000) goto done;
	/* No hardware writes if the full-touch candidate lacks its correct file. */
	if (mode == 3) {
		firmware = redwood_image_file("fw", "nvt_dj_fw.bin", &size, NULL);
		if (!firmware || ln_validate_firmware(firmware, size, &plan) != LN_OK) {
			printf("lancelot-touch: missing/invalid pinned Dijing firmware; no MMIO writes\n"); goto done;
		}
	}
	banks[LP_TOP] = ml_io_map(0x10000000, 4096);
	banks[LP_INFRA] = ml_io_map(0x10001000, 4096);
	banks[LP_GPIO] = ml_io_map(0x10005000, 4096);
	vm_offset_t iocfg = ml_io_map(0x10002000, 4096);
	if (!banks[0] || !banks[1] || !banks[2] || !iocfg) goto done;
	banks[LP_RM] = iocfg + 0x800;
	printf("lancelot-touch: stage=%u inherited clock=%#x gate=%#x mode1=%#x spi-mode=%#x dir=%#x/%#x ies=%#x\n",
	    mode, touch_read_bank(NULL, LP_TOP, 0x60), touch_read_bank(NULL, LP_INFRA, 0x94),
	    touch_read_bank(NULL, LP_GPIO, 0x300), touch_read_bank(NULL, LP_GPIO, 0x340),
	    touch_read_bank(NULL, LP_GPIO, 0), touch_read_bank(NULL, LP_GPIO, 0x10), touch_read_bank(NULL, LP_RM, 0x60));
	if (mode == 1) goto done;
	resources.read = touch_read_bank; resources.write = touch_write_bank;
	resources.ticks = touch_ticks; resources.timeout = frequency / 10;
	error = lancelot_touch_prepare_resources(&resources);
	if (error) { printf("lancelot-touch: resource setup refused/failed %d\n", error); goto done; }
	spi_regs = ml_io_map(0x1100a000, 4096);
	if (!spi_regs) goto done;
	/* GIC INTID=32+DT SPI138, not Android's virtual IRQ73 or NVT IRQ135. */
	gic_v3_spi_mask(170);
	spi.read = touch_spi_read; spi.write = touch_spi_write; spi.ticks = touch_ticks; spi.timeout = frequency / 10;
	if (lancelot_spi_configure(&spi, 26000000, 2000000) != LS_OK) goto done;
	nvt.transfer = touch_transfer; nvt.delay_us = touch_delay;
	error = ln_probe(&nvt);
	printf("lancelot-touch: trim=%02x%02x%02x%02x%02x%02x result=%d; GPIO92 untouched\n",
	    nvt.trim[0], nvt.trim[1], nvt.trim[2], nvt.trim[3], nvt.trim[4], nvt.trim[5], error);
	if (error || mode == 2) goto done;
	error = ln_boot_firmware(&nvt, firmware, size);
	printf("lancelot-touch: RAM firmware result=%d version=%02x reset=%02x CRC=%02x ILM=%08x DLM=%08x\n",
	    error, nvt.version, nvt.reset_state, nvt.crc_status, nvt.ilm_crc, nvt.dlm_crc);
	if (error) goto done;
	lancelot_nt36672_state_init(&contacts);
	printf("lancelot-touch: real reports enabled, mode0 FIFO only, poll=8000us; no EINT/GIC touch IRQ enabled\n");
	unsigned bad_run = 0, reloads = 0;
	uint64_t good = 0, bad = 0, reported_at = touch_ticks(NULL);
	for (;;) {
		uint8_t packet[66];
		struct lancelot_nt36672_report report;
		error = ln_read_report(&nvt, packet);
		if (error) {
			lancelot_nt36672_state_bad_packet(&contacts, touch_emit, NULL); bad++; bad_run++;
		} else {
			bool reset = lancelot_nt36672_state_wdt_recovery(&contacts, packet, sizeof(packet), touch_emit, NULL);
			enum lancelot_nt36672_packet_result parsed = lancelot_nt36672_parse(packet, sizeof(packet), &report);
			if (reset) {
				lancelot_nt36672_state_release(&contacts, touch_emit, NULL);
				if (reloads++ >= 2 || ln_boot_firmware(&nvt, firmware, size) != LN_OK) break;
				bad_run = 0;
			} else if (parsed == LANCELOT_NT36672_PACKET_OK) {
				(void)lancelot_nt36672_state_consume(&contacts, &report, touch_emit, NULL); good++; bad_run = 0;
			} else if (parsed == LANCELOT_NT36672_PACKET_WDT_PATTERN) {
				/* Do not break consecutive WDT accounting via bad_packet(). */
				bad++; bad_run++;
			} else if (parsed == LANCELOT_NT36672_PACKET_PALM || parsed == LANCELOT_NT36672_PACKET_RECOVERY) {
				lancelot_nt36672_state_release(&contacts, touch_emit, NULL); bad++; bad_run++;
			} else {
				lancelot_nt36672_state_bad_packet(&contacts, touch_emit, NULL); bad++; bad_run++;
			}
		}
		if (bad_run >= 100) { printf("lancelot-touch: stopping after 100 consecutive invalid reads\n"); break; }
		if (touch_ticks(NULL) - reported_at >= frequency * 60) {
			printf("lancelot-touch: good=%llu bad=%llu messages=%llu SPI-errors=%llu reloads=%u gpio1=%u\n",
			    good, bad, spi.messages, spi.failures, reloads, (touch_read_bank(NULL, LP_GPIO, 0x200) >> 1) & 1);
			reported_at = touch_ticks(NULL);
		}
		touch_delay(NULL, 8000);
	}
done:
	lancelot_nt36672_state_release(&contacts, touch_emit, NULL);
	if (spi_regs && spi.write) lancelot_spi_reset(&spi);
	if (resources.changed) printf("lancelot-touch: resource restore=%d\n", lancelot_touch_restore_resources(&resources));
	printf("lancelot-touch: worker ended (stage=%u); system continues\n", mode);
	thread_terminate(current_thread());
}

void
lancelot_touch_init(void)
{
	uint32_t mode = 0, cpus = 0;
	thread_t thread;
	if (started || !lancelot_touch_platform() || !PE_parse_boot_argn("lancelot_touch", &mode, sizeof(mode)) || !mode) return;
	if (mode > 3 || !PE_parse_boot_argn("cpus", &cpus, sizeof(cpus)) || cpus != 1) {
		printf("lancelot-touch: refusing unsupported stage/SMP settings\n"); return;
	}
	started = true;
	if (kernel_thread_start(touch_thread, (void *)(uintptr_t)mode, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
