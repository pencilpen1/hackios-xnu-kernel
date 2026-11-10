/* Lancelot MT6768 WLAN: loader reservation and per-device calibration ABI.
 * The addresses and sizes are the shipping Android DT/runtime allocations.
 * Firmware and factory data are supplied by the owner, never compiled here.
 */
#ifndef LANCELOT_WIFI_CONTRACT_H
#define LANCELOT_WIFI_CONTRACT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define LW_EMI_PHYS 0xbf000000u
#define LW_EMI_SIZE 0x00400000u
#define LW_DMA_PHYS 0xbe000000u
#define LW_DMA_SIZE 0x00300000u
#define LW_BOARD_MAGIC 0x3142574cu
#define LW_HANDOFF_MAGIC 0x3148574cu
#define LW_ABI_VERSION 1u
#define LW_RESERVED 1u
#define LW_NVRAM_SIZE 2048u
#define LW_FIRMWARE_FILES 6u

struct lw_board {
    uint32_t magic, version;
    uint8_t mac[6], country[2];
    uint16_t bt_tssi;
    uint8_t bt_tssi_from_wifi, ant_mode;
    uint8_t co_clock, wifi_ant_swap, wifi_main_polarity, reserved;
    uint32_t nvram_size;
    uint8_t firmware_sha256[LW_FIRMWARE_FILES][32];
    uint32_t crc;
};

static inline const char *
lw_firmware_name(unsigned index)
{
    static const char *const names[LW_FIRMWARE_FILES] = {
        "soc1_0_ram_mcu_1a_1_hdr.bin", "soc1_0_ram_bt_1a_1_hdr.bin",
        "soc1_0_ram_wifi_1a_1_hdr.bin", "soc1_0_patch_mcu_1a_1_hdr.bin",
        "WIFI_RAM_CODE_soc1_0_1a_1.bin", "WIFI",
    };
    return index < LW_FIRMWARE_FILES ? names[index] : NULL;
}

struct lw_handoff {
    uint32_t magic, version;
    uint64_t emi_phys, emi_size, dma_phys, dma_size;
    struct lw_board board;
    uint32_t flags, crc;
};

_Static_assert(sizeof(struct lw_board) == 224, "Wi-Fi board ABI");
_Static_assert(sizeof(struct lw_handoff) == 272, "Wi-Fi reservation ABI");

static inline uint32_t
lw_crc(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = ~0u;
    for (size_t i = 0; i < length; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static inline bool
lw_mac_valid(const uint8_t *mac)
{
    return !(mac[0] & 1) &&
        (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

static inline bool
lw_board_valid(const struct lw_board *b)
{
    if (!b) return false;
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        uint8_t present = 0;
        for (unsigned j = 0; j < 32; j++) present |= b->firmware_sha256[i][j];
        if (!present) return false;
    }
    bool country = (b->country[0] == '0' && b->country[1] == '0') ||
        (b->country[0] == 'C' && b->country[1] == 'N');
    return b->magic == LW_BOARD_MAGIC && b->version == LW_ABI_VERSION &&
        lw_mac_valid(b->mac) && country && b->bt_tssi == 3719 &&
        b->bt_tssi_from_wifi == 3 && b->ant_mode == 1 && b->co_clock == 1 &&
        !b->wifi_ant_swap && !b->wifi_main_polarity && !b->reserved &&
        b->nvram_size == LW_NVRAM_SIZE && b->crc == lw_crc(b, offsetof(struct lw_board, crc));
}

static inline bool
lw_overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    if (!an || !bn || an > UINT64_MAX - a || bn > UINT64_MAX - b) return true;
    return a < b + bn && b < a + an;
}

static inline bool
lw_handoff_valid(const struct lw_handoff *h, uint64_t kernel, uint64_t kernel_size)
{
    return h && h->magic == LW_HANDOFF_MAGIC && h->version == LW_ABI_VERSION &&
        h->flags == LW_RESERVED && h->emi_phys == LW_EMI_PHYS && h->emi_size == LW_EMI_SIZE &&
        h->dma_phys == LW_DMA_PHYS && h->dma_size == LW_DMA_SIZE &&
        lw_board_valid(&h->board) &&
        h->crc == lw_crc(h, offsetof(struct lw_handoff, crc)) &&
        !lw_overlaps(h->emi_phys, h->emi_size, kernel, kernel_size) &&
        !lw_overlaps(h->dma_phys, h->dma_size, kernel, kernel_size);
}

/* Exact, unique decimal option. Unknown/malformed/duplicate values are rejected. */
static inline int
lw_option(const char *line, const char *key, unsigned *value)
{
    unsigned found = 0;
    for (const char *p = line; *p;) {
        while (*p == ' ') p++;
        const char *end = p;
        while (*end && *end != ' ') end++;
        const char *q = p, *k = key;
        while (*k && q < end && *q == *k) { q++; k++; }
        if (!*k && q < end && *q == '=') {
            if (++found > 1 || ++q == end) return -1;
            unsigned n = 0;
            for (; q < end; q++) {
                if (*q < '0' || *q > '9' || n > 100) return -1;
                n = n * 10 + (unsigned)(*q - '0');
            }
            *value = n;
        }
        p = end;
    }
    return (int)found;
}

/* wifi=1 selects the complete native station; connsys_step=1..17 is diagnostic. */
static inline int
lw_boot_step(const char *line)
{
    unsigned wifi = 0, step = 0;
    int w = lw_option(line, "wifi", &wifi), s = lw_option(line, "connsys_step", &step);
    if (w < 0 || s < 0 || wifi > 1 || (s && (!step || step > 17))) return -1;
    return s ? (int)step : wifi ? 17 : 0;
}
#endif
