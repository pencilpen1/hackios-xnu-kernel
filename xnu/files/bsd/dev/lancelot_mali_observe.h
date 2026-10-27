/* MT6768 always-on observation only. No write callback, GPU/MFG bank or PMIC
 * command. Tables: clk-mt6768[-pg].c and mainline MT6768 pwrap register table. */
#ifndef LANCELOT_MALI_OBSERVE_H
#define LANCELOT_MALI_OBSERVE_H
#include <stdint.h>
#include <stdbool.h>
enum lmao_bank { LMAO_SPM, LMAO_TOP, LMAO_PLL, LMAO_INFRA, LMAO_WRAP, LMAO_BANK_COUNT };
static const uint32_t lmao_spm_offsets[] = {0x180,0x184,0x338,0x34c,0x310,0x334,0x180,0x184};
static const uint32_t lmao_pll_offsets[] = {0x248,0x24c,0x254,0x258,0x25c,0x264,0x238,0x23c,0x244};
static const uint32_t lmao_wrap_offsets[] = {
	0x000,0x004,0x070,0x060,0x064,0x068,0x06c,
	0x080,0x084,0x088,0x08c,0x090,0x094,0x098,0x09c,
	0x0a0,0x0a4,0x0a8,0x0ac,0xc04,0xc14,0xc24,0xc34
};
struct lmao_io {
	void *cookie;
	int (*read32)(void *, enum lmao_bank, uint32_t, uint32_t *);
	void (*delay_us)(void *, uint32_t);
};
struct lmao_snapshot {
	uint32_t spm[8], top, pll[9], infra[2], wrap[23];
	uint32_t wacs2_first, wacs2_last, wacs2_active, wacs2_changes, wacs2_no_init, samples;
	uint32_t wacs2_trace[64], wacs2_fsm_or_req, wacs2_sync_busy;
};
static inline bool
lmao_read_allowed(enum lmao_bank bank, uint32_t offset)
{
	if (bank == LMAO_TOP) return offset == 0x50;
	if (bank == LMAO_INFRA) return offset == 0x220 || offset == 0x228;
	const uint32_t *list = 0; unsigned count = 0;
	if (bank == LMAO_SPM) { list = lmao_spm_offsets; count = 8; }
	if (bank == LMAO_PLL) { list = lmao_pll_offsets; count = 9; }
	if (bank == LMAO_WRAP) { list = lmao_wrap_offsets; count = 23; }
	for (unsigned i = 0; i < count; i++) if (list[i] == offset) return true;
	return false;
}
static inline int
lmao_read(const struct lmao_io *io, enum lmao_bank bank, uint32_t off, uint32_t *value)
{
	if (!io || !io->read32 || !value || !lmao_read_allowed(bank, off)) return -22;
	return io->read32(io->cookie, bank, off, value);
}
static inline int
lmao_capture_spm(const struct lmao_io *io, struct lmao_snapshot *s)
{
	if (!s) return -22;
	for (unsigned i = 0; i < 8; i++) {
		int e = lmao_read(io, LMAO_SPM, lmao_spm_offsets[i], &s->spm[i]);
		if (e) return e;
	}
	return 0;
}
static inline int
lmao_capture_ao(const struct lmao_io *io, struct lmao_snapshot *s)
{
	if (!io || !io->delay_us || !s) return -22;
	int e = lmao_read(io, LMAO_TOP, 0x50, &s->top);
	if (e) return e;
	for (unsigned i = 0; i < 9; i++) {
		e = lmao_read(io, LMAO_PLL, lmao_pll_offsets[i], &s->pll[i]); if (e) return e;
	}
	e = lmao_read(io, LMAO_INFRA, 0x220, &s->infra[0]); if (e) return e;
	e = lmao_read(io, LMAO_INFRA, 0x228, &s->infra[1]); if (e) return e;
	for (unsigned i = 0; i < 23; i++) {
		e = lmao_read(io, LMAO_WRAP, lmao_wrap_offsets[i], &s->wrap[i]); if (e) return e;
	}
	s->samples = s->wacs2_active = s->wacs2_changes = s->wacs2_no_init = 0;
	s->wacs2_fsm_or_req = s->wacs2_sync_busy = 0;
	for (unsigned i = 0; i < 64; i++) {
		uint32_t v;
		e = lmao_read(io, LMAO_WRAP, 0xc24, &v); if (e) return e;
		if (!i) s->wacs2_first = v;
		else if (v != s->wacs2_last) s->wacs2_changes++;
		s->wacs2_last = v;
		s->wacs2_trace[i] = v;
		if (((v >> 16) & 7) || (v & (1u << 19))) s->wacs2_fsm_or_req++;
		if (!(v & (1u << 20))) s->wacs2_sync_busy++;
		if (((v >> 16) & 7) || (v & (1u << 19)) || !(v & (1u << 20))) s->wacs2_active++;
		if (!(v & (1u << 21))) s->wacs2_no_init++;
		s->samples++;
		if (i != 63) io->delay_us(io->cookie, 1000);
	}
	return 0; /* No activity in this finite window is NOT ownership proof. */
}
#endif
