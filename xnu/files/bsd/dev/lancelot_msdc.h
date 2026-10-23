/* MT6768 MSDC0, inherited from the boot firmware, read-only PIO transport.
 * Register encodings follow the MTK MSDC interface; the card is already in
 * sector-addressed eMMC user-area transfer state. No reset/reinitialization,
 * DMA, card configuration, data writes or second CPU are used here.
 */
#ifndef LANCELOT_MSDC_H
#define LANCELOT_MSDC_H
#include <stdint.h>
#include <stddef.h>

#define LM_CFG 0x00u
#define LM_INT 0x0cu
#define LM_INTEN 0x10u
#define LM_FIFO 0x14u
#define LM_RX 0x1cu
#define LM_CMD 0x34u
#define LM_ARG 0x38u
#define LM_STATUS 0x3cu
#define LM_RESPONSE 0x40u
#define LM_COUNT 0x50u
#define LM_CMD_READY (1u << 8)
#define LM_CMD_ERROR ((1u << 9) | (1u << 10))
#define LM_DATA_DONE (1u << 12)
#define LM_DATA_ERROR ((1u << 14) | (1u << 15))
#define LM_EVENTS (LM_CMD_READY | LM_CMD_ERROR | LM_DATA_DONE | LM_DATA_ERROR)
#define LM_READ_SINGLE (17u | (1u << 7) | (1u << 11) | (512u << 16))

struct lancelot_msdc {
	void *ctx;
	uint32_t (*read)(void *, uint32_t);
	void (*write)(void *, uint32_t, uint32_t);
	uint64_t (*ticks)(void *);
	uint64_t timeout;
	uint32_t last_int, last_response, last_stage;
};
enum { LM_OK = 0, LM_TIMEOUT = -1, LM_IO = -2, LM_CONFIG = -3 };
static int
lm_wait_clear(struct lancelot_msdc *m, uint32_t reg, uint32_t bits)
{
	uint64_t start = m->ticks(m->ctx);
	while (m->read(m->ctx, reg) & bits)
		if (m->ticks(m->ctx) - start >= m->timeout) return LM_TIMEOUT;
	return LM_OK;
}
/* Exactly one 512-byte CMD17. Byte stores permit any caller buffer alignment. */
static int
lancelot_msdc_sector(struct lancelot_msdc *m, uint32_t sector, uint8_t out[512])
{
	int status;
	m->last_int = m->last_response = 0;
	m->last_stage = 1;
	uint32_t cfg = m->read(m->ctx, LM_CFG);
	if (!m->timeout || (cfg & 9) != 9 || (cfg & 4)) return LM_CONFIG;
	if ((status = lm_wait_clear(m, LM_STATUS, 3)) != LM_OK) return status;
	uint32_t saved_interrupts = m->read(m->ctx, LM_INTEN);
	m->write(m->ctx, LM_INTEN, saved_interrupts & ~LM_EVENTS);
	m->last_stage = 2;
	m->write(m->ctx, LM_FIFO, 1u << 31);
	if ((status = lm_wait_clear(m, LM_FIFO, 1u << 31)) != LM_OK) goto done;
	m->write(m->ctx, LM_INT, LM_EVENTS);
	m->write(m->ctx, LM_COUNT, 1);
	m->write(m->ctx, LM_ARG, sector);
	m->write(m->ctx, LM_CMD, LM_READ_SINGLE);
	m->last_stage = 3;
	uint64_t start = m->ticks(m->ctx);
	for (;;) {
		m->last_int = m->read(m->ctx, LM_INT);
		if (m->last_int & (LM_CMD_ERROR | LM_DATA_ERROR)) { status = LM_IO; goto done; }
		if (m->last_int & LM_CMD_READY) break;
		if (m->ticks(m->ctx) - start >= m->timeout) { status = LM_TIMEOUT; goto done; }
	}
	m->write(m->ctx, LM_INT, LM_CMD_READY);
	m->last_response = m->read(m->ctx, LM_RESPONSE);
	/* R1 error bits: address/range, ECC, command, write-protect, erase or locked card. */
	if (m->last_response & 0xfff9a080u) { status = LM_IO; goto done; }
	m->last_stage = 4;
	unsigned received = 0;
	start = m->ticks(m->ctx);
	for (;;) {
		m->last_int = m->read(m->ctx, LM_INT);
		if (m->last_int & (LM_CMD_ERROR | LM_DATA_ERROR)) { status = LM_IO; goto done; }
		unsigned available = m->read(m->ctx, LM_FIFO) & 0xff;
		if (available > 128 || available > 512 - received) { status = LM_IO; goto done; }
		while (available >= 4) {
			uint32_t word = m->read(m->ctx, LM_RX);
			for (unsigned j = 0; j < 4; j++) out[received++] = (uint8_t)(word >> (j * 8));
			available -= 4;
		}
		if (m->last_int & LM_DATA_DONE) {
			status = received == 512 ? LM_OK : LM_IO;
			break;
		}
		if (m->ticks(m->ctx) - start >= m->timeout) { status = LM_TIMEOUT; break; }
	}
done:
	m->write(m->ctx, LM_INT, LM_EVENTS);
	m->write(m->ctx, LM_INTEN, saved_interrupts);
	return status;
}
#endif
