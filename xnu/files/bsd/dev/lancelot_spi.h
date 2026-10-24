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
#define LS_STATUS1 0x20u
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
	void (*delay_us)(void *, uint32_t);
	uint64_t timeout;                 /* per entire message; nonzero, monotonic ticks */
	uint64_t (*critical_enter)(void *);
	void (*critical_exit)(void *, uint64_t);
	uint32_t cfg0, cfg1, cfg2, last_status;
	uint32_t bus_hz;
	uint32_t last_length, completed, last_command, last_status1;
	uint64_t messages, failures;
};

static inline int
lancelot_spi_configure(struct lancelot_spi *s, uint32_t source_hz, uint32_t bus_hz)
{
	if (!s || !s->read || !s->write || !s->ticks || !s->timeout ||
	    source_hz < 5000000 || source_hz > 300000000 || bus_hz < 100000 || bus_hz > 8000000) return LS_ARGUMENT;
	uint32_t half = (source_hz + 2 * bus_hz - 1) / (2 * bus_hz);
	uint32_t cs = half * 2;
	uint32_t setup = (uint32_t)(((uint64_t)source_hz * 230 + 999999999) / 1000000000);
	if (!half || half > 65536 || cs > 256) return LS_ARGUMENT;
	s->cfg0 = (cs - 1) | ((setup - 1) << 16); /* >=230ns at the verified live source rate */
	s->cfg1 = cs - 1;                /* tick_delay=0, packet_loop=1 */
	s->cfg2 = (half - 1) | ((half - 1) << 16);
	s->bus_hz = source_hz / (2 * half);
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
	    !length || length > LS_MAX_TRANSFER || !s->cfg0 || !s->cfg2 || !s->bus_hz) return LS_ARGUMENT;
	if ((s->critical_enter == NULL) != (s->critical_exit == NULL)) return LS_ARGUMENT;
	/* One short FIFO message must not include unrelated eMMC scheduling time
	 * in its hardware deadline. The XNU adapter caps this interval at 10ms. */
	uint64_t critical = s->critical_enter ? s->critical_enter(s->ctx) : 0;
	uint64_t start = s->ticks(s->ctx);
	s->last_length = length; s->completed = 0;
	lancelot_spi_reset(s);
	/* Match Linux prepare_message/set_cs: arm completion and PAUSE before
	 * the ACT edge, not in the same write as the first short page message. */
	s->write(s->ctx, LS_CMD, LS_MSB | LS_IE | LS_PAUSE);
	(void)s->read(s->ctx, LS_CMD);
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
		uint32_t command = s->read(s->ctx, LS_CMD) & ~(LS_ACT | LS_RESUME);
		s->write(s->ctx, LS_CMD, command | (at ? LS_RESUME : LS_ACT));
		/* Do not hammer a read-to-clear event latch while the short packet
		 * is still clocking. STATUS1 first confirms ACT/RESUME was observed,
		 * as in MediaTek coreboot's polled FIFO path. Then defer the first
		 * STATUS0 read until the programmed wire time has elapsed. */
		while (!(s->read(s->ctx, LS_STATUS1) & 1u)) {
			if (s->ticks(s->ctx) - start >= s->timeout) { result = LS_TIMEOUT; goto done; }
		}
		if (s->delay_us) s->delay_us(s->ctx, (uint32_t)(((uint64_t)n * 8000000 + s->bus_hz - 1) / s->bus_hz) + 2);
		for (;;) {
			s->last_status = s->read(s->ctx, LS_STATUS);
			if (s->last_status & 3u) break;
			if (s->ticks(s->ctx) - start >= s->timeout) { result = LS_TIMEOUT; goto done; }
		}
		/* With PAUSE enabled, completion must be PAUSED, not an early FINISH. */
		if (!(s->last_status & 2u)) { result = LS_STATUS_ERROR; goto done; }
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t word = s->read(s->ctx, LS_RX);
			if (rx) for (uint32_t j = 0; j < 4 && i + j < n; j++) rx[at + i + j] = (uint8_t)(word >> (8 * j));
		}
		at += n;
		s->completed = at;
		if (s->ticks(s->ctx) - start >= s->timeout) { result = LS_TIMEOUT; break; }
	}
done:
	s->last_command = s->read(s->ctx, LS_CMD);
	s->last_status1 = s->read(s->ctx, LS_STATUS1);
	lancelot_spi_reset(s);           /* DMA remains disabled; interrupts no longer raised */
	s->messages++;
	if (result != LS_OK) s->failures++;
	if (s->critical_exit) s->critical_exit(s->ctx, critical);
	return result;
}
#endif
