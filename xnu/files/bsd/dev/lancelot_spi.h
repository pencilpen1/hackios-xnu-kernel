/* MT6768 SPI0, MT6765-compatible enhanced timing; FIFO only, no DMA.
 * Caller owns the controller, supplies clocks/pins and serializes access.
 * Register layout: pinned Shockwave spi-mt65xx.c; polled pause/finish scheme
 * also used by MediaTek's coreboot common/spi.c. Hardware still needs testing.
 */
#ifndef LANCELOT_SPI_H
#define LANCELOT_SPI_H
#include <stdint.h>
#include <stddef.h>

#define LS_CFG0 0x00u
#define LS_CFG1 0x04u
#define LS_TX 0x10u
#define LS_RX 0x14u
#define LS_CMD 0x18u
#define LS_STATUS 0x1cu
#define LS_PAD 0x24u
#define LS_CFG2 0x28u
#define LS_ACT 1u
#define LS_RESUME 2u
#define LS_RESET 4u
#define LS_PAUSE 16u
#define LS_MSB ((1u << 12) | (1u << 13))
#define LS_IE ((1u << 16) | (1u << 17))
#define LS_MAX_TRANSFER 258u
enum { LS_OK = 0, LS_ARGUMENT = -1, LS_TIMEOUT = -2, LS_STATUS_ERROR = -3 };

struct lancelot_spi {
	void *ctx;
	uint32_t (*read)(void *, uint32_t);
	void (*write)(void *, uint32_t, uint32_t);
	uint64_t (*ticks)(void *);
	uint64_t timeout;                 /* per entire message; nonzero, monotonic ticks */
	uint32_t cfg0, cfg1, cfg2, last_status;
	uint64_t messages, failures;
};

static inline int
lancelot_spi_configure(struct lancelot_spi *s, uint32_t source_hz, uint32_t bus_hz)
{
	if (!s || !s->read || !s->write || !s->ticks || !s->timeout ||
	    source_hz != 26000000 || bus_hz < 100000 || bus_hz > 8000000) return LS_ARGUMENT;
	uint32_t half = (source_hz + 2 * bus_hz - 1) / (2 * bus_hz);
	uint32_t cs = half * 2;
	if (!half || half > 65536 || cs > 256) return LS_ARGUMENT;
	s->cfg0 = (cs - 1) | (5u << 16); /* >=230ns setup = 6 cycles at 26MHz */
	s->cfg1 = cs - 1;                /* tick_delay=0, packet_loop=1 */
	s->cfg2 = (half - 1) | ((half - 1) << 16);
	return LS_OK;
}

static inline void
lancelot_spi_reset(struct lancelot_spi *s)
{
	s->write(s->ctx, LS_CMD, LS_MSB | LS_RESET);
	s->write(s->ctx, LS_CMD, LS_MSB);
}

static inline int
lancelot_spi_transfer(struct lancelot_spi *s, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	if (!s || !s->read || !s->write || !s->ticks || !s->timeout || !tx ||
	    !length || length > LS_MAX_TRANSFER || !s->cfg0 || !s->cfg2) return LS_ARGUMENT;
	uint64_t start = s->ticks(s->ctx);
	lancelot_spi_reset(s);
	s->write(s->ctx, LS_CFG0, s->cfg0);
	s->write(s->ctx, LS_CFG2, s->cfg2);
	s->write(s->ctx, LS_PAD, 0);      /* DT pad-select=0, GPIO32..35 */
	(void)s->read(s->ctx, LS_STATUS); /* read-to-clear stale completion before ACT */
	int result = LS_OK;
	for (uint32_t at = 0; at < length;) {
		uint32_t n = length - at < 32 ? length - at : 32;
		s->write(s->ctx, LS_CFG1, s->cfg1 | ((n - 1) << 16));
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t word = 0;
			for (uint32_t j = 0; j < 4 && i + j < n; j++) word |= (uint32_t)tx[at + i + j] << (8 * j);
			s->write(s->ctx, LS_TX, word);
		}
		/* Keep CS asserted across FIFO pauses, including the final chunk.
		 * Clear PAUSE and reset only when all RX words have been drained. */
		s->write(s->ctx, LS_CMD, LS_MSB | LS_IE | LS_PAUSE | (at ? LS_RESUME : LS_ACT));
		for (;;) {
			s->last_status = s->read(s->ctx, LS_STATUS);
			if (s->last_status & 3u) break;
			if (s->ticks(s->ctx) - start >= s->timeout) { result = LS_TIMEOUT; goto done; }
		}
		/* With PAUSE enabled, completion must be PAUSED, not an early FINISH. */
		if ((s->last_status & 3u) != 2u) { result = LS_STATUS_ERROR; goto done; }
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t word = s->read(s->ctx, LS_RX);
			if (rx) for (uint32_t j = 0; j < 4 && i + j < n; j++) rx[at + i + j] = (uint8_t)(word >> (8 * j));
		}
		at += n;
		if (s->ticks(s->ctx) - start >= s->timeout) { result = LS_TIMEOUT; break; }
	}
done:
	lancelot_spi_reset(s);           /* DMA remains disabled; interrupts no longer raised */
	s->messages++;
	if (result != LS_OK) s->failures++;
	return result;
}
#endif
