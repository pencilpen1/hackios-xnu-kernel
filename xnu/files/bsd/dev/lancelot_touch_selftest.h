/* Pure in-memory fixtures shared by host sanitizers and opt-in XNU boot test.
 * No MMIO, firmware access, device writes, HID injection or return timer. */
#ifndef _LANCELOT_TOUCH_SELFTEST_H_
#define _LANCELOT_TOUCH_SELFTEST_H_
#include "lancelot_nt36672_protocol.h"
#include <stddef.h>

struct lt_test_sink {
	bool down[10], failed;
	uint32_t downs, moves, ups;
};

static void
lt_test_emit(void *context, uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	struct lt_test_sink *s = context;
	if (finger >= 10 || x > 1080 || y > 2340) { s->failed = true; return; }
	if (type == 1) {
		if (s->down[finger]) s->failed = true;
		s->down[finger] = true; s->downs++;
	} else if (type == 2) {
		if (!s->down[finger]) s->failed = true;
		s->moves++;
	} else if (type == 3) {
		if (!s->down[finger]) s->failed = true;
		s->down[finger] = false; s->ups++;
	} else s->failed = true;
}

static void
lt_test_checksum(uint8_t *p)
{
	uint8_t sum = 0;
	for (unsigned i = 1; i < 65; i++) sum = (uint8_t)(sum + p[i]);
	p[65] = (uint8_t)(0u - sum);
}

static void
lt_test_slot(uint8_t *p, unsigned slot, unsigned id, unsigned event, unsigned x, unsigned y)
{
	uint8_t *v = p + 1 + 6 * slot;
	v[0] = (uint8_t)((id << 3) | event);
	v[1] = (uint8_t)(x >> 4); v[2] = (uint8_t)(y >> 4);
	v[3] = (uint8_t)(((x & 15) << 4) | (y & 15));
	v[4] = 0; v[5] = 0;             /* width/pressure minimum handling */
}

/* Returns 0 on success, otherwise the failing fixture's source line. */
static unsigned
lancelot_touch_protocol_selftest(void)
{
	uint8_t p[66] = {0}, short_packet[1] = {0};
	struct lancelot_nt36672_report r;
	struct lancelot_nt36672_state state;
	struct lt_test_sink sink = {0};
#define LT_CHECK(test) do { if (!(test)) return __LINE__; } while (0)
	lancelot_nt36672_state_init(&state);
	LT_CHECK(lancelot_nt36672_parse(short_packet, 1, &r) == LANCELOT_NT36672_PACKET_BAD_ARGUMENT);
	LT_CHECK(!r.valid && !lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	LT_CHECK(!lancelot_nt36672_state_wdt_recovery(&state, short_packet, 1, lt_test_emit, &sink));
	for (unsigned i = 0; i < 10; i++) lt_test_slot(p, i, 10 - i, 2, i * 120, i * 260);
	p[63] = 255; p[64] = 1;
	lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_OK);
	LT_CHECK(r.valid && r.present_mask == 1023 && r.contact[9].pressure == 1000);
	LT_CHECK(r.contact[8].pressure == 256 && r.contact[0].width == 1 && r.contact[0].pressure == 1);
	LT_CHECK(lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	LT_CHECK(sink.downs == 10 && !sink.failed);
	LT_CHECK(lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	LT_CHECK(sink.downs == 10 && sink.moves == 0);
	lt_test_slot(p, 0, 10, 2, 100, 200); lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_OK);
	LT_CHECK(lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	LT_CHECK(sink.moves == 1);
	p[65] ^= 1;
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_BAD_CHECKSUM);
	LT_CHECK(!r.valid && !lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	lancelot_nt36672_state_bad_packet(&state, lt_test_emit, &sink);
	lancelot_nt36672_state_bad_packet(&state, lt_test_emit, &sink);
	LT_CHECK(sink.ups == 0);
	lancelot_nt36672_state_bad_packet(&state, lt_test_emit, &sink);
	LT_CHECK(sink.ups == 10 && !sink.failed);
	lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_OK);
	LT_CHECK(lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	LT_CHECK(sink.downs == 20);
	for (unsigned i = 1; i < 7; i++) p[i] = i & 1 ? 0xfd : 0xfe;
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_WDT_PATTERN);
	for (unsigned i = 0; i < 10; i++) LT_CHECK(!lancelot_nt36672_state_wdt_recovery(&state, p, 66, lt_test_emit, &sink));
	LT_CHECK(!lancelot_nt36672_state_wdt_recovery(&state, short_packet, 1, lt_test_emit, &sink));
	LT_CHECK(state.wdt_packets == 0);
	for (unsigned i = 0; i < 10; i++) LT_CHECK(!lancelot_nt36672_state_wdt_recovery(&state, p, 66, lt_test_emit, &sink));
	LT_CHECK(lancelot_nt36672_state_wdt_recovery(&state, p, 66, lt_test_emit, &sink));
	LT_CHECK(sink.ups == 20 && state.wdt_packets == 0 && !sink.failed);
	for (unsigned i = 1; i < 7; i++) p[i] = 0x77;
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_RECOVERY);
	LT_CHECK(!r.valid);
	for (unsigned i = 0; i < 66; i++) p[i] = 0;
	p[1] = 30u << 3; p[2] = 4; p[3] = 3; lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_PALM && !r.valid);
	p[3] = 4; lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_PALM);
	p[3] = 5; lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_MALFORMED);
	for (unsigned i = 0; i < 66; i++) p[i] = 0;
	lt_test_slot(p, 0, 1, 1, 1081, 0); lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_MALFORMED);
	lt_test_slot(p, 0, 1, 1, 1080, 2341); lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_MALFORMED);
	lt_test_slot(p, 0, 1, 1, 1080, 2340); lt_test_slot(p, 1, 1, 2, 0, 0); lt_test_checksum(p);
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_MALFORMED && !r.valid);
	for (unsigned i = 0; i < 66; i++) p[i] = 0;
	LT_CHECK(lancelot_nt36672_parse(p, 66, &r) == LANCELOT_NT36672_PACKET_OK);
	LT_CHECK(lancelot_nt36672_state_consume(&state, &r, lt_test_emit, &sink));
	LT_CHECK(!sink.failed && sink.downs == sink.ups);
	return 0;
#undef LT_CHECK
}
#endif
