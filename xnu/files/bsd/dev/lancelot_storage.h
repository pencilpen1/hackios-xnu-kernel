/* Firmware-to-kernel metadata only; no callbacks or shared request mailbox. */
#ifndef LANCELOT_STORAGE_H
#define LANCELOT_STORAGE_H
#include <stdint.h>
#define LANCELOT_STORAGE_MAGIC 0x314344534d4b544dULL
#define LANCELOT_UBUNTU_BYTES 33285963776ULL
#define LANCELOT_IOS_BYTES 8388608000ULL
struct lancelot_storage {
	uint64_t magic;
	uint32_t version, size;
	uint64_t mmio, partition_offset, partition_bytes;
	uint8_t superblock_sha256[32];
};
_Static_assert(sizeof(struct lancelot_storage) == 72, "loader/kernel storage handoff ABI");
#endif
