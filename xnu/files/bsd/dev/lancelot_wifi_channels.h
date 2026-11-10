/* MT6768 gen4m reg_rule.c / CMD_SET_DOMAIN_INFO_V2. The initial country is
 * captured from the owner's Android framework; unknown countries use world.
 */
#ifndef LANCELOT_WIFI_CHANNELS_H
#define LANCELOT_WIFI_CHANNELS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define LW_CH_NO_IR 2u
#define LW_CH_RADAR 8u
#define LW_CH_NO_OFDM 64u
struct lw_channel { uint16_t number; uint32_t flags; };

static inline unsigned
lw_channels_for_bands(const uint8_t country[2], struct lw_channel *out, unsigned capacity, bool band5)
{
    static const uint16_t five[] = {36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165};
    bool cn = country[0] == 'C' && country[1] == 'N';
    unsigned count = 0;
    for (unsigned channel = 1; channel <= (cn ? 13u : 14u) && count < capacity; channel++) {
        uint32_t flags = cn || channel <= 11 ? 0 : LW_CH_NO_IR;
        if (channel == 14) flags |= LW_CH_NO_OFDM;
        out[count++] = (struct lw_channel){ (uint16_t)channel, flags };
    }
    for (unsigned i = 0; band5 && i < sizeof(five) / sizeof(five[0]) && count < capacity; i++) {
        unsigned channel = five[i];
        if (cn && channel >= 100 && channel <= 144) continue;
        uint32_t flags = cn ? 0 : LW_CH_NO_IR;
        if (channel >= 52 && channel <= 144) flags |= LW_CH_RADAR;
        out[count++] = (struct lw_channel){ (uint16_t)channel, flags };
    }
    return count;
}

static inline unsigned
lw_channels(const uint8_t country[2], struct lw_channel *out, unsigned capacity)
{
    return lw_channels_for_bands(country, out, capacity, true);
}

static inline uint16_t lw_channel_frequency(unsigned channel)
{
    return (uint16_t)(channel == 14 ? 2484 : channel <= 14 ? 2407 + channel * 5 : 5000 + channel * 5);
}

static inline size_t
lw_domain_command_for_bands(uint8_t *buffer, size_t room, const uint8_t country[2], bool band5)
{
    struct lw_channel channels[39];
    unsigned count = lw_channels_for_bands(country, channels, 39, band5);
    size_t size = 12 + count * 8;
    if (room < size) return 0;
    for (size_t i = 0; i < size; i++) buffer[i] = 0;
    bool cn = country[0] == 'C' && country[1] == 'N';
    buffer[0] = cn ? 'C' : '0';
    buffer[1] = cn ? 'N' : '0';
    for (unsigned i = 0; i < count; i++) {
        uint8_t *p = buffer + 12 + i * 8;
        unsigned channel = channels[i].number;
        p[0] = (uint8_t)channel;
        p[1] = (uint8_t)(channel >> 8);
        p[4] = (uint8_t)channels[i].flags;
        if (channel <= 14) buffer[8]++;
        else buffer[9]++;
    }
    return size;
}

static inline size_t
lw_domain_command(uint8_t *buffer, size_t room, const uint8_t country[2])
{
    return lw_domain_command_for_bands(buffer, room, country, true);
}

/* Without TAG_CAP_SINGLE_SKU, gen4m sends passive then active legacy
 * CMD_SET_DOMAIN_INFO, each 56 bytes. CN tables are the vendor's group 7
 * and default passive group (span is in units of 5 MHz). The passive table
 * does not make a channel available: the active table supplies that set.
 * The legacy uint16 country has reversed byte order versus the V2 string. */
static inline size_t
lw_legacy_domain_command(uint8_t *buffer, size_t room, const uint8_t country[2],
    bool band5, bool passive)
{
    static const uint8_t active_cn[6][5] = {
        {81,1,1,1,13}, {115,2,4,36,4}, {118,2,4,52,4},
        {121,0,0,0,0}, {125,2,4,149,5}, {0,0,0,0,0},
    };
    static const uint8_t passive_cn[6][5] = {
        {81,1,1,1,0}, {82,1,1,14,0}, {115,2,4,36,0},
        {118,2,4,52,4}, {121,2,4,100,12}, {125,2,4,149,0},
    };
    /* Keep the existing world channel policy, including passive channels
     * 12/13/14 and passive 5 GHz, in the legacy wire representation. */
    static const uint8_t active_world[6][5] = {
        {81,1,1,1,13}, {82,1,1,14,1}, {115,2,4,36,4},
        {118,2,4,52,4}, {121,2,4,100,12}, {125,2,4,149,5},
    };
    static const uint8_t passive_world[6][5] = {
        {81,1,1,12,2}, {82,1,1,14,1}, {115,2,4,36,4},
        {118,2,4,52,4}, {121,2,4,100,12}, {125,2,4,149,5},
    };
    if (!buffer || !country || room < 56) return 0;
    bool cn = country[0] == 'C' && country[1] == 'N';
    const uint8_t (*table)[5] = cn ? (passive ? passive_cn : active_cn) :
        (passive ? passive_world : active_world);
    for (unsigned i = 0; i < 56; i++) buffer[i] = 0;
    buffer[0] = cn ? 'N' : '0';
    buffer[1] = cn ? 'C' : '0';
    buffer[2] = passive ? 1 : 0;
    for (unsigned i = 0; i < 6; i++) {
        if (!band5 && table[i][1] == 2) continue;
        for (unsigned j = 0; j < 5; j++) buffer[4 + i * 8 + j] = table[i][j];
    }
    /* 20 MHz is the current station's PHY width, on both enabled bands. */
    return 56;
}
#endif
