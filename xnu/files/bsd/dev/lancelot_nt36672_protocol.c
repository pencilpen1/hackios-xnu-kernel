#include "lancelot_nt36672_protocol.h"
#include <stddef.h>

#define REDWOOD_TOUCH_DOWN 1u
#define REDWOOD_TOUCH_MOVE 2u
#define REDWOOD_TOUCH_UP   3u

static void
zero_report(struct lancelot_nt36672_report *report)
{
	unsigned i;

	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		report->contact[i].present = false;
		report->contact[i].id = (uint8_t)(i + 1);
		report->contact[i].event = 0;
		report->contact[i].x = 0;
		report->contact[i].y = 0;
		report->contact[i].width = 0;
		report->contact[i].pressure = 0;
	}
	report->present_mask = 0;
	report->first_id = 0;
	report->first_event = 0;
}

bool
lancelot_nt36672_checksum_ok(const uint8_t *packet, uint32_t length)
{
	uint8_t sum = 0;
	uint32_t i;

	if (packet == NULL || length != LANCELOT_NT36672_PACKET_BYTES) {
		return false;
	}
	for (i = 1; i < LANCELOT_NT36672_PACKET_BYTES; i++) {
		sum = (uint8_t)(sum + packet[i]);
	}
	return sum == 0;
}

static bool
handshake_pattern(const uint8_t *packet)
{
	unsigned i;

	for (i = 1; i < 7; i++) {
		if (packet[i] != 0x77) {
			return false;
		}
	}
	return true;
}

static bool
wdt_pattern(const uint8_t *packet)
{
	unsigned i;

	for (i = 1; i < 7; i++) {
		if (packet[i] != 0xfd && packet[i] != 0xfe) {
			return false;
		}
	}
	return true;
}

enum lancelot_nt36672_packet_result
lancelot_nt36672_parse(const uint8_t *packet, uint32_t length,
	struct lancelot_nt36672_report *report)
{
	unsigned i;

	if (packet == NULL || report == NULL ||
	    length != LANCELOT_NT36672_PACKET_BYTES) {
		return LANCELOT_NT36672_PACKET_BAD_ARGUMENT;
	}
	zero_report(report);
	if (handshake_pattern(packet)) {
		return LANCELOT_NT36672_PACKET_RECOVERY;
	}
	if (wdt_pattern(packet)) {
		return LANCELOT_NT36672_PACKET_WDT_PATTERN;
	}
	if (!lancelot_nt36672_checksum_ok(packet, length)) {
		return LANCELOT_NT36672_PACKET_BAD_CHECKSUM;
	}

	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		const uint8_t *slot = packet + 1 + i * 6;
		uint8_t id = (uint8_t)(slot[0] >> 3);
		uint8_t event = (uint8_t)(slot[0] & 0x07);
		uint16_t x, y;
		struct lancelot_nt36672_contact *contact;

		if (id == 0 || id > LANCELOT_NT36672_MAX_CONTACTS) {
			continue;
		}
		if (event != 1 && event != 2) {
			continue;
		}
		/* A repeated active controller ID makes release accounting ambiguous. */
		if ((report->present_mask & (uint16_t)(1u << (id - 1))) != 0) {
			return LANCELOT_NT36672_PACKET_MALFORMED;
		}
		x = (uint16_t)(((uint16_t)slot[1] << 4) | (slot[3] >> 4));
		y = (uint16_t)(((uint16_t)slot[2] << 4) | (slot[3] & 0x0f));
		if (x > LANCELOT_NT36672_MAX_X || y > LANCELOT_NT36672_MAX_Y) {
			return LANCELOT_NT36672_PACKET_MALFORMED;
		}
		contact = &report->contact[id - 1];
		contact->present = true;
		contact->id = id;
		contact->event = event;
		contact->x = x;
		contact->y = y;
		contact->width = slot[4] == 0 ? 1 : slot[4];
		/* The vendor packet carries the high pressure byte for the first two slots. */
		contact->pressure = slot[5];
		if (i < 2) {
			contact->pressure |= (uint16_t)packet[63 + i] << 8;
			if (contact->pressure > 1000) {
				contact->pressure = 1000;
			}
		}
		if (contact->pressure == 0) {
			contact->pressure = 1;
		}
		report->present_mask |= (uint16_t)(1u << (id - 1));
		if (report->first_id == 0) {
			report->first_id = id;
			report->first_event = event;
		}
	}
	return LANCELOT_NT36672_PACKET_OK;
}

void
lancelot_nt36672_state_init(struct lancelot_nt36672_state *state)
{
	unsigned i;

	if (state == NULL) {
		return;
	}
	state->bad_packets = 0;
	state->wdt_packets = 0;
	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		state->down[i] = false;
		state->x[i] = state->y[i] = 0;
	}
}

void
lancelot_nt36672_state_release(struct lancelot_nt36672_state *state,
	lancelot_nt36672_emit emit, void *context)
{
	unsigned i;

	if (state == NULL) {
		return;
	}
	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		if (state->down[i]) {
			if (emit != NULL) {
				emit(context, REDWOOD_TOUCH_UP, i, state->x[i], state->y[i]);
			}
			state->down[i] = false;
		}
	}
}

void
lancelot_nt36672_state_bad_packet(struct lancelot_nt36672_state *state,
	lancelot_nt36672_emit emit, void *context)
{
	if (state == NULL) {
		return;
	}
	if (state->bad_packets < 255) {
		state->bad_packets++;
	}
	if (state->bad_packets >= LANCELOT_NT36672_BAD_LIMIT) {
		lancelot_nt36672_state_release(state, emit, context);
		state->bad_packets = 0;
	}
}

bool
lancelot_nt36672_state_wdt_recovery(struct lancelot_nt36672_state *state,
	const uint8_t *packet, uint32_t length, lancelot_nt36672_emit emit,
	void *context)
{
	if (state == NULL || packet == NULL ||
	    length != LANCELOT_NT36672_PACKET_BYTES) {
		return false;
	}
	if (wdt_pattern(packet)) {
		if (state->wdt_packets < 255) {
			state->wdt_packets++;
		}
		if (state->wdt_packets > 10) {
			state->wdt_packets = 0;
			lancelot_nt36672_state_release(state, emit, context);
			return true;
		}
	} else {
		state->wdt_packets = 0;
	}
	return false;
}

bool
lancelot_nt36672_state_consume(struct lancelot_nt36672_state *state,
	const struct lancelot_nt36672_report *report,
	lancelot_nt36672_emit emit, void *context)
{
	unsigned i;

	if (state == NULL || report == NULL) {
		return false;
	}
	if ((report->present_mask & (uint16_t)~0x03ffu) != 0) {
		return false;
	}
	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		const struct lancelot_nt36672_contact *contact = &report->contact[i];
		bool mask_present = (report->present_mask & (uint16_t)(1u << i)) != 0;
		if (contact->present != mask_present ||
		    (mask_present && (contact->id != i + 1 ||
		    (contact->event != 1 && contact->event != 2) ||
		    contact->x > LANCELOT_NT36672_MAX_X ||
		    contact->y > LANCELOT_NT36672_MAX_Y))) {
			return false;
		}
	}
	state->bad_packets = 0;
	state->wdt_packets = 0;
	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		const struct lancelot_nt36672_contact *contact = &report->contact[i];
		if (!contact->present) {
			continue;
		}
		if (!state->down[i]) {
			if (emit != NULL) {
				emit(context, REDWOOD_TOUCH_DOWN, i, contact->x, contact->y);
			}
			state->down[i] = true;
		} else if (state->x[i] != contact->x || state->y[i] != contact->y) {
			if (emit != NULL) {
				emit(context, REDWOOD_TOUCH_MOVE, i, contact->x, contact->y);
			}
		}
		state->x[i] = contact->x;
		state->y[i] = contact->y;
	}
	for (i = 0; i < LANCELOT_NT36672_MAX_CONTACTS; i++) {
		if (state->down[i] &&
		    (report->present_mask & (uint16_t)(1u << i)) == 0) {
			if (emit != NULL) {
				emit(context, REDWOOD_TOUCH_UP, i, state->x[i], state->y[i]);
			}
			state->down[i] = false;
		}
	}
	return true;
}
