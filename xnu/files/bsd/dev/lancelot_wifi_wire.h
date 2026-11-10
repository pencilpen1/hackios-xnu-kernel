/* Checked formats used by MT6768 CONNSYS and CONNAC gen4m.
 * Layout sources: gen4m chips/common/fw_dl.c, fw_dl.h and STP common/core.
 * Kept freestanding so malformed firmware and DMA input can be tested on host.
 */
#ifndef LANCELOT_WIFI_WIRE_H
#define LANCELOT_WIFI_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lancelot_wifi_contract.h"

static inline uint32_t lw_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline bool
lw_wmt_frame_valid(const uint8_t *data, size_t length, uint8_t type)
{
    return data && length >= 4 && data[0] == type &&
        ((size_t)data[2] | (size_t)data[3] << 8) == length - 4;
}

/* WMT_QUERY_A_DIE_EVT matches only its five-byte header/status in Android
 * (common_main/core/wmt_ic_soc.c). This phone returns 01 8b 31 66 after it:
 * chip 0x6631, revision 0x8b01, not a fixed little-endian 0x00006631 word.
 * Keep the board's chip-family check; the revision is reported, not guessed. */
static inline bool
lw_wmt_adie(const uint8_t *reply, size_t length, uint16_t *revision)
{
    if (!lw_wmt_frame_valid(reply, length, 2) || length != 9 || reply[1] != 2 || reply[4] != 0)
        return false;
    uint32_t id = lw_get_le32(reply + 5);
    if ((id >> 16) != 0x6631) return false;
    if (revision) *revision = (uint16_t)id;
    return true;
}

static inline bool
lw_wmt_reply_matches(const uint8_t *command, size_t command_length,
    const uint8_t *reply, size_t reply_length, const uint8_t *expected, size_t expected_length)
{
    if (!lw_wmt_frame_valid(command, command_length, 1) ||
        !lw_wmt_frame_valid(reply, reply_length, 2) || reply_length < 5 ||
        command[1] != reply[1] || !expected || expected_length < 2 || expected_length > reply_length)
        return false;
    for (size_t i = 0; i < expected_length; i++) if (reply[i] != expected[i]) return false;
    if (command_length == 5 && command[1] == 2 && command[4] == 0x12)
        return lw_wmt_adie(reply, reply_length, NULL);
    /* RF calibration carries variable data, but success and its echoed
     * sub-operation must still correspond to the outstanding request. */
    return command[1] != 0x14 || (command_length >= 5 && reply_length >= 6 &&
        reply[4] == 0 && reply[5] == command[4]);
}

/* nic_txd_v1: fixed-rate-enable is DW2 bit 31; the rate is DW6[30:16],
 * with DW6 bit 31 clear to select the descriptor instead of MAC registers. */
static inline bool
lw_command_fixed_rate(uint8_t *txd, size_t size, uint16_t rate)
{
    if (!txd || size < 32) return false;
    txd[11] |= 0x80u;
    txd[26] = (uint8_t)rate;
    txd[27] = (uint8_t)((rate >> 8) & 0x7fu);
    return true;
}

/* wlanoidSetRemoveKey's CMD_802_11_KEY: TxKey, algorithm, key bytes and
 * RSC are zero for removal. WTBL 1 is the peer, WTBL 2 its group key. */
static inline size_t
lw_remove_key_command(uint8_t *command, size_t room, const uint8_t peer[6],
    bool pairwise, unsigned key_id)
{
    if (!command || !peer || room < 64 || key_id > 3 || (pairwise && key_id)) return 0;
    for (unsigned i = 0; i < 64; i++) command[i] = 0;
    command[2] = pairwise ? 1 : 0;
    for (unsigned i = 0; i < 6; i++) command[4 + i] = peer[i];
    command[12] = (uint8_t)key_id;
    command[14] = pairwise ? 1 : 2;
    return 64;
}

struct lw_fw_region { uint32_t address, length, offset; uint8_t feature; };
struct lw_fw_plan { unsigned count; uint32_t entry; struct lw_fw_region regions[8]; };

static inline bool
lw_fw_parse(const uint8_t *data, size_t length, struct lw_fw_plan *plan)
{
    if (!data || !plan || length < 76 || length > LW_EMI_SIZE) return false;
    unsigned count = data[length - 36 + 2];
    if (!count || count > 8 || length < 36 + count * 40) return false;
    size_t end = length - 36 - count * 40, offset = 0;
    plan->count = count;
    plan->entry = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *r = data + end + i * 40;
        uint32_t address = lw_get_le32(r + 16), bytes = lw_get_le32(r + 20);
        uint8_t feature = r[24];
        if (offset > end || bytes > end - offset) return false;
        if (!(feature & 0x80) && !(feature & 0x40)) {
            bool ilm = address >= 0x00050000 && address < 0x00080000 && bytes <= 0x00080000 - address;
            bool patch = address >= 0x02010000 && address < 0x02020000 && bytes <= 0x02020000 - address;
            bool dlm = address >= 0x40100000 && address < 0x40200000 && bytes <= 0x40200000 - address;
            if (!(ilm || patch || dlm) || (feature & ~0x17u)) return false;
        }
        if (feature & 0x80) {
            uint32_t at = address & 0xffffff;
            if ((address >> 24) != 0xf0 || at >= LW_EMI_SIZE || bytes > LW_EMI_SIZE - at)
                return false;
        }
        if (feature & 0x20) plan->entry = address;
        plan->regions[i] = (struct lw_fw_region){ address, bytes, (uint32_t)offset, feature };
        offset += bytes;
    }
    /* Vendor release-information TLVs may follow the region bytes. */
    return offset <= end;
}

/* ROM RAM image: 48-byte header, MT6768 E1, ALPS, one of the three fixed EMI slots. */
static inline bool
lw_rom_image(const uint8_t *data, size_t length, uint32_t expected, uint32_t *offset)
{
    if (!data || length <= 48 || length > LW_EMI_SIZE ||
        data[16] != 'A' || data[17] != 'L' || data[18] != 'P' || data[19] != 'S' ||
        data[20] != 0x8a || data[21] != 0 || data[22] != 0x8a || data[23] != 0) return false;
    uint32_t address = lw_get_le32(data + 24), at = address & 0x00ffff00;
    if ((address >> 24) != 0xf0 || (address & 0xff) != 0x11 || at != expected || length - 48 > LW_EMI_SIZE - at) return false;
    *offset = at;
    return true;
}

static inline uint16_t
lw_crc16_step(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned i = 0; i < 8; i++)
        crc = (uint16_t)((crc >> 1) ^ (0xa001u & (0u - (crc & 1u))));
    return crc;
}

static inline uint16_t
lw_stp_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < length; i++) crc = lw_crc16_step(crc, data[i]);
    return crc;
}

static inline bool
lw_stp_header_valid(const uint8_t h[4], bool full)
{
    return (h[0] & 0xc0) == 0x80 &&
        (!full || h[3] == (uint8_t)(h[0] + h[1] + h[2]));
}

static inline size_t
lw_stp_encode(uint8_t *dst, size_t room, const uint8_t *payload, size_t length,
    bool full, unsigned seq, unsigned ack)
{
    if (!dst || !payload || length > 0xfff || length + 6 > room || seq > 7 || ack > 7) return 0;
    dst[0] = (uint8_t)(0x80 | (full ? ((seq << 3) | ack) : 0));
    dst[1] = (uint8_t)(0x40 | (length >> 8));
    dst[2] = (uint8_t)length;
    dst[3] = full ? (uint8_t)(dst[0] + dst[1] + dst[2]) : 0;
    for (size_t i = 0; i < length; i++) dst[4 + i] = payload[i];
    uint16_t crc = full ? lw_stp_crc(payload, length) : 0;
    dst[length + 4] = (uint8_t)crc;
    dst[length + 5] = (uint8_t)(crc >> 8);
    return length + 6;
}

static inline bool
lw_rx_size(uint32_t descriptor, uint32_t room, uint32_t *length)
{
    uint32_t bytes = (descriptor >> 16) & 0x3fff;
    if (!(descriptor & (1u << 31)) || bytes < 16 || bytes > 3840 || bytes > room) return false;
    *length = bytes;
    return true;
}
#endif
