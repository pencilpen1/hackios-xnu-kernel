/* MT6768 CONNAC AXI, single data TX ring. The vendor's halHifSwInfoInit
 * invokes asicPcieDmaShdlInit before setting up WPDMA rings. Reproduce that
 * branch here: one PLE descriptor page, 24 PSE payload pages, group 0 with
 * refill and min/max quota 3/4095. A 2112-byte NVRAM packet needs 17 pages.
 * Offsets/masks: gen4m/include/chips/dma_sch.h and nic/connac_dmashdl.h. */
#ifndef LW_DMASHDL_BARRIER
#define LW_DMASHDL_BARRIER() __asm__ volatile("dsb sy" ::: "memory")
#endif

static bool
wf_dmashdl_init(void)
{
    if (lancelot_wifi_stopping() || (wf_rd(WPDMA_GLO_CFG) & 0xfu)) return false;
    uint32_t before = wf_rd(0x601c), refill_before = wf_rd(0x6010), group_before = wf_rd(0x6020);
    uint32_t wanted = (before & ~0x0fff0fffu) | 0x00180001u;
    wf_wr(0x601c, wanted);
    wf_wr(0x6010, 0xfffe0000u);
    wf_wr(0x6020, 0x0fff0003u);
    wf_wr(0x6024, 0);
    wf_wr(0x6028, 0);
    wf_wr(0x60d0, 0);
    for (unsigned group = 3; group < 15; group++) {
        if (lancelot_wifi_stopping()) return false;
        wf_wr(0x6020 + group * 4, 0);
    }
    /* Vendor leaves group 15 and the remaining queue maps at their reset
     * values. They are not new rings or memory allocations owned by us. */
    LW_DMASHDL_BARRIER();
    uint32_t packet = wf_rd(0x601c), refill = wf_rd(0x6010), group0 = wf_rd(0x6020);
    bool ok = (packet & 0x0fff0fffu) == 0x00180001u && refill == 0xfffe0000u &&
        group0 == 0x0fff0003u && wf_rd(0x60d0) == 0;
    for (unsigned group = 1; group < 15; group++) ok &= wf_rd(0x6020 + group * 4) == 0;
    lancelot_wifi_log("lancelot-wifi: DMASHDL packet=%#x->%#x PLE/PSE=1/24 pages refill=%#x->%#x group0=%#x->%#x qmap=%#x/%#x free=%#x verified=%u",
        before, packet, refill_before, refill, group_before, group0, wf_rd(0x60d0), wf_rd(0x60d4),
        wf_rd(0x6100), ok);
    return ok && !lancelot_wifi_stopping();
}
