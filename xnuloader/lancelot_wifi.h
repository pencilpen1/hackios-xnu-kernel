/* Native lancelot WLAN handoff. Mu reserves both physical pools before any
 * allocator sees RAM; the loader verifies the map and passes the board data.
 */
static struct lw_handoff loader_wifi_handoff;

static int
lancelot_wifi_reserved(uint64_t base, uint64_t length)
{
    static uint8_t map[65536];
    uint64_t bytes = sizeof(map), key, stride;
    uint32_t version;
    if (!length || base > UINT64_MAX - length ||
        EFI_ERROR(BS->get_memory_map(&bytes, map, &key, &stride, &version)) ||
        stride < sizeof(efi_memory_descriptor) || bytes % stride) return 0;
    uint64_t at = base, end = base + length;
    while (at < end) {
        uint64_t next = at;
        for (uint64_t off = 0; off < bytes; off += stride) {
            const efi_memory_descriptor *d = (const void *)(map + off);
            if (d->pages > (UINT64_MAX - d->phys_start) / 4096) return 0;
            uint64_t hi = d->phys_start + d->pages * 4096;
            if (d->phys_start <= at && hi > at) {
                if (d->type != 0) return 0; /* EfiReservedMemoryType */
                next = hi;
                break;
            }
        }
        if (next == at) return 0;
        at = next;
    }
    return 1;
}

static int
lancelot_wifi_loader_prepare(const struct payload_info *pi)
{
    memset(&loader_wifi_handoff, 0, sizeof(loader_wifi_handoff));
    if (PLAT != &platform_lancelot) return 1;
    int step = lw_boot_step(pi->cmdline);
    if (step < 0) {
        print("lancelot-wifi: invalid or duplicate Wi-Fi boot option\n");
        return 0;
    }
    if (step < 3) return 1;
    struct lw_board board;
    if (!pi->wifi_board || pi->wifi_board_size != sizeof(board)) {
        print("lancelot-wifi: native Wi-Fi requires the private wifi-board payload\n");
        return 0;
    }
    memcpy(&board, pi->wifi_board, sizeof(board));
    if (!lw_board_valid(&board) ||
        !lancelot_wifi_reserved(LW_EMI_PHYS, LW_EMI_SIZE) ||
        !lancelot_wifi_reserved(LW_DMA_PHYS, LW_DMA_SIZE)) {
        print("lancelot-wifi: board data or Mu reserved pools do not match lancelot\n");
        return 0;
    }
    loader_wifi_handoff = (struct lw_handoff){
        .magic = LW_HANDOFF_MAGIC, .version = LW_ABI_VERSION,
        .emi_phys = LW_EMI_PHYS, .emi_size = LW_EMI_SIZE,
        .dma_phys = LW_DMA_PHYS, .dma_size = LW_DMA_SIZE,
        .board = board, .flags = LW_RESERVED,
    };
    loader_wifi_handoff.crc = lw_crc(&loader_wifi_handoff, offsetof(struct lw_handoff, crc));
    print("lancelot-wifi: Mu reserved EMI %x/%x and DMA %x/%x; board data verified\n",
        (uint64_t)LW_EMI_PHYS, (uint64_t)LW_EMI_SIZE, (uint64_t)LW_DMA_PHYS, (uint64_t)LW_DMA_SIZE);
    return 1;
}
