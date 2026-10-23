/* Negotiate SDR width/clock in the existing HS timing mode. Every change is
 * checked with EXT_CSD and a caller-provided known-data read. No HS200/HS400
 * switch without voltage/tuning support, and no unverified fallback state. */
#ifndef LANCELOT_MMC_MODE_H
#define LANCELOT_MMC_MODE_H
#include "lancelot_msdc.h"

struct lancelot_mmc_mode {
	uint32_t width, timing, device_type, fallbacks;
	uint64_t clock_hz;
};
static uint32_t lm_width(uint32_t cfg)
{
	unsigned n = (cfg >> 16) & 3;
	return n == 0 ? 1 : n == 1 ? 4 : n == 2 ? 8 : 0;
}
static unsigned lm_width_code(uint32_t width) { return width == 8 ? 2 : width == 4 ? 1 : 0; }
static uint64_t lm_clock_hz(uint32_t cfg, uint64_t source)
{
	unsigned mode = (cfg >> 20) & 3, div = (cfg >> 8) & 0xfff;
	if (mode == 1) return source;
	if (mode != 0) return 0; /* DDR/HS400 are preserved, not reinterpreted as SDR. */
	return div ? source / (4 * div) : source / 2;
}
static uint32_t lm_clock_bits(uint64_t source, uint64_t limit)
{
	if (source <= limit) return 1u << 20;
	if (source / 2 <= limit) return 0;
	uint64_t div = (source + 4 * limit - 1) / (4 * limit);
	return div <= 0xfff ? (uint32_t)div << 8 : UINT32_MAX;
}
static int lm_program_clock(struct lancelot_msdc *m, uint32_t cfg)
{
	int status = lm_wait_clear(m, LM_STATUS, 3);
	if (status != LM_OK) return status;
	uint32_t current = m->read(m->ctx, LM_CFG);
	m->write(m->ctx, LM_CFG, current & ~2u); /* pause the outgoing clock while changing its divisor */
	m->write(m->ctx, LM_CFG, cfg & ~2u);
	m->write(m->ctx, LM_CFG, cfg);
	return lm_wait_value(m, LM_CFG, LM_CLOCK_STABLE, LM_CLOCK_STABLE);
}
static int lm_switch_width(struct lancelot_msdc *m, uint32_t width)
{
	uint32_t argument = (3u << 24) | (183u << 16) | (lm_width_code(width) << 8);
	int status = lm_command(m, 6u | (7u << 7), argument);
	if (status == LM_OK) return lm_card_ready(m); /* SWITCH_ERROR may only appear after R1b busy */
	if (status == LM_SWITCH_REJECTED) (void)lm_card_ready(m);
	return status;
}
static int
lm_check_mode(struct lancelot_msdc *m, const uint8_t original[512], unsigned width,
    int (*verify)(void *), void *ctx)
{
	uint8_t current[512];
	/* Compare read-only fields, not the mode segment: BUS_WIDTH need not
	 * read back its written value. Successful data CRC + the known-data hash
	 * prove that card and host agree about the selected width. */
	if (lm_width(m->read(m->ctx, LM_BUS_CFG)) != width || lancelot_msdc_ext_csd(m, current) != LM_OK ||
	    current[192] != original[192] || current[194] != original[194] || current[196] != original[196]) return 0;
	for (unsigned i = 212; i < 216; i++) if (current[i] != original[i]) return 0;
	return verify(ctx);
}
/* Return OK only with a proven mode. Failure leaves the caller responsible for
 * refusing storage access; the routine never assumes a rollback worked. */
static int
lancelot_msdc_negotiate(struct lancelot_msdc *m, unsigned max_width, uint64_t source, unsigned timing,
    int (*verify)(void *), void *ctx, struct lancelot_mmc_mode *result)
{
	uint8_t ext[512];
	uint32_t saved_interrupts = m->read(m->ctx, LM_INTEN);
	uint32_t host_width = m->read(m->ctx, LM_BUS_CFG);
	uint32_t cfg = m->read(m->ctx, LM_CFG);
	result->width = lm_width(host_width);
	result->clock_hz = lm_clock_hz(cfg, source);
	result->timing = timing; result->device_type = result->fallbacks = 0;
	if (!verify || !result->width || (max_width != 1 && max_width != 4 && max_width != 8) ||
	    source > 1000000000 || timing > 3 || !m->rca || m->rca > 65535) return LM_CONFIG;
	m->write(m->ctx, LM_INTEN, saved_interrupts & ~LM_EVENTS);
	int status = LM_IO;
	if (lm_card_ready(m) != LM_OK || lancelot_msdc_ext_csd(m, ext) != LM_OK) goto done;
	result->device_type = ext[196];
	/* An inherited faster/tuned or DDR mode already passed the caller's initial
	 * read. Leave it intact rather than guessing its sampling/voltage setup. */
	if (result->timing > 1 || ((cfg >> 20) & 3) > 1 || result->clock_hz > 52000000) {
		status = verify(ctx) ? LM_OK : LM_IO;
		goto done;
	}
	for (unsigned desired = max_width; desired > result->width; desired = desired == 8 ? 4 : 1) {
		unsigned previous = result->width;
		uint32_t previous_host = m->read(m->ctx, LM_BUS_CFG);
		status = lm_switch_width(m, desired);
		if (status == LM_OK) {
			m->write(m->ctx, LM_BUS_CFG, (previous_host & ~(3u << 16)) | (lm_width_code(desired) << 16));
			if (lm_check_mode(m, ext, desired, verify, ctx)) { result->width = desired; break; }
		}
		result->fallbacks++;
		if (status == LM_SWITCH_REJECTED && lm_check_mode(m, ext, previous, verify, ctx)) continue;
		/* CMD uses one wire irrespective of data width, so switch the card back
		 * before restoring the host's data-width field. Verify both afterwards. */
		if (lm_switch_width(m, previous) != LM_OK) { status = LM_IO; goto done; }
		m->write(m->ctx, LM_BUS_CFG, previous_host);
		if (!lm_check_mode(m, ext, previous, verify, ctx)) { status = LM_IO; goto done; }
	}
	/* Existing HS_TIMING=1 allows 52 MHz when DEVICE_TYPE advertises it.
	 * Legacy SDR stays within 26 MHz. No parent PLL or voltage is changed. */
	uint64_t limit = result->timing == 1 && (ext[196] & 2) ? 52000000 : 26000000;
	if (source && (ext[196] & 3)) {
		uint32_t bits = lm_clock_bits(source, limit);
		uint32_t candidate = (cfg & ~((3u << 20) | (0xfffu << 8) | (1u << 22))) | bits;
		if (bits != UINT32_MAX && lm_clock_hz(candidate, source) > result->clock_hz) {
			status = lm_program_clock(m, candidate);
			if (status == LM_OK && lm_check_mode(m, ext, result->width, verify, ctx)) {
				result->clock_hz = lm_clock_hz(candidate, source);
			} else {
				result->fallbacks++;
				if (lm_program_clock(m, cfg) != LM_OK ||
				    !lm_check_mode(m, ext, result->width, verify, ctx)) { status = LM_IO; goto done; }
			}
		}
	}
	status = lm_check_mode(m, ext, result->width, verify, ctx) ? LM_OK : LM_IO;
done:
	m->write(m->ctx, LM_INT, LM_EVENTS);
	m->write(m->ctx, LM_INTEN, saved_interrupts);
	return status;
}
#endif
