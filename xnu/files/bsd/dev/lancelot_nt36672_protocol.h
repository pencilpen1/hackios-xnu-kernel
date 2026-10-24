/* NT36672A report format and contact state machine.
 *
 * This file deliberately has no XNU or hardware dependency.  The same bounded
 * parser can be shared by the future hardware driver and host/device selftests.
 * There is not yet a hardware transport calling it.
 */
#ifndef _LANCELOT_NT36672_PROTOCOL_H_
#define _LANCELOT_NT36672_PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>

#define LANCELOT_NT36672_PACKET_BYTES 66u
#define LANCELOT_NT36672_DATA_BYTES 65u
#define LANCELOT_NT36672_MAX_CONTACTS 10u
#define LANCELOT_NT36672_MAX_X 1080u
#define LANCELOT_NT36672_MAX_Y 2340u
#define LANCELOT_NT36672_BAD_LIMIT 3u

enum lancelot_nt36672_packet_result {
	LANCELOT_NT36672_PACKET_OK = 0,
	LANCELOT_NT36672_PACKET_BAD_ARGUMENT = 1,
	LANCELOT_NT36672_PACKET_BAD_CHECKSUM = 2,
	LANCELOT_NT36672_PACKET_WDT_PATTERN = 3,
	LANCELOT_NT36672_PACKET_RECOVERY = 4,
	LANCELOT_NT36672_PACKET_MALFORMED = 5,
	LANCELOT_NT36672_PACKET_PALM = 6,
};

struct lancelot_nt36672_contact {
	bool present;
	uint8_t id;                         /* controller ID: 1..10 */
	uint8_t event;                      /* 1 enter, 2 move */
	uint16_t x, y;                      /* physical panel coordinates */
	uint8_t width;
	uint16_t pressure;
};

struct lancelot_nt36672_report {
	bool valid;                        /* only a complete, accepted contact frame */
	uint16_t present_mask;
	uint8_t first_id;
	uint8_t first_event;
	struct lancelot_nt36672_contact contact[LANCELOT_NT36672_MAX_CONTACTS];
};

typedef void (*lancelot_nt36672_emit)(void *context, uint32_t type,
	uint32_t finger, uint32_t x, uint32_t y);

struct lancelot_nt36672_state {
	bool down[LANCELOT_NT36672_MAX_CONTACTS];
	uint16_t x[LANCELOT_NT36672_MAX_CONTACTS];
	uint16_t y[LANCELOT_NT36672_MAX_CONTACTS];
	uint8_t bad_packets;
	uint8_t wdt_packets;
};

/* Vendor CTP_SPI_READ output: command/offset at [0], 65 data bytes at [1..65].
 * Raw SPI RX is 67 bytes (two leading bytes); it must be unpacked first. */
bool lancelot_nt36672_checksum_ok(const uint8_t *packet, uint32_t length);
enum lancelot_nt36672_packet_result lancelot_nt36672_parse(
	const uint8_t *packet, uint32_t length,
	struct lancelot_nt36672_report *report);

/* type values are REDWOOD_TOUCH_* but remain numeric here for host tests. */
void lancelot_nt36672_state_init(struct lancelot_nt36672_state *state);
void lancelot_nt36672_state_release(struct lancelot_nt36672_state *state,
	lancelot_nt36672_emit emit, void *context);
bool lancelot_nt36672_state_consume(struct lancelot_nt36672_state *state,
	const struct lancelot_nt36672_report *report,
	lancelot_nt36672_emit emit, void *context);
void lancelot_nt36672_state_bad_packet(struct lancelot_nt36672_state *state,
	lancelot_nt36672_emit emit, void *context);
bool lancelot_nt36672_state_wdt_recovery(struct lancelot_nt36672_state *state,
	const uint8_t *packet, uint32_t length, lancelot_nt36672_emit emit,
	void *context);

#endif
