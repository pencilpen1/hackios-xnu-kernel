#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_wifi_caps.h"
#include "lancelot_wifi_channels.h"

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static void append(uint8_t *buf, size_t *length, unsigned tag, const uint8_t *body, unsigned size)
{
    assert(*length + 8 + size <= 512);
    put32(buf + *length, tag);
    put32(buf + *length + 4, size);
    if (size) memcpy(buf + *length + 8, body, size);
    *length += 8 + size;
    buf[0]++;
}

int main(void)
{
    /* Independent wire fixtures use vendor CAP_* layouts, not WMT's A-die
     * reply order. These are layout tests, not an alleged device capture. */
    const uint8_t hw[20] = {0x68, 0x76, 0, 0};
    const uint8_t sw[24] = {2, 0, 42, 0, 't', 'k', '_', '_', '2', '0', '2', '1', '1', '0', '1', '3'};
    const uint8_t phy[12] = {1, 1, 1, 2, 1};
    const uint8_t sku[12] = {'C', 'N', 0, 0, 1};
    const uint8_t adie[20] = {0x31, 0x66, 1, 0x8b};
    const uint8_t *const bodies[] = {hw, sw, phy, sku, adie};
    const unsigned sizes[] = {sizeof(hw), sizeof(sw), sizeof(phy), sizeof(sku), sizeof(adie)};
    const unsigned tags[] = {5, 6, 8, 3, 0x14};
    const unsigned bits[] = {LW_CAP_HW, LW_CAP_SW, LW_CAP_PHY, LW_CAP_SKU, LW_CAP_ADIE};
    uint8_t buf[512], full[512];
    struct lw_nic_caps caps;
    unsigned cases = 0;
    size_t full_length = 0;
    /* Any subset is legal: optional presence is not initialization failure. */
    for (unsigned subset = 0; subset < 32; subset++) {
        memset(buf, 0, sizeof(buf));
        size_t length = 4;
        unsigned seen = 0;
        for (unsigned i = 0; i < 5; i++) if (subset & (1u << i)) {
            append(buf, &length, tags[i], bodies[i], sizes[i]);
            seen |= bits[i];
        }
        assert(lw_nic_caps_parse(buf, length, &caps));
        assert(caps.seen == seen && caps.parsed == buf[0] && caps.offset == length && caps.error == LW_CAPS_OK);
        if (seen & LW_CAP_HW) assert(caps.product == 0x7668 && caps.eco == 0);
        if (seen & LW_CAP_SW) assert(caps.version == 2 && caps.build == 42 && !memcmp(caps.branch, "tk__", 4));
        if (seen & LW_CAP_PHY) assert(caps.ht == 1 && caps.vht == 1 && caps.band5 == 1);
        if (seen & LW_CAP_SKU) assert(caps.efuse_valid && !memcmp(caps.country, "CN", 2));
        if (seen & LW_CAP_ADIE) assert(caps.adie == 0x6631 && caps.adie_eco == 0x8b01);
        if (subset == 31) { full_length = length; memcpy(full, buf, length); }
        cases++;
    }
    for (size_t length = 0; length < full_length; length++) {
        assert(!lw_nic_caps_parse(full, length, &caps));
        cases++;
    }
    memset(buf, 0, sizeof(buf));
    size_t length = 4;
    append(buf, &length, 0xfedcba98, NULL, 0);
    append(buf, &length, 0x14, adie, sizeof(adie));
    assert(lw_nic_caps_parse(buf, length, &caps) && caps.parsed == 2 && caps.seen == LW_CAP_ADIE);
    cases++;
    buf[length - sizeof(adie)] = 0x32;
    assert(!lw_nic_caps_parse(buf, length, &caps) && caps.error == LW_CAPS_FOREIGN_ADIE && caps.adie == 0x6632);
    cases++;
    for (unsigned i = 0; i < 5; i++) {
        memset(buf, 0, sizeof(buf));
        length = 4;
        unsigned minimum = tags[i] == 3 ? 5 : tags[i] == 6 ? 24 : 4;
        append(buf, &length, tags[i], bodies[i], minimum - 1);
        bool ok = lw_nic_caps_parse(buf, length, &caps);
        if (tags[i] == 0x14) assert(!ok && caps.error == LW_CAPS_SHORT_FIELD);
        else assert(ok && caps.error == LW_CAPS_OK && !caps.seen && caps.short_seen == bits[i]);
        assert(caps.tag == tags[i] && caps.body_length == minimum - 1);
        cases++;
    }
    memcpy(buf, full, full_length);
    put32(buf + 8, UINT32_MAX);
    assert(!lw_nic_caps_parse(buf, full_length, &caps) && caps.error == LW_CAPS_TLV_BODY);
    cases++;
    memcpy(buf, full, full_length);
    buf[0]++;
    assert(!lw_nic_caps_parse(buf, full_length, &caps) && caps.error == LW_CAPS_TLV_HEADER);
    cases++;
    memset(buf, 0, sizeof(buf));
    length = 4;
    uint8_t two_ghz[12] = {1, 0, 0, 1, 1};
    append(buf, &length, 8, two_ghz, sizeof(two_ghz));
    assert(lw_nic_caps_parse(buf, length, &caps) && (caps.seen & LW_CAP_PHY) && !caps.band5);
    struct lw_channel channels[39];
    const uint8_t cn[2] = {'C', 'N'};
    unsigned count = lw_channels_for_bands(cn, channels, 39, caps.band5);
    assert(count == 13);
    for (unsigned i = 0; i < count; i++) assert(channels[i].number == i + 1);
    length = lw_domain_command_for_bands(buf, sizeof(buf), cn, caps.band5);
    assert(length == 12 + 13 * 8 && buf[8] == 13 && buf[9] == 0);
    assert(lw_channels_for_bands(cn, channels, 39, true) > count);
    cases++;
    assert(!lw_nic_caps_parse(NULL, 4, &caps));
    assert(!lw_nic_caps_parse(full, full_length, NULL));
    printf("PASS: %u NIC capability layout scenarios; all optional tag subsets, exact ADIE field order, bounded TLVs and 2.4GHz-only channel/domain policy\n", cases);
    return 0;
}
