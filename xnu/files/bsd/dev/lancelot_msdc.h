/* MT6768 MSDC0 counted read-only PIO. Card configuration commands used by
 * lancelot_mmc_mode.h are volatile; no media write/erase commands exist here. */
#ifndef LANCELOT_MSDC_H
#define LANCELOT_MSDC_H
#include <stdint.h>
#include <stddef.h>

#define LM_CFG 0x00u
#define LM_PS 0x08u
#define LM_INT 0x0cu
#define LM_INTEN 0x10u
#define LM_FIFO 0x14u
#define LM_RX 0x1cu
#define LM_BUS_CFG 0x30u
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
#define LM_READ_MULTIPLE (18u | (1u << 7) | (1u << 12) | (512u << 16))
#define LM_READ_EXT_CSD (8u | (1u << 7) | (1u << 11) | (512u << 16))
#define LM_STOP (12u | (7u << 7) | (1u << 14))
#define LM_R1_ERRORS 0xfff9a080u
#define LM_DAT0 (1u << 16)
#define LM_CLOCK_STABLE (1u << 7)
/* Bound PIO interrupt-off latency; larger requests are split, not rejected. */
#define LM_MAX_BLOCKS 128u

struct lancelot_msdc {
	void *ctx;
	uint32_t (*read)(void *, uint32_t);
	void (*write)(void *, uint32_t, uint32_t);
	uint64_t (*ticks)(void *);
	uint64_t timeout;
	uint32_t last_int, last_response, last_stage;
	void (*read_fifo)(void *, uint8_t *, unsigned); /* optional word-burst adapter */
	uint64_t (*critical_enter)(void *);
	void (*critical_exit)(void *, uint64_t);
	uint32_t rca, last_command;
	int recovery_status;
	uint64_t commands_sent, multi_reads, recoveries;
};
enum { LM_OK = 0, LM_TIMEOUT = -1, LM_IO = -2, LM_CONFIG = -3, LM_SWITCH_REJECTED = -4 };
static int
lm_wait_value(struct lancelot_msdc *m, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint64_t start = m->ticks(m->ctx);
	while ((m->read(m->ctx, reg) & mask) != value)
		if (m->ticks(m->ctx) - start >= m->timeout) return LM_TIMEOUT;
	return LM_OK;
}
static int
lm_wait_clear(struct lancelot_msdc *m, uint32_t reg, uint32_t bits)
{
	return lm_wait_value(m, reg, bits, 0);
}
/* Caller serializes commands and masks controller interrupts. CMD12 must be
 * issuable while the data engine is busy; all other commands require idle. */
static int
lm_command(struct lancelot_msdc *m, uint32_t raw, uint32_t argument)
{
	uint32_t command = raw & 63;
	if (command != 6 && command != 8 && command != 12 && command != 13 &&
	    command != 17 && command != 18 && command != 23) return LM_CONFIG;
	if (command == 6 && (raw != (6u | (7u << 7)) ||
	    (argument & 0xffff00ffu) != ((3u << 24) | (183u << 16)) || ((argument >> 8) & 255) > 2)) return LM_CONFIG;
	m->last_command = command;
	int status = lm_wait_clear(m, LM_STATUS, command == 12 ? 2 : 3);
	if (status != LM_OK) return status;
	m->write(m->ctx, LM_INT, LM_CMD_READY | LM_CMD_ERROR);
	m->write(m->ctx, LM_ARG, argument);
	m->write(m->ctx, LM_CMD, raw);
	m->commands_sent++;
	m->last_stage = 3;
	uint64_t start = m->ticks(m->ctx);
	for (;;) {
		m->last_int = m->read(m->ctx, LM_INT);
		if (m->last_int & LM_CMD_ERROR) return LM_IO;
		if (m->last_int & LM_CMD_READY) break;
		if (m->ticks(m->ctx) - start >= m->timeout) return LM_TIMEOUT;
	}
	m->write(m->ctx, LM_INT, LM_CMD_READY);
	m->last_response = m->read(m->ctx, LM_RESPONSE);
	if (m->last_response & (LM_R1_ERRORS & ~0x80u)) return LM_IO;
	if (m->last_response & 0x80) return LM_SWITCH_REJECTED;
	if (((raw >> 7) & 7) == 7) {
		status = lm_wait_value(m, LM_PS, LM_DAT0, LM_DAT0);
		if (status != LM_OK) return status;
	}
	return LM_OK;
}
static int
lm_card_ready(struct lancelot_msdc *m)
{
	if (!m->rca || m->rca > 65535) return LM_CONFIG;
	uint64_t start = m->ticks(m->ctx);
	for (;;) {
		int status = lm_command(m, 13u | (1u << 7), m->rca << 16);
		if (status != LM_OK) return status;
		unsigned state = (m->last_response >> 9) & 15;
		if (state == 4 && (m->last_response & 0x100)) return LM_OK;
		if (state != 4 && state != 7) return LM_IO;
		if (m->ticks(m->ctx) - start >= m->timeout) return LM_TIMEOUT;
	}
}
static int
lm_recover_read(struct lancelot_msdc *m)
{
	uint32_t interrupt = m->last_int, response = m->last_response;
	uint32_t stage = m->last_stage, command = m->last_command;
	m->recoveries++;
	/* A completed/failed single transfer may reject STOP. CMD13 must still
	 * prove ready/TRAN, and both the host engine and FIFO must become idle. */
	(void)lm_command(m, LM_STOP, 0);
	int status = lm_card_ready(m);
	if (status == LM_OK) status = lm_wait_clear(m, LM_STATUS, 3);
	if (status == LM_OK) {
		m->write(m->ctx, LM_FIFO, 1u << 31);
		status = lm_wait_clear(m, LM_FIFO, 1u << 31);
	}
	m->last_int = interrupt; m->last_response = response;
	m->last_stage = stage; m->last_command = command;
	m->recovery_status = status;
	return status;
}
static void
lm_fifo_read(struct lancelot_msdc *m, uint8_t *out, unsigned bytes)
{
	if (m->read_fifo) { m->read_fifo(m->ctx, out, bytes / 4); return; }
	for (unsigned i = 0; i < bytes; i += 4) {
		uint32_t word = m->read(m->ctx, LM_RX);
		for (unsigned j = 0; j < 4; j++) out[i + j] = (uint8_t)(word >> (j * 8));
	}
}
static int
lm_data_read(struct lancelot_msdc *m, uint32_t raw, uint32_t argument,
    uint32_t count, uint8_t *out, size_t capacity)
{
	if (!out || !count || count > LM_MAX_BLOCKS || capacity < (size_t)count * 512 || !m->timeout)
		return LM_CONFIG;
	if ((m->critical_enter == NULL) != (m->critical_exit == NULL)) return LM_CONFIG;
	uint64_t critical = m->critical_enter ? m->critical_enter(m->ctx) : 0;
	m->last_int = m->last_response = 0; m->last_stage = 1; m->recovery_status = LM_OK;
	uint32_t cfg = m->read(m->ctx, LM_CFG);
	int status = LM_CONFIG;
	if ((cfg & 9) != 9 || (cfg & 4)) goto leave;
	status = lm_wait_clear(m, LM_STATUS, 3);
	if (status != LM_OK) goto leave;
	uint32_t saved_interrupts = m->read(m->ctx, LM_INTEN);
	m->write(m->ctx, LM_INTEN, saved_interrupts & ~LM_EVENTS);
	int started = 0;
	m->last_stage = 2;
	m->write(m->ctx, LM_FIFO, 1u << 31);
	if ((status = lm_wait_clear(m, LM_FIFO, 1u << 31)) != LM_OK) goto done;
	m->write(m->ctx, LM_INT, LM_EVENTS);
	if (count > 1) {
		status = lm_command(m, 23u | (1u << 7), count);
		if (status != LM_OK) goto done; /* do not start CMD18 if SET_BLOCK_COUNT failed */
	}
	m->write(m->ctx, LM_COUNT, count);
	started = 1; /* command timeout is also an uncertain transfer state */
	status = lm_command(m, raw, argument);
	if (status != LM_OK) goto done;
	if (count > 1) m->multi_reads++;
	m->last_stage = 4;
	unsigned received = 0, total = count * 512;
	uint64_t start = m->ticks(m->ctx);
	for (;;) {
		m->last_int = m->read(m->ctx, LM_INT);
		if (m->last_int & (LM_CMD_ERROR | LM_DATA_ERROR)) { status = LM_IO; goto done; }
		unsigned available = m->read(m->ctx, LM_FIFO) & 0xff;
		if (available > 128 || available > total - received) { status = LM_IO; goto done; }
		unsigned take = available & ~3u;
		if (take) {
			lm_fifo_read(m, out + received, take);
			received += take;
		}
		if (m->last_int & LM_DATA_DONE) {
			status = received == total ? lm_wait_clear(m, LM_STATUS, 3) : LM_IO;
			break;
		}
		if (m->ticks(m->ctx) - start >= m->timeout) { status = LM_TIMEOUT; break; }
	}
done:
	if (status != LM_OK && started) (void)lm_recover_read(m);
	m->write(m->ctx, LM_INT, LM_EVENTS);
	m->write(m->ctx, LM_INTEN, saved_interrupts);
leave:
	if (m->critical_exit) m->critical_exit(m->ctx, critical);
	return status;
}
static int
lancelot_msdc_read(struct lancelot_msdc *m, uint32_t sector, uint32_t count, uint8_t *out, size_t capacity)
{
	if (!count || count > LM_MAX_BLOCKS || (uint64_t)sector + count > (1ULL << 32)) return LM_CONFIG;
	return lm_data_read(m, count == 1 ? LM_READ_SINGLE : LM_READ_MULTIPLE, sector, count, out, capacity);
}
static int
lancelot_msdc_ext_csd(struct lancelot_msdc *m, uint8_t out[512])
{
	return lm_data_read(m, LM_READ_EXT_CSD, 0, 1, out, 512);
}
#endif
