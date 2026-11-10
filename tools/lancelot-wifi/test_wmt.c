#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_wifi_wire.h"

int main(void)
{
    const uint8_t query[] = {1, 2, 1, 0, 0x12};
    const uint8_t expected[] = {2, 2, 5, 0, 0};
    /* Exact W2 lancelot response, with STP already validated/removed. */
    const uint8_t observed[] = {2, 2, 5, 0, 0, 1, 0x8b, 0x31, 0x66};
    uint16_t revision = 0;
    assert(lw_wmt_adie(observed, sizeof(observed), &revision) && revision == 0x8b01);
    assert(lw_wmt_reply_matches(query, sizeof(query), observed, sizeof(observed), expected, sizeof(expected)));
    for (size_t n = 0; n < sizeof(observed); n++) {
        assert(!lw_wmt_adie(observed, n, &revision));
        assert(!lw_wmt_reply_matches(query, sizeof(query), observed, n, expected, sizeof(expected)));
    }
    for (size_t n = 0; n < sizeof(query); n++)
        assert(!lw_wmt_reply_matches(query, n, observed, sizeof(observed), expected, sizeof(expected)));
    uint8_t reply[10];
    memcpy(reply, observed, sizeof(observed));
    reply[9] = 0;
    assert(!lw_wmt_adie(reply, sizeof(reply), &revision));
    for (unsigned i = 0; i < sizeof(observed); i++) {
        memcpy(reply, observed, sizeof(observed));
        reply[i] ^= 1;
        bool revision_field = i == 5 || i == 6;
        assert(lw_wmt_reply_matches(query, sizeof(query), reply, sizeof(observed), expected, sizeof(expected)) == revision_field);
    }
    /* A foreign chip or the old misplaced ID constant is not MT6631's reply. */
    const uint8_t misplaced[] = {2, 2, 5, 0, 0, 0x31, 0x66, 0, 0};
    assert(!lw_wmt_adie(misplaced, sizeof(misplaced), &revision));
    assert(!lw_wmt_reply_matches(query, sizeof(query), NULL, 9, expected, sizeof(expected)));
    assert(!lw_wmt_reply_matches(query, sizeof(query), observed, sizeof(observed), NULL, 5));

    const uint8_t tssi[] = {1, 2, 4, 0, 0x10, 3, 0x87, 0x0e};
    const uint8_t done[] = {2, 2, 1, 0, 0};
    assert(lw_wmt_reply_matches(tssi, sizeof(tssi), done, sizeof(done), done, sizeof(done)));
    assert(!lw_wmt_reply_matches(query, sizeof(query), done, sizeof(done), done, sizeof(done)));
    const uint8_t extra_done[] = {2, 2, 2, 0, 0, 0};
    assert(!lw_wmt_reply_matches(tssi, sizeof(tssi), extra_done, sizeof(extra_done), done, sizeof(done)));
    /* Register values are deliberately dynamic; the expected prefix includes
     * the full declared length, address and status, not the final value. */
    const uint8_t read_id[] = {1, 8, 16, 0, 2, 1, 0, 1, 8, 0, 0, 0x80, 0, 0, 0, 0, 255, 255, 255, 255};
    const uint8_t id_reply[] = {2, 8, 12, 0, 0, 0, 0, 1, 8, 0, 0, 0x80, 0x32, 0x66, 0, 0};
    assert(lw_wmt_reply_matches(read_id, sizeof(read_id), id_reply, sizeof(id_reply), id_reply, 12));
    const uint8_t utc[] = {1, 0xf0, 9, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t utc_done[] = {2, 0xf0, 2, 0, 2, 0};
    assert(lw_wmt_reply_matches(utc, sizeof(utc), utc_done, sizeof(utc_done), utc_done, sizeof(utc_done)));
    const uint8_t calibration[] = {1, 0x14, 1, 0, 1};
    const uint8_t calibrated[] = {2, 0x14};
    uint8_t calibration_reply[454] = {2, 0x14, 0xc2, 1, 0, 1};
    assert(lw_wmt_reply_matches(calibration, sizeof(calibration), calibration_reply, sizeof(calibration_reply), calibrated, sizeof(calibrated)));
    calibration_reply[4] = 1;
    assert(!lw_wmt_reply_matches(calibration, sizeof(calibration), calibration_reply, sizeof(calibration_reply), calibrated, sizeof(calibrated)));
    calibration_reply[4] = 0;
    calibration_reply[5] = 2;
    assert(!lw_wmt_reply_matches(calibration, sizeof(calibration), calibration_reply, sizeof(calibration_reply), calibrated, sizeof(calibrated)));
    calibration_reply[5] = 1;
    const uint8_t other_calibration[] = {1, 0x14, 1, 0, 2};
    assert(!lw_wmt_reply_matches(other_calibration, sizeof(other_calibration), calibration_reply, sizeof(calibration_reply), calibrated, sizeof(calibrated)));
    calibration_reply[2]--;
    assert(!lw_wmt_reply_matches(calibration, sizeof(calibration), calibration_reply, sizeof(calibration_reply), calibrated, sizeof(calibrated)));
    puts("PASS: actual W2 MT6631 A-die reply; dynamic revision, foreign chip/status/length rejection, TSSI/UTC/calibration response contracts");
    return 0;
}
