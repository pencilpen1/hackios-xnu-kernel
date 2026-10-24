#!/usr/bin/env python3
"""Build the actual NT36672 parser/state machine with host sanitizers."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "xnu/files/bsd/dev/lancelot_nt36672_protocol.c"
FIXTURE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lancelot_nt36672_protocol.h"
struct sample { uint32_t type, finger, x, y; };
static struct sample out[128]; static unsigned out_count;
static void emit(void *unused, uint32_t type, uint32_t finger, uint32_t x, uint32_t y) {
    (void)unused; assert(out_count < 128); out[out_count++] = (struct sample){type, finger, x, y};
}
static void checksum(uint8_t p[66]) {
    uint8_t sum = 0; unsigned i; p[65] = 0;
    for (i = 1; i < 65; ++i) sum = (uint8_t)(sum + p[i]);
    p[65] = (uint8_t)(0 - sum);
}
static void slot(uint8_t p[66], unsigned n, unsigned id, unsigned event, unsigned x, unsigned y) {
    uint8_t *s = p + 1 + n * 6;
    s[0] = (uint8_t)((id << 3) | event); s[1] = (uint8_t)(x >> 4); s[2] = (uint8_t)(y >> 4);
    s[3] = (uint8_t)((x & 15) << 4 | (y & 15)); s[4] = 4; s[5] = 20;
}
static void reset_out(void) { out_count = 0; memset(out, 0, sizeof(out)); }
int main(void) {
    uint8_t p[66] = {0}, noise[80]; struct lancelot_nt36672_report r; struct lancelot_nt36672_state s;
    assert(lancelot_nt36672_parse(p, 65, &r) == LANCELOT_NT36672_PACKET_BAD_ARGUMENT);
    assert(lancelot_nt36672_parse(NULL, 66, &r) == LANCELOT_NT36672_PACKET_BAD_ARGUMENT);
    assert(lancelot_nt36672_parse(p, 66, NULL) == LANCELOT_NT36672_PACKET_BAD_ARGUMENT);
    assert(!lancelot_nt36672_checksum_ok(NULL, sizeof(p)));
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_OK);
    assert(r.present_mask == 0);
    slot(p, 0, 1, 1, 1080, 2340); checksum(p);
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_OK);
    assert(r.contact[0].present && r.contact[0].x == 1080 && r.contact[0].y == 2340);
    lancelot_nt36672_state_init(&s); reset_out(); assert(lancelot_nt36672_state_consume(&s, &r, emit, NULL));
    assert(out_count == 1 && out[0].type == 1 && out[0].finger == 0);
    slot(p, 0, 1, 2, 100, 200); checksum(p); assert(lancelot_nt36672_parse(p, sizeof(p), &r) == 0);
    reset_out(); lancelot_nt36672_state_consume(&s, &r, emit, NULL);
    assert(out_count == 1 && out[0].type == 2 && out[0].x == 100 && out[0].y == 200);
    memset(p, 0, sizeof(p)); checksum(p); reset_out(); lancelot_nt36672_parse(p, sizeof(p), &r);
    lancelot_nt36672_state_consume(&s, &r, emit, NULL); assert(out_count == 1 && out[0].type == 3);
    slot(p, 0, 1, 1, 20, 30); checksum(p); lancelot_nt36672_parse(p, sizeof(p), &r);
    lancelot_nt36672_state_consume(&s, &r, emit, NULL); assert(s.down[0]);
    p[10] ^= 1; reset_out(); assert(lancelot_nt36672_parse(p, sizeof(p), &r) == 2);
    lancelot_nt36672_state_bad_packet(&s, emit, NULL); lancelot_nt36672_state_bad_packet(&s, emit, NULL);
    assert(s.down[0]); lancelot_nt36672_state_bad_packet(&s, emit, NULL);
    assert(!s.down[0] && out_count == 1 && out[0].type == 3);
    memset(p, 0, sizeof(p)); for (unsigned i = 1; i < 7; ++i) p[i] = 0x77;
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_RECOVERY);
    memset(p, 0, sizeof(p)); for (unsigned i = 1; i < 7; ++i) p[i] = (i & 1) ? 0xfd : 0xfe;
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_WDT_PATTERN);
    slot(p, 0, 1, 1, 40, 50); checksum(p); assert(lancelot_nt36672_parse(p, sizeof(p), &r) == 0);
    lancelot_nt36672_state_consume(&s, &r, emit, NULL); assert(s.down[0]);
    memset(p, 0, sizeof(p)); for (unsigned i = 1; i < 7; ++i) p[i] = (i & 1) ? 0xfd : 0xfe;
    assert(!lancelot_nt36672_state_wdt_recovery(&s, p, 65, emit, NULL));
    for (unsigned i = 0; i < 10; ++i) assert(!lancelot_nt36672_state_wdt_recovery(&s, p, sizeof(p), emit, NULL));
    p[1] = 0; assert(!lancelot_nt36672_state_wdt_recovery(&s, p, sizeof(p), emit, NULL));
    assert(s.wdt_packets == 0 && s.down[0]);
    for (unsigned i = 0; i < 10; ++i) assert(!lancelot_nt36672_state_wdt_recovery(&s, p, sizeof(p), emit, NULL));
    p[1] = 0xfd; for (unsigned i = 2; i < 7; ++i) p[i] = (i & 1) ? 0xfd : 0xfe;
    for (unsigned i = 0; i < 10; ++i) assert(!lancelot_nt36672_state_wdt_recovery(&s, p, sizeof(p), emit, NULL));
    reset_out(); assert(lancelot_nt36672_state_wdt_recovery(&s, p, sizeof(p), emit, NULL));
    assert(s.wdt_packets == 0 && !s.down[0] && out_count == 1 && out[0].type == 3);
    memset(p, 0, sizeof(p)); slot(p, 0, 1, 1, 1081, 1); checksum(p);
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_MALFORMED);
    memset(p, 0, sizeof(p)); slot(p, 0, 1, 1, 1, 1); slot(p, 1, 1, 2, 2, 2); checksum(p);
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_MALFORMED);
    memset(p, 0, sizeof(p)); slot(p, 0, 1, 1, 1, 1); p[63] = 1; checksum(p);
    assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_OK);
    assert(r.contact[0].pressure == 276);
    assert(lancelot_nt36672_state_consume(&s, &r, emit, NULL));
    for (unsigned i = 0; i < 10; ++i) {
        memset(p, 0, sizeof(p)); slot(p, i, i + 1, 1, i, i); checksum(p);
        assert(lancelot_nt36672_parse(p, sizeof(p), &r) == LANCELOT_NT36672_PACKET_OK);
        assert(r.contact[i].present && r.contact[i].id == i + 1);
    }
    memset(noise, 0xa5, sizeof(noise));
    for (unsigned n = 0; n <= sizeof(noise); ++n) {
        for (unsigned i = 0; i < sizeof(noise); ++i) noise[i] = (uint8_t)(i * 37u + n * 13u);
        (void)lancelot_nt36672_parse(noise, n, &r);
    }
    puts("NT36672 protocol: checksum, bounds, IDs, multi-touch, transitions, WDT recovery and release passed");
}
'''

if os.name == "nt" and shutil.which("cc") is None:
    drive_root = ROOT.drive[0].lower()
    wsl_root = f"/mnt/{drive_root}{ROOT.as_posix()[2:]}"
    subprocess.run(["wsl", "-d", "Ubuntu", "--", "bash", "-lc",
                    f"cd '{wsl_root}' && python3 tools/test-lancelot-nt36672-protocol.py --inside-wsl"], check=True)
    sys.exit(0)

with tempfile.TemporaryDirectory(prefix="lancelot-nt36672-") as directory:
    d = Path(directory)
    (d / "fixture.c").write_text(FIXTURE)
    binary = d / "fixture"
    subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(SRC.parent),
                    str(SRC), str(d / "fixture.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
