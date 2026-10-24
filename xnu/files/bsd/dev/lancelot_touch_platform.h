/* MT6768-only touch resources. All masks come from the pinned clock/pinctrl
 * tables. No PMIC access, no reset GPIO92, no eMMC/USB register writes.
 * Caller maps only the four fixed banks and serializes this owner.
 */
#ifndef LANCELOT_TOUCH_PLATFORM_H
#define LANCELOT_TOUCH_PLATFORM_H
#include <stdint.h>
#include <stdbool.h>
enum { LP_TOP = 0, LP_INFRA, LP_GPIO, LP_RM };
struct lancelot_touch_resources {
	void *ctx;
	uint32_t (*read)(void *, unsigned, uint32_t);
	void (*write)(void *, unsigned, uint32_t, uint32_t);
	uint64_t (*ticks)(void *);
	uint64_t timeout;
	uint32_t clock, gate, mode1, mode_spi, direction0, direction1, output1, ies;
	bool changed;
};

static inline int
lp_wait(struct lancelot_touch_resources *p, unsigned bank, uint32_t offset, uint32_t mask, uint32_t expected)
{
	uint64_t start = p->ticks(p->ctx);
	while ((p->read(p->ctx, bank, offset) & mask) != expected)
		if (p->ticks(p->ctx) - start >= p->timeout) return -1;
	return 0;
}

static inline void
lp_field(struct lancelot_touch_resources *p, unsigned bank, uint32_t offset, uint32_t mask, uint32_t value)
{
	/* Non-mode pin fields have SET/CLR aliases at +4/+8. */
	p->write(p->ctx, bank, offset + 4, value & mask);
	p->write(p->ctx, bank, offset + 8, ~value & mask);
}

static inline void
lp_modes(struct lancelot_touch_resources *p, uint32_t mode1, uint32_t spi_modes)
{
	/* MODE is different: +0xc is a masked write register, bit3 of each
	 * nibble is the write-enable. Do NOT use ordinary SET/CLR here. */
	p->write(p->ctx, LP_GPIO, 0x30c, (mode1 & 0x70) | 0x80);
	p->write(p->ctx, LP_GPIO, 0x34c, (spi_modes & 0x7777) | 0x8888);
}

static inline int
lancelot_touch_prepare_resources(struct lancelot_touch_resources *p)
{
	if (!p || !p->read || !p->write || !p->ticks || !p->timeout || p->changed) return -1;
	p->clock = p->read(p->ctx, LP_TOP, 0x60);
	p->gate = p->read(p->ctx, LP_INFRA, 0x94);
	p->mode1 = p->read(p->ctx, LP_GPIO, 0x300);
	p->mode_spi = p->read(p->ctx, LP_GPIO, 0x340);
	p->direction0 = p->read(p->ctx, LP_GPIO, 0x00);
	p->direction1 = p->read(p->ctx, LP_GPIO, 0x10);
	p->output1 = p->read(p->ctx, LP_GPIO, 0x110);
	p->ies = p->read(p->ctx, LP_RM, 0x60);
	/* A shared SPI mux cannot be retimed underneath another active bus. */
	uint32_t other = (1u << 6) | (1u << 9) | (1u << 10) | (1u << 25) | (1u << 26);
	if ((p->clock & 0x83000000) && (p->read(p->ctx, LP_INFRA, 0xac) & other) != other) return -2;
	if ((p->mode1 & 0x80) || (p->mode_spi & 0x8888)) return -3;
	p->changed = true;
	p->write(p->ctx, LP_INFRA, 0x88, 2); /* gate only SPI0 while changing its source */
	if (lp_wait(p, LP_INFRA, 0x94, 2, 2)) return -4;
	p->write(p->ctx, LP_TOP, 0x68, 0x83000000); /* SPI parent=clk26m, gate open */
	if (p->clock & 0x03000000) p->write(p->ctx, LP_TOP, 0x04, 1u << 11);
	if (lp_wait(p, LP_TOP, 0x60, 0x83000000, 0)) return -5;
	p->write(p->ctx, LP_INFRA, 0x8c, 2);
	if (lp_wait(p, LP_INFRA, 0x94, 2, 0)) return -6;
	/* Safe idle output levels before assigning SPI functions. */
	lp_field(p, LP_GPIO, 0x110, 0x0e, 2); /* CS high; MOSI/CLK low */
	lp_field(p, LP_GPIO, 0x10, 0x0f, 0x0e); /* MISO input, three outputs */
	lp_field(p, LP_GPIO, 0x00, 2, 0);       /* IRQ GPIO1 input */
	lp_field(p, LP_RM, 0x60, (1u << 31) | (1u << 8), (1u << 31) | (1u << 8));
	lp_modes(p, 0, 0x1111);               /* GPIO1=GPIO, GPIO32..35=SPI0 */
	if ((p->read(p->ctx, LP_GPIO, 0x340) & 0xffff) != 0x1111 ||
	    (p->read(p->ctx, LP_GPIO, 0x300) & 0xf0) ||
	    (p->read(p->ctx, LP_GPIO, 0x00) & 2) ||
	    (p->read(p->ctx, LP_RM, 0x60) & ((1u << 31) | (1u << 8))) != ((1u << 31) | (1u << 8))) return -7;
	return 0;
}

static inline int
lancelot_touch_restore_resources(struct lancelot_touch_resources *p)
{
	if (!p || !p->changed) return 0;
	lp_modes(p, p->mode1, p->mode_spi);
	lp_field(p, LP_GPIO, 0x110, 0x0e, p->output1);
	lp_field(p, LP_GPIO, 0x10, 0x0f, p->direction1);
	lp_field(p, LP_GPIO, 0x00, 2, p->direction0);
	lp_field(p, LP_RM, 0x60, (1u << 31) | (1u << 8), p->ies);
	p->write(p->ctx, LP_INFRA, 0x88, 2);
	p->write(p->ctx, LP_TOP, 0x68, 0x83000000);
	p->write(p->ctx, LP_TOP, 0x64, p->clock & 0x83000000);
	if (p->clock & 0x03000000) p->write(p->ctx, LP_TOP, 0x04, 1u << 11);
	p->write(p->ctx, LP_INFRA, p->gate & 2 ? 0x88 : 0x8c, 2);
	if (lp_wait(p, LP_TOP, 0x60, 0x83000000, p->clock & 0x83000000) ||
	    lp_wait(p, LP_INFRA, 0x94, 2, p->gate & 2)) return -1;
	p->changed = false;
	return 0;
}
#endif
