#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_wifi_channels.h"

/* Independent vendor wire layout. Channel span is in 5 MHz units. */
struct subband { uint8_t reg, band, span, first, count, reserved[3]; };
struct legacy_domain {
    uint16_t country, passive;
    struct subband sub[6];
    uint8_t bw2, bw5, reserved[2];
};
_Static_assert(sizeof(struct legacy_domain) == 56, "legacy gen4m domain size");
_Static_assert(offsetof(struct legacy_domain, sub) == 4, "legacy subband offset");
_Static_assert(offsetof(struct legacy_domain, bw2) == 52, "legacy bandwidth offset");

static bool has_channel(const struct legacy_domain *d, unsigned band, unsigned channel)
{
    for (unsigned i = 0; i < 6; i++) {
        const struct subband *s = &d->sub[i];
        if (s->band != band || !s->span) continue;
        for (unsigned j = 0; j < s->count; j++) if (s->first + j * s->span == channel) return true;
    }
    return false;
}

int main(void)
{
    const uint8_t cn[2] = {'C', 'N'}, world[2] = {'0', '0'};
    uint8_t buffer[324];
    struct legacy_domain active, passive;
    assert(lw_legacy_domain_command(buffer, sizeof(buffer), cn, true, false) == sizeof(active));
    memcpy(&active, buffer, sizeof(active));
    assert(buffer[0] == 'N' && buffer[1] == 'C' && !buffer[2] && !buffer[3]);
    assert(active.sub[0].reg == 81 && active.sub[0].span == 1 && active.sub[0].count == 13);
    assert(active.sub[1].reg == 115 && active.sub[1].span == 4 && active.sub[1].first == 36 && active.sub[1].count == 4);
    assert(active.sub[2].reg == 118 && active.sub[2].span == 4 && active.sub[2].first == 52 && active.sub[2].count == 4);
    assert(active.sub[3].reg == 121 && !active.sub[3].band && !active.sub[3].count);
    assert(active.sub[4].reg == 125 && active.sub[4].first == 149 && active.sub[4].count == 5);
    assert(!active.sub[5].band && !active.bw2 && !active.bw5);
    assert(lw_legacy_domain_command(buffer, sizeof(buffer), cn, true, true) == sizeof(passive));
    memcpy(&passive, buffer, sizeof(passive));
    assert(buffer[0] == 'N' && buffer[1] == 'C' && buffer[2] == 1 && buffer[3] == 0);
    assert(passive.sub[3].reg == 118 && passive.sub[3].first == 52 && passive.sub[3].count == 4);
    assert(passive.sub[4].reg == 121 && passive.sub[4].first == 100 && passive.sub[4].count == 12);
    for (unsigned channel = 1; channel <= 13; channel++) assert(has_channel(&active, 1, channel));
    for (unsigned channel = 36; channel <= 64; channel += 4) {
        assert(has_channel(&active, 2, channel));
        assert(has_channel(&passive, 2, channel) == (channel >= 52));
    }
    for (unsigned channel = 149; channel <= 165; channel += 4) assert(has_channel(&active, 2, channel));
    for (unsigned channel = 100; channel <= 144; channel += 4) assert(!has_channel(&active, 2, channel));
    for (unsigned i = 0; i < 6; i++) for (unsigned j = 0; j < 3; j++)
        assert(!active.sub[i].reserved[j] && !passive.sub[i].reserved[j]);
    struct lw_channel channels[39];
    for (unsigned c = 0; c < 2; c++) for (unsigned band5 = 0; band5 < 2; band5++) {
        const uint8_t *country = c ? world : cn;
        unsigned count = lw_channels_for_bands(country, channels, 39, band5);
        assert(lw_legacy_domain_command(buffer, sizeof(buffer), country, band5, false) == 56);
        memcpy(&active, buffer, sizeof(active));
        for (unsigned i = 0; i < count; i++) assert(has_channel(&active,
            channels[i].number <= 14 ? 1 : 2, channels[i].number));
        if (!band5) for (unsigned i = 0; i < 6; i++) assert(active.sub[i].band != 2);
    }
    /* V2 retains its distinct ASCII country and variable list; do not reuse
     * its representation when Single SKU is absent. */
    assert(lw_domain_command_for_bands(buffer, sizeof(buffer), cn, true) == 12 + 26 * 8);
    assert(buffer[0] == 'C' && buffer[1] == 'N' && buffer[8] == 13 && buffer[9] == 13);
    memset(buffer, 0xa5, sizeof(buffer));
    assert(!lw_legacy_domain_command(buffer, 55, cn, true, false) && buffer[0] == 0xa5);
    assert(!lw_legacy_domain_command(NULL, 56, cn, true, false));
    assert(!lw_legacy_domain_command(buffer, 56, NULL, true, false));
    puts("PASS: 56-byte legacy passive/active domain ABI, CN endian/subbands, both bands retained, world policy and distinct V2 format");
    return 0;
}
