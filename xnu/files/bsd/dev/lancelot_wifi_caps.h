/* gen4m EVENT_NIC_CAPABILITY_V2: optional tagged capabilities, not a fixed
 * structure. Layouts follow nic_cmd_event.h and rlm_domain.h. As in
 * nicCmdEventQueryNicCapabilityV2, an omitted tag leaves the board defaults
 * intact. Bounds and an explicitly reported foreign A-die are still errors. */
#ifndef LANCELOT_WIFI_CAPS_H
#define LANCELOT_WIFI_CAPS_H
#include "lancelot_wifi_wire.h"

#define LW_CAP_HW    1u
#define LW_CAP_SW    2u
#define LW_CAP_PHY   4u
#define LW_CAP_SKU   8u
#define LW_CAP_ADIE 16u

enum lw_caps_error { LW_CAPS_OK, LW_CAPS_HEADER, LW_CAPS_TLV_HEADER,
    LW_CAPS_TLV_BODY, LW_CAPS_SHORT_FIELD, LW_CAPS_FOREIGN_ADIE };

struct lw_nic_caps {
    uint32_t seen, tag, body_length, offset;
    uint32_t short_seen, short_tag, short_length;
    uint16_t elements, parsed, product, eco, adie, adie_eco, version, build;
    uint8_t ht, vht, band5, bandwidth, efuse_valid, country[2];
    char branch[4], date[16];
    enum lw_caps_error error;
};

static inline uint16_t lw_caps_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline bool
lw_nic_caps_parse(const uint8_t *data, size_t length, struct lw_nic_caps *out)
{
    if (!out) return false;
    *out = (struct lw_nic_caps){0};
    if (!data || length < 4) { out->error = LW_CAPS_HEADER; return false; }
    out->elements = lw_caps_u16(data);
    size_t at = 4;
    for (unsigned i = 0; i < out->elements; i++) {
        out->offset = (uint32_t)at;
        if (at > length || length - at < 8) { out->error = LW_CAPS_TLV_HEADER; return false; }
        uint32_t tag = lw_get_le32(data + at), size = lw_get_le32(data + at + 4);
        out->tag = tag;
        out->body_length = size;
        if (size > length - at - 8) { out->error = LW_CAPS_TLV_BODY; return false; }
        const uint8_t *b = data + at + 8;
        unsigned need = tag == 3 ? 5 : tag == 5 || tag == 8 || tag == 0x14 ? 4 : tag == 6 ? 24 : 0;
        if (size < need) {
            if (tag == 0x14) { out->error = LW_CAPS_SHORT_FIELD; return false; }
            /* Older firmware can omit fields in optional information tags.
             * Do not turn an ignored capability into a startup requirement. */
            out->short_seen |= tag == 3 ? LW_CAP_SKU : tag == 5 ? LW_CAP_HW :
                tag == 6 ? LW_CAP_SW : LW_CAP_PHY;
            out->short_tag = tag;
            out->short_length = size;
            at += 8 + size;
            out->parsed++;
            continue;
        }
        if (tag == 3) {
            out->seen |= LW_CAP_SKU;
            out->efuse_valid = b[4];
            out->country[0] = b[0];
            out->country[1] = b[1];
        } else if (tag == 5) {
            out->seen |= LW_CAP_HW;
            out->product = lw_caps_u16(b);
            out->eco = b[2];
        } else if (tag == 6) {
            out->seen |= LW_CAP_SW;
            out->version = lw_caps_u16(b);
            out->build = lw_caps_u16(b + 2);
            for (unsigned j = 0; j < 4; j++) out->branch[j] = (char)b[4 + j];
            for (unsigned j = 0; j < 16; j++) out->date[j] = (char)b[8 + j];
        } else if (tag == 8) {
            out->seen |= LW_CAP_PHY;
            out->ht = b[0]; out->vht = b[1]; out->band5 = b[2]; out->bandwidth = b[3];
        } else if (tag == 0x14) {
            out->seen |= LW_CAP_ADIE;
            /* Unlike WMT_QUERY_A_DIE, this structure is product first. */
            out->adie = lw_caps_u16(b);
            out->adie_eco = lw_caps_u16(b + 2);
            if (out->adie != 0x6631) { out->error = LW_CAPS_FOREIGN_ADIE; return false; }
        }
        at += 8 + size;
        out->parsed++;
    }
    out->offset = (uint32_t)at;
    return true;
}
#endif
