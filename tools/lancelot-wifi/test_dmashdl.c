#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WPDMA_GLO_CFG 0x4208u
static uint32_t bank[128], glo, fail_address;
static uint32_t addresses[32], values[32];
static unsigned writes, barriers, logs, stop_after;
static bool stopping;
static bool lancelot_wifi_stopping(void) { return stopping || (stop_after && writes >= stop_after); }
static uint32_t wf_rd(uint32_t address)
{
    if (address == WPDMA_GLO_CFG) return glo;
    assert(address >= 0x6000 && address < 0x6200 && !(address & 3));
    return bank[(address - 0x6000) / 4];
}
static void wf_wr(uint32_t address, uint32_t value)
{
    assert(address >= 0x6000 && address < 0x6200 && !(address & 3) && writes < 32);
    addresses[writes] = address;
    values[writes++] = value;
    if (address != fail_address) bank[(address - 0x6000) / 4] = value;
}
static void lancelot_wifi_log(const char *format, ...) { (void)format; logs++; }
#define LW_DMASHDL_BARRIER() (barriers++)
#include "lancelot_wifi_dmashdl.inc"

static void reset(void)
{
    for (unsigned i = 0; i < 128; i++) bank[i] = 0xa5a55a5au;
    writes = barriers = logs = stop_after = 0;
    glo = fail_address = 0;
    stopping = false;
}

int main(void)
{
    reset();
    uint32_t before = wf_rd(0x601c);
    assert(wf_dmashdl_init() && writes == 18 && barriers == 1 && logs == 1);
    assert((wf_rd(0x601c) & 0xf000f000u) == (before & 0xf000f000u));
    assert((wf_rd(0x601c) & 0x0fff0fffu) == 0x00180001u);
    assert(((wf_rd(0x601c) >> 16) & 0xfff) * 128 >= 64 + 2048);
    assert(wf_rd(0x6010) == 0xfffe0000u && wf_rd(0x6020) == 0x0fff0003u);
    assert(addresses[0] == 0x601c && addresses[1] == 0x6010 && addresses[2] == 0x6020);
    assert(addresses[3] == 0x6024 && addresses[4] == 0x6028 && addresses[5] == 0x60d0);
    for (unsigned group = 1; group < 15; group++) assert(!wf_rd(0x6020 + group * 4));
    assert(wf_rd(0x605c) == 0xa5a55a5au && wf_rd(0x60d4) == 0xa5a55a5au);
    for (unsigned bit = 0; bit < 4; bit++) {
        reset(); glo = 1u << bit;
        assert(!wf_dmashdl_init() && !writes && !logs);
    }
    reset(); stopping = true;
    assert(!wf_dmashdl_init() && !writes);
    reset(); stop_after = 7;
    assert(!wf_dmashdl_init() && writes == 7 && !barriers);
    const uint32_t bad[] = {0x601c, 0x6010, 0x6020, 0x6024, 0x6058, 0x60d0};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset(); fail_address = bad[i];
        assert(!wf_dmashdl_init() && logs == 1);
    }
    puts("PASS: production CONNAC DMASHDL sequence, 3072-byte PSE capacity, quota/refill readback, busy/cancel and failed-write rejection");
    return 0;
}
