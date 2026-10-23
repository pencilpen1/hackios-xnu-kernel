/* Exercise the actual CMD17 PIO implementation against a scripted controller. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../xnu/files/bsd/dev/lancelot_msdc.h"
enum scenario { GOOD, NO_PIO, BUSY, FIFO_STUCK, CMD_TIMEOUT, CMD_CRC, CARD_ERROR, DATA_TIMEOUT, DATA_CRC, SHORT, OVERFLOW };
struct mock {
	enum scenario scenario;
	uint32_t interrupts, phase, position, command_count, count, argument;
	uint64_t time;
};
static uint8_t byte_at(unsigned i) { return (uint8_t)((i * 37) ^ 0x5a); }
static uint32_t read_reg(void *ctx, uint32_t reg)
{
	struct mock *m = ctx;
	switch (reg) {
	case LM_CFG: return m->scenario == NO_PIO ? 1 : 9;
	case LM_STATUS: return m->scenario == BUSY ? 1 : 0;
	case LM_INTEN: return m->interrupts;
	case LM_FIFO:
		if (m->scenario == FIFO_STUCK) return 1u << 31;
		if (m->phase != 2 || m->scenario == DATA_TIMEOUT) return 0;
		if (m->scenario == OVERFLOW) return 200;
		if (m->scenario == SHORT) return 64;
		return m->position < 512 ? 128 : 0;
	case LM_INT:
		if (m->phase == 1) {
			if (m->scenario == CMD_TIMEOUT) return 0;
			return m->scenario == CMD_CRC ? 1u << 10 : LM_CMD_READY;
		}
		if (m->phase == 2) {
			if (m->scenario == DATA_CRC) return 1u << 15;
			if (m->scenario == SHORT || m->position >= 384) return LM_DATA_DONE;
		}
		return 0;
	case LM_RESPONSE: return m->scenario == CARD_ERROR ? 1u << 31 : 0x900;
	case LM_RX: {
		assert(m->phase == 2 && m->position + 4 <= 512);
		uint32_t word = 0;
		for (unsigned i = 0; i < 4; i++) word |= (uint32_t)byte_at(m->position++) << (8 * i);
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
	case LM_FIFO: assert(value == (1u << 31)); break;
	case LM_INT:
		assert(value == LM_EVENTS || value == LM_CMD_READY);
		if (value == LM_CMD_READY && m->phase == 1) m->phase = 2;
		break;
	case LM_COUNT: assert(value == 1); m->count = value; break;
	case LM_ARG: assert(value == 0x12345); m->argument = value; break;
	case LM_CMD:
		assert(value == LM_READ_SINGLE && m->count == 1 && m->argument == 0x12345);
		m->command_count++; m->phase = 1; break;
	default: assert(!"forbidden register/data write");
	}
}
static uint64_t ticks(void *ctx)
{
	struct mock *m = ctx;
	assert(m->time < 100);
	return m->time++;
}
int main(void)
{
	for (unsigned scenario = GOOD; scenario <= OVERFLOW; scenario++) {
		for (unsigned alignment = 0; alignment < 8; alignment++) {
			struct mock mock = {.scenario = scenario, .interrupts = 0xa5a5ffff};
			struct lancelot_msdc controller = {.ctx = &mock, .read = read_reg, .write = write_reg, .ticks = ticks, .timeout = 8};
			uint8_t buffer[528]; memset(buffer, 0xcc, sizeof(buffer));
			int result = lancelot_msdc_sector(&controller, 0x12345, buffer + 8 + alignment);
			assert((result == LM_OK) == (scenario == GOOD));
			assert(mock.command_count <= 1 && mock.interrupts == 0xa5a5ffff);
			for (unsigned i = 0; i < 8 + alignment; i++) assert(buffer[i] == 0xcc);
			for (unsigned i = 8 + alignment + 512; i < sizeof(buffer); i++) assert(buffer[i] == 0xcc);
			if (scenario == GOOD)
				for (unsigned i = 0; i < 512; i++) assert(buffer[8 + alignment + i] == byte_at(i));
		}
	}
	puts("CMD17 PIO: exact bytes/alignment, bounded timeout/CRC/R1/short-read failures and read-only command/register policy passed");
}
