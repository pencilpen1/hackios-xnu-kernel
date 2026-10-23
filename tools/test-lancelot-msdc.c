/* Scripted eMMC: counted reads, aborts, width/clock negotiation and rollback. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../xnu/files/bsd/dev/lancelot_mmc_mode.h"
enum scenario {
	GOOD, NO_PIO, BUSY, FIFO_STUCK, CMD_TIMEOUT, CMD_CRC, CARD_ERROR,
	DATA_TIMEOUT, DATA_CRC, SHORT, OVERFLOW, COUNT_REJECT, RECOVERY_FAIL,
	WIDTH8_REJECT, WIDTH8_BAD, ALL_WIDE_BAD, CLOCK_BAD, CLOCK_STUCK, ROLLBACK_BAD
};
struct mock {
	enum scenario scenario;
	uint32_t interrupts, cfg, bus_cfg, phase, position, total, count, pending_count;
	uint32_t argument, command, response, commands[64], command_count, bursts;
	uint32_t clock_writes, width_writes, verifies, depth, irq, status_busy_left;
	int tried_fast, restored_clock, pending_switch_error;
	uint64_t time;
	uint8_t ext[512];
};
static int data_command(unsigned n) { return n == 8 || n == 17 || n == 18; }
static int fast_clock(struct mock *m) { return ((m->cfg >> 8) & 0xfff) == 1; }
static int bad_data(struct mock *m)
{
	return (m->ext[183] <= 2 && lm_width(m->bus_cfg) != (m->ext[183] == 2 ? 8u : m->ext[183] == 1 ? 4u : 1u)) ||
	    m->scenario == DATA_CRC || m->scenario == RECOVERY_FAIL ||
	    (m->scenario == WIDTH8_BAD && m->ext[183] == 2) ||
	    (m->scenario == ALL_WIDE_BAD && m->ext[183] != 0) ||
	    ((m->scenario == CLOCK_BAD || m->scenario == ROLLBACK_BAD) && fast_clock(m));
}
static uint8_t byte_at(uint32_t sector, unsigned i) { return (uint8_t)((i * 37) ^ ((sector + i / 512) * 13)); }
static uint32_t read_reg(void *ctx, uint32_t reg)
{
	struct mock *m = ctx;
	switch (reg) {
	case LM_CFG:
		if (m->scenario == NO_PIO) return m->cfg & ~8u;
		return m->cfg | ((m->scenario == CLOCK_STUCK && fast_clock(m)) ? 0 : LM_CLOCK_STABLE);
	case LM_BUS_CFG: return m->bus_cfg;
	case LM_PS: return LM_DAT0;
	case LM_STATUS:
		if (m->scenario == BUSY) return 1;
		if (m->scenario == RECOVERY_FAIL && m->phase == 2) return 3;
		return 0;
	case LM_INTEN: return m->interrupts;
	case LM_FIFO:
		if (m->scenario == FIFO_STUCK) return 1u << 31;
		if (m->phase != 2 || m->scenario == DATA_TIMEOUT) return 0;
		if (m->scenario == OVERFLOW) return 200;
		if (m->scenario == SHORT) return 64;
		return m->position < m->total ? (m->total - m->position < 128 ? m->total - m->position : 128) : 0;
	case LM_INT:
		if (m->phase == 1) {
			if (data_command(m->command) && m->scenario == CMD_TIMEOUT) return 0;
			if (data_command(m->command) && m->scenario == CMD_CRC) return 1u << 10;
			return LM_CMD_READY;
		}
		if (m->phase == 2) {
			if (bad_data(m)) return 1u << 15;
			if (m->scenario == SHORT || m->position + 128 >= m->total) return LM_DATA_DONE;
		}
		return 0;
	case LM_RESPONSE: return m->response;
	case LM_RX: {
		assert(m->depth == 1 && m->phase == 2 && m->position + 4 <= m->total);
		uint32_t word = 0;
		for (unsigned i = 0; i < 4; i++) {
			uint8_t value = m->command == 8 ? m->ext[m->position] : byte_at(m->argument, m->position);
			if (m->command == 8 && m->position == 183) value = 0; /* write-only mode byte does not echo */
			m->position++; word |= (uint32_t)value << (8 * i);
		}
		return word;
	}
	default: assert(!"unexpected register read"); return 0;
	}
}
static void write_reg(void *ctx, uint32_t reg, uint32_t value)
{
	struct mock *m = ctx;
	switch (reg) {
	case LM_INTEN: m->interrupts = value; break;
	case LM_CFG:
		assert((value & ~((3u << 20) | (0xfffu << 8) | LM_CLOCK_STABLE | 2u)) == 9);
		m->cfg = value & ~LM_CLOCK_STABLE; m->clock_writes++;
		if (fast_clock(m)) m->tried_fast = 1;
		else if (m->tried_fast) m->restored_clock = 1;
		break;
	case LM_BUS_CFG:
		assert((value & ~(3u << 16)) == (m->bus_cfg & ~(3u << 16)));
		m->bus_cfg = value; m->width_writes++; break;
	case LM_FIFO: assert(value == (1u << 31)); break;
	case LM_INT:
		assert(value == LM_EVENTS || value == LM_CMD_READY || value == (LM_CMD_READY | LM_CMD_ERROR));
		if (value == LM_CMD_READY && m->phase == 1)
			m->phase = data_command(m->command) && !(m->response & LM_R1_ERRORS) ? 2 : 0;
		break;
	case LM_COUNT: assert(value && value <= LM_MAX_BLOCKS); m->count = value; break;
	case LM_ARG: m->argument = value; break;
	case LM_CMD: {
		unsigned command = value & 63;
		assert(m->command_count < 64);
		m->commands[m->command_count++] = command;
		m->command = command; m->phase = 1; m->response = 0x900;
		switch (command) {
		case 23:
			assert(value == (23u | (1u << 7)) && m->argument && m->argument <= LM_MAX_BLOCKS);
			if (m->scenario == COUNT_REJECT) m->response |= 1u << 22;
			else m->pending_count = m->argument;
			break;
		case 18:
			assert(value == LM_READ_MULTIPLE && m->pending_count == m->count && m->count > 1);
			m->pending_count = 0;
			m->position = 0; m->total = m->count * 512;
			break;
		case 17: case 8:
			assert(value == (command == 17 ? LM_READ_SINGLE : LM_READ_EXT_CSD) && m->count == 1);
			if (command == 8) assert(m->argument == 0);
			m->position = 0; m->total = 512;
			break;
		case 12: assert(value == LM_STOP && m->argument == 0); m->pending_count = 0; break;
		case 13:
			assert(value == (13u | (1u << 7)) && m->argument == (1u << 16));
			if (m->status_busy_left) { m->status_busy_left--; m->response = 7u << 9; }
			if (m->pending_switch_error) { m->response |= 0x80; m->pending_switch_error = 0; }
			break;
		case 6: {
			assert(value == (6u | (7u << 7)) && (m->argument & 0xffff00ffu) == 0x03b70000u);
			unsigned width = (m->argument >> 8) & 255;
			assert(width <= 2);
			if (m->scenario == WIDTH8_REJECT && width == 2) m->pending_switch_error = 1;
			else m->ext[183] = width;
			break;
		}
		default: assert(!"forbidden media write/erase command");
		}
		if (data_command(command) && m->scenario == CARD_ERROR) m->response |= 1u << 31;
		break;
	}
	default: assert(!"forbidden register/data write");
	}
}
static uint64_t ticks(void *ctx)
{
	struct mock *m = ctx;
	assert(m->time < 1000000);
	return m->time++;
}
static uint64_t enter(void *ctx)
{
	struct mock *m = ctx; assert(m->depth++ == 0);
	uint64_t previous = m->irq; m->irq = 0; return previous;
}
static void leave(void *ctx, uint64_t previous)
{
	struct mock *m = ctx; assert(m->depth-- == 1 && m->irq == 0);
	m->irq = previous;
}
static void burst(void *ctx, uint8_t *out, unsigned words)
{
	struct mock *m = ctx; m->bursts++;
	assert(words && words <= 32);
	for (unsigned i = 0; i < words; i++) {
		uint32_t word = read_reg(ctx, LM_RX);
		for (unsigned j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(word >> (j * 8));
	}
}
static struct lancelot_msdc setup(struct mock *m, enum scenario scenario, int fast)
{
	memset(m, 0, sizeof(*m)); m->scenario = scenario;
	m->interrupts = 0xa5a5ffff; m->cfg = 9 | 2 | (2u << 8); m->irq = 1;
	m->ext[185] = 1; m->ext[192] = 8; m->ext[196] = 3; m->ext[214] = 0x60; m->ext[215] = 7;
	return (struct lancelot_msdc){.ctx=m, .read=read_reg, .write=write_reg, .ticks=ticks,
	    .timeout=2048, .rca=1, .read_fifo=fast ? burst : NULL, .critical_enter=enter, .critical_exit=leave};
}
static int verify(void *ctx)
{
	struct mock *m = ctx;
	m->verifies++;
	return !bad_data(m) && !(m->scenario == ROLLBACK_BAD && m->restored_clock) &&
	    lm_width(m->bus_cfg) == (m->ext[183] == 2 ? 8u : m->ext[183] == 1 ? 4u : 1u);
}
int main(void)
{
	for (unsigned fast = 0; fast < 2; fast++) for (unsigned count = 1; count <= LM_MAX_BLOCKS; count++) {
		unsigned bytes = count * 512;
		for (unsigned alignment = 0; alignment < 8; alignment++) {
			struct mock m; struct lancelot_msdc controller = setup(&m, GOOD, fast);
			uint8_t *buffer = malloc(bytes + 16); assert(buffer); memset(buffer, 0xcc, bytes + 16);
			assert(lancelot_msdc_read(&controller, 0x12345, count, buffer + 8 + alignment, bytes) == LM_OK);
			assert(m.command_count == (count == 1 ? 1u : 2u));
			for (unsigned i = 0; i < bytes; i++) assert(buffer[8 + alignment + i] == byte_at(0x12345, i));
			for (unsigned i = 0; i < 8 + alignment; i++) assert(buffer[i] == 0xcc);
			for (unsigned i = bytes + 8 + alignment; i < bytes + 16; i++) assert(buffer[i] == 0xcc);
			assert(!m.depth && m.irq == 1 && m.interrupts == 0xa5a5ffff && !m.clock_writes && !m.width_writes);
			assert(!fast || m.bursts == bytes / 128);
			free(buffer);
		}
	}
	for (unsigned scenario = NO_PIO; scenario <= RECOVERY_FAIL; scenario++) {
		struct mock m; struct lancelot_msdc controller = setup(&m, scenario, 1);
		uint8_t buffer[1040]; memset(buffer, 0xcc, sizeof(buffer));
		assert(lancelot_msdc_read(&controller, 0x12345, 2, buffer + 8, 1024) != LM_OK);
		for (unsigned i = 0; i < 8; i++) assert(buffer[i] == 0xcc && buffer[1032 + i] == 0xcc);
		assert(!m.depth && m.irq == 1 && m.interrupts == 0xa5a5ffff);
		if (scenario == COUNT_REJECT) assert(m.command_count == 1 && m.commands[0] == 23);
		if (scenario == RECOVERY_FAIL) assert(controller.recovery_status != LM_OK);
		if (scenario == DATA_CRC || scenario == SHORT || scenario == OVERFLOW) {
			assert(controller.recovery_status == LM_OK && controller.recoveries == 1);
			assert(m.commands[m.command_count - 2] == 12 && m.commands[m.command_count - 1] == 13);
		}
	}
	struct mock m; struct lancelot_msdc controller = setup(&m, GOOD, 1);
	uint8_t buffer[512];
	assert(lancelot_msdc_read(&controller, 0, 0, buffer, sizeof(buffer)) == LM_CONFIG);
	assert(lancelot_msdc_read(&controller, 0, 129, buffer, sizeof(buffer)) == LM_CONFIG);
	assert(lancelot_msdc_read(&controller, UINT32_MAX, 2, buffer, sizeof(buffer)) == LM_CONFIG);
	assert(lancelot_msdc_read(&controller, 0, 1, buffer, 511) == LM_CONFIG);
	assert(lm_command(&controller, 24 | (1 << 7), 0) == LM_CONFIG);
	assert(lm_command(&controller, 6 | (7 << 7), 0x03b30100) == LM_CONFIG); /* partition switch is forbidden */
	assert(m.command_count == 0 && m.depth == 0);
	m.irq = 0;
	assert(lancelot_msdc_read(&controller, 0, 1, buffer, sizeof(buffer)) == LM_OK && m.irq == 0);

	for (unsigned scenario = GOOD; scenario <= ROLLBACK_BAD; scenario++) {
		if (scenario > GOOD && scenario < WIDTH8_REJECT) continue;
		controller = setup(&m, scenario, 1);
		struct lancelot_mmc_mode mode;
		int status = lancelot_msdc_negotiate(&controller, 8, 208000000, 1, verify, &m, &mode);
		assert(!m.depth && m.irq == 1 && m.interrupts == 0xa5a5ffff);
		if (scenario == ROLLBACK_BAD) { assert(status != LM_OK); continue; }
		assert(status == LM_OK && m.verifies);
		assert(mode.width == (scenario == WIDTH8_REJECT || scenario == WIDTH8_BAD ? 4u : scenario == ALL_WIDE_BAD ? 1u : 8u));
		assert(mode.clock_hz == (scenario == CLOCK_BAD || scenario == CLOCK_STUCK ? 26000000u : 52000000u));
		assert(verify(&m));
	}
	controller = setup(&m, GOOD, 1);
	struct lancelot_mmc_mode mode;
	m.status_busy_left = 2;
	assert(lancelot_msdc_negotiate(&controller, 8, 0, 1, verify, &m, &mode) == LM_OK);
	assert(mode.width == 8 && !mode.clock_hz && !m.clock_writes && !m.status_busy_left);
	controller = setup(&m, GOOD, 1);
	m.ext[185] = 2; m.ext[183] = 2; m.bus_cfg = 2u << 16; m.cfg = 9 | 2 | (1u << 20);
	assert(lancelot_msdc_negotiate(&controller, 8, 200000000, 2, verify, &m, &mode) == LM_OK);
	assert(mode.width == 8 && mode.clock_hz == 200000000 && !m.clock_writes && !m.width_writes);
	puts("counted reads: 1..128 sectors, exact bytes/alignment, 2 commands per 64 KiB, IRQ restore and bounded recovery passed");
	puts("SDR negotiation: 8/4/1-bit, HS52 divisor selection, verified rollback and inherited tuned-mode preservation passed");
}
