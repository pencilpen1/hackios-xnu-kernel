#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
#include "lancelot_nt36672_hw.h"
#define XNULOADER_NO_LIBC
#include "lancelot_sha256.h"
#undef XNULOADER_NO_LIBC

static uint32_t
ln_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool
ln_bus_valid(const struct ln_device *d)
{
	return d && d->transfer && d->delay_us;
}

static int
ln_page(struct ln_device *d, uint32_t address)
{
	uint8_t tx[3] = {0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7)};
	return d->transfer(d->ctx, tx, NULL, 3) == 0 ? LN_OK : LN_IO;
}

int
ln_read(struct ln_device *d, uint32_t address, uint8_t *data, uint32_t length)
{
	if (!ln_bus_valid(d) || !data || !length || length > 256 || address >= 0x40000 || length > 0x40000 - address) return LN_ARGUMENT;
	uint8_t tx[258] = {0}, rx[258];
	if (ln_page(d, address) != LN_OK) return LN_IO;
	tx[0] = (uint8_t)(address & 0x7f);
	if (d->transfer(d->ctx, tx, rx, length + 2) != 0) return LN_IO;
	/* Two SPI RX bytes precede payload: command phase and dummy byte. */
	memcpy(data, rx + 2, length);
	return LN_OK;
}

int
ln_write(struct ln_device *d, uint32_t address, const uint8_t *data, uint32_t length)
{
	if (!ln_bus_valid(d) || !data || !length || length > 256 || address >= 0x40000 || length > 0x40000 - address) return LN_ARGUMENT;
	uint8_t tx[257];
	if (ln_page(d, address) != LN_OK) return LN_IO;
	tx[0] = (uint8_t)((address & 0x7f) | 0x80);
	memcpy(tx + 1, data, length);
	return d->transfer(d->ctx, tx, NULL, length + 1) == 0 ? LN_OK : LN_IO;
}

static int
ln_write8(struct ln_device *d, uint32_t address, uint8_t value)
{
	return ln_write(d, address, &value, 1);
}

static int
ln_bootloader_reset(struct ln_device *d)
{
	if (ln_write8(d, LN_RESET, 0x69) != LN_OK) return LN_IO;
	d->delay_us(d->ctx, 5000);
	return ln_write8(d, LN_FAST, 0);
}

int
ln_probe(struct ln_device *d)
{
	if (!ln_bus_valid(d)) return LN_ARGUMENT;
	d->identified = false;
	d->running = false;
	memset(d->trim, 0, sizeof(d->trim));
	/* Try without a reset first; only page selection is written. No fallback
	 * reset of an unidentified device: missing identity requires diagnosis. */
	if (ln_read(d, LN_TRIM, d->trim, 6) != LN_OK) return LN_IO;
	if (d->trim[0] != 0x0a || d->trim[3] != 0x72 || d->trim[4] != 0x66 || d->trim[5] != 0x03) return LN_ID;
	d->identified = true;
	return LN_OK;
}

/* Parse untrusted header arithmetic independently of the pinned SHA-256. */
static int
ln_fw_header(const uint8_t *data, size_t length, struct ln_firmware *plan)
{
	if (!data || !plan || length != LN_FW_BYTES) return LN_FIRMWARE;
	memset(plan, 0, sizeof(*plan));
	if (memcmp(data + LN_FW_USED - 3, "NVT", 3) || data[LN_FW_USED - 4096] != 8 || data[LN_FW_USED - 4095] != 0xf7) return LN_FIRMWARE;
	uint32_t end = ln_le32(data);
	if (end < 0x30 || end > 0x200 || (end & 15)) return LN_FIRMWARE;
	unsigned info = (end - 0x30) / 16;
	unsigned overlays = data[0x28] & 0x10 ? data[0x28] & 15 : 0;
	if (2 + info + overlays > LN_FW_PARTS) return LN_FIRMWARE;
	plan->count = 2 + info + overlays;
	for (unsigned i = 0; i < plan->count; i++) {
		struct ln_partition *p = &plan->part[i];
		if (i < 2) {
			p->bin = ln_le32(data + i * 12);
			p->sram = ln_le32(data + i * 12 + 4);
			p->last = ln_le32(data + i * 12 + 8);
			p->crc = ln_le32(data + 0x18 + i * 4);
		} else {
			uint32_t at = i < 2 + info ? 0x30 + (i - 2) * 16 : plan->part[1].bin + (i - 2 - info) * 16;
			if (at > LN_FW_USED - 16) return LN_FIRMWARE;
			p->sram = ln_le32(data + at); p->last = ln_le32(data + at + 4);
			p->bin = ln_le32(data + at + 8); p->crc = ln_le32(data + at + 12);
		}
		if (!p->last) {
			if (i < 2) return LN_FIRMWARE;
			continue;
		}
		/* last+1 bytes, not last: reject overflow and writes into registers. */
		if (p->bin >= LN_FW_USED || p->last >= LN_FW_USED - p->bin ||
		    p->sram >= 0x30000 || p->last >= 0x30000 - p->sram) return LN_FIRMWARE;
		if (i < 2 && p->last > 65535) return LN_FIRMWARE; /* hw_crc=1 has 16-bit lengths */
	}
	if (!plan->part[0].last || !plan->part[1].last) return LN_FIRMWARE;
	return LN_OK;
}

int
ln_validate_firmware(const uint8_t *data, size_t length, struct ln_firmware *plan)
{
	static const uint8_t expected[32] = {
		0xc4,0x1b,0xa8,0xac,0xa0,0xd0,0xa9,0x6d,0xb5,0x47,0x1a,0xf8,0xa6,0xa5,0x4f,0xe6,
		0xc6,0xc5,0x5f,0xc9,0x75,0x9f,0x26,0xca,0x73,0x81,0xd9,0xe2,0xde,0x34,0x90,0x2d
	};
	uint8_t digest[32];
	if (ln_fw_header(data, length, plan) != LN_OK) {
		if (plan) memset(plan, 0, sizeof(*plan));
		return LN_FIRMWARE;
	}
	sha256(data, length, digest);
	if (memcmp(digest, expected, 32)) { memset(plan, 0, sizeof(*plan)); return LN_FIRMWARE; }
	return LN_OK;
}

static int
ln_crc_bank(struct ln_device *d, const struct ln_partition *p, bool dlm)
{
	uint8_t addr[3] = {(uint8_t)p->sram, (uint8_t)(p->sram >> 8), (uint8_t)(p->sram >> 16)};
	uint8_t len[2] = {(uint8_t)p->last, (uint8_t)(p->last >> 8)};
	uint8_t crc[4] = {(uint8_t)p->crc, (uint8_t)(p->crc >> 8), (uint8_t)(p->crc >> 16), (uint8_t)(p->crc >> 24)};
	if (ln_write(d, dlm ? 0x3f12c : 0x3f128, addr, 3) != LN_OK ||
	    ln_write(d, dlm ? 0x3f130 : 0x3f118, len, 2) != LN_OK ||
	    ln_write(d, dlm ? 0x3f104 : 0x3f100, crc, 4) != LN_OK) return LN_IO;
	return LN_OK;
}

int
ln_boot_firmware(struct ln_device *d, const uint8_t *data, size_t length)
{
	struct ln_firmware plan;
	if (!ln_bus_valid(d)) return LN_ARGUMENT;
	d->running = false;
	if (!d->identified) return LN_ID;
	/* Validation occurs before any reset or RAM write. */
	if (ln_validate_firmware(data, length, &plan) != LN_OK) return LN_FIRMWARE;
	int probed = ln_probe(d);
	if (probed != LN_OK) return probed;
	d->version = d->reset_state = d->crc_status = 0;
	d->ilm_crc = d->dlm_crc = 0;
	if (ln_bootloader_reset(d) != LN_OK) return LN_IO;
	for (unsigned i = 0; i < plan.count; i++) {
		const struct ln_partition *p = &plan.part[i];
		if (!p->last) continue;
		for (uint32_t at = 0; at <= p->last;) {
			uint32_t n = p->last + 1 - at;
			uint32_t page = 128 - ((p->sram + at) & 127);
			if (n > page) n = page;
			if (ln_write(d, p->sram + at, data + p->bin + at, n) != LN_OK) return LN_IO;
			at += n;
		}
	}
	if (ln_crc_bank(d, &plan.part[0], false) != LN_OK || ln_crc_bank(d, &plan.part[1], true) != LN_OK) return LN_IO;
	uint8_t bld;
	if (ln_read(d, 0x3f30e, &bld, 1) != LN_OK || ln_write8(d, 0x3f30e, bld | 0x80) != LN_OK ||
	    ln_write8(d, LN_EVENT + 0x60, 0) != LN_OK || ln_write8(d, LN_EVENT + 0x50, 0xae) != LN_OK ||
	    ln_write8(d, 0x3f10d, 1) != LN_OK) return LN_IO;
	d->delay_us(d->ctx, 5000);
	for (unsigned i = 0; i < 51; i++) {
		if (ln_read(d, LN_EVENT + 0x60, &d->reset_state, 1) != LN_OK) return LN_IO;
		if (d->reset_state >= 0xa1 && d->reset_state <= 0xaf) break;
		if (i == 50) return LN_BOOT;
		d->delay_us(d->ctx, 10000);
	}
	uint8_t crc[8];
	if (ln_read(d, 0x3f133, &d->crc_status, 1) != LN_OK || ln_read(d, 0x3f120, crc, 8) != LN_OK) return LN_IO;
	d->ilm_crc = ln_le32(crc); d->dlm_crc = ln_le32(crc + 4);
	if ((d->crc_status & 7) != 4 || d->ilm_crc != plan.part[0].crc || d->dlm_crc != plan.part[1].crc) return LN_CRC;
	uint8_t info[16];
	if (ln_read(d, LN_EVENT + 0x78, info, 16) != LN_OK) return LN_IO;
	d->version = info[0];
	if (info[0] != 8 || info[1] != 0xf7 || ((unsigned)info[4] << 8 | info[5]) != 1080 ||
	    ((unsigned)info[6] << 8 | info[7]) != 2340) return LN_FIRMWARE;
	d->running = true;
	return LN_OK;
}

int
ln_read_report(struct ln_device *d, uint8_t packet[66])
{
	if (!packet || !ln_bus_valid(d) || !d->identified || !d->running) return LN_ARGUMENT;
	packet[0] = 0;
	return ln_read(d, LN_EVENT, packet + 1, 65);
}
