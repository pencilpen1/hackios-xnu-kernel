/* Bounded NT36672A SPI framing and RAM host-download. No GPIO reset or flash
 * programming commands. Only the observed Dijing firmware/trim is accepted. */
#ifndef LANCELOT_NT36672_HW_H
#define LANCELOT_NT36672_HW_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define LN_FW_BYTES 118784u
#define LN_FW_USED 110592u
#define LN_FW_PARTS 32u
#define LN_EVENT 0x21c00u
#define LN_TRIM 0x1f64eu
#define LN_RESET 0x3f0feu
#define LN_FAST 0x3f310u
enum { LN_OK = 0, LN_ARGUMENT = -10, LN_IO = -11, LN_ID = -12, LN_FIRMWARE = -13, LN_BOOT = -14, LN_CRC = -15 };
struct ln_partition { uint32_t bin, sram, last, crc; }; /* last is inclusive, 0 means reserved */
struct ln_firmware { unsigned count; struct ln_partition part[LN_FW_PARTS]; };
struct ln_device {
	void *ctx;
	int (*transfer)(void *, const uint8_t *, uint8_t *, uint32_t);
	void (*delay_us)(void *, uint32_t);
	bool identified, running;
	uint8_t trim[6], version, reset_state, crc_status;
	uint32_t ilm_crc, dlm_crc;
	uint32_t last_address, last_length;
	bool last_write;
};
int ln_read(struct ln_device *, uint32_t address, uint8_t *data, uint32_t length);
int ln_write(struct ln_device *, uint32_t address, const uint8_t *data, uint32_t length);
int ln_probe(struct ln_device *);
int ln_validate_firmware(const uint8_t *, size_t, struct ln_firmware *);
int ln_boot_firmware(struct ln_device *, const uint8_t *, size_t);
int ln_read_report(struct ln_device *, uint8_t packet[66]);
#endif
