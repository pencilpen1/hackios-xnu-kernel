#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lancelot_wifi_wire.h"
#include "lancelot_wifi_channels.h"

static uint8_t *load(const char *name, size_t *size)
{
    FILE *f = fopen(name, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n > 0 && n < 8 * 1024 * 1024 && !fseek(f, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)n);
    assert(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    const uint16_t command_rates[] = {0x00, 0x4b};
    for (unsigned r = 0; r < 2; r++) {
        uint8_t txd[32], before[32];
        memset(txd, 0xa5, sizeof(txd));
        memcpy(before, txd, sizeof(before));
        assert(lw_command_fixed_rate(txd, sizeof(txd), command_rates[r]));
        assert(lw_get_le32(txd + 8) & (1u << 31));
        assert(!(lw_get_le32(txd + 24) & (1u << 31)));
        assert((txd[26] | (uint16_t)txd[27] << 8) == command_rates[r]);
        for (unsigned i = 0; i < 32; i++)
            if (i != 11 && i != 26 && i != 27) assert(txd[i] == before[i]);
        assert((txd[11] & 0x7f) == (before[11] & 0x7f));
        assert(!lw_command_fixed_rate(txd, 31, command_rates[r]));
    }
    assert(!lw_command_fixed_rate(NULL, 32, 0x4b));
    assert(lw_crc("123456789", 9) == 0xcbf43926);
    assert(lw_stp_crc((const uint8_t *)"123456789", 9) == 0xbb3d);
    struct lw_board board = { .magic = LW_BOARD_MAGIC, .version = LW_ABI_VERSION,
        .mac = {2, 1, 2, 3, 4, 5}, .country = {'C', 'N'}, .bt_tssi = 3719,
        .bt_tssi_from_wifi = 3, .ant_mode = 1, .co_clock = 1, .nvram_size = LW_NVRAM_SIZE };
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) board.firmware_sha256[i][0] = (uint8_t)(i + 1);
    board.crc = lw_crc(&board, offsetof(struct lw_board, crc));
    assert(lw_board_valid(&board));
    struct lw_handoff h = { .magic = LW_HANDOFF_MAGIC, .version = LW_ABI_VERSION,
        .emi_phys = LW_EMI_PHYS, .emi_size = LW_EMI_SIZE,
        .dma_phys = LW_DMA_PHYS, .dma_size = LW_DMA_SIZE, .board = board, .flags = LW_RESERVED };
    h.crc = lw_crc(&h, offsetof(struct lw_handoff, crc));
    assert(lw_handoff_valid(&h, 0xc0000000, 0x7c000000));
    assert(!lw_handoff_valid(&h, LW_DMA_PHYS, 0x10000));
    assert(!lw_handoff_valid(&h, LW_EMI_PHYS - 4096, 8192));
    assert(!lw_handoff_valid(&h, UINT64_MAX - 4095, 8192));
    struct lw_handoff bad = h;
    bad.emi_phys = 0x51000000;
    bad.crc = lw_crc(&bad, offsetof(struct lw_handoff, crc));
    assert(!lw_handoff_valid(&bad, 0xc0000000, 0x7c000000));
    board.mac[0] |= 1;
    board.crc = lw_crc(&board, offsetof(struct lw_board, crc));
    assert(!lw_board_valid(&board));
    assert(lw_boot_step("") == 0);
    assert(lw_boot_step("wifi=0") == 0);
    assert(lw_boot_step("wifi=1") == 17);
    assert(lw_boot_step("connsys_step=1") == 1);
    assert(lw_boot_step("wifi=1 connsys_step=12") == 12);
    assert(lw_boot_step("other_wifi=1") == 0);
    assert(lw_boot_step("wifi=10") < 0);
    assert(lw_boot_step("wifi=1 wifi=0") < 0);
    assert(lw_boot_step("connsys_step=18") < 0);
    assert(lw_boot_step("connsys_step=") < 0);
    assert(lw_boot_step("wifi=1x") < 0);

    uint8_t query[] = {1, 4, 1, 0, 4}, frame[70];
    assert(lw_stp_encode(frame, sizeof(frame), query, sizeof(query), true, 0, 7) == 11);
    assert(frame[0] == 0x87 && frame[1] == 0x40 && frame[2] == 5 && frame[3] == 0xcc);
    assert(lw_stp_header_valid(frame, true));
    frame[3] ^= 1;
    assert(!lw_stp_header_valid(frame, true));
    assert(!lw_stp_encode(frame, 10, query, sizeof(query), true, 0, 0));
    assert(!lw_stp_encode(frame, sizeof(frame), query, sizeof(query), true, 8, 0));
    assert(lw_stp_encode(frame, sizeof(frame), query, sizeof(query), false, 7, 7) == 11);
    assert(frame[0] == 0x80 && frame[3] == 0 && frame[9] == 0 && frame[10] == 0);

    struct lw_channel channels[39];
    uint8_t domain[324];
    const uint8_t cn[] = {'C', 'N'}, world[] = {'0', '0'};
    assert(lw_channels(cn, channels, 39) == 26);
    assert(channels[0].number == 1 && channels[25].number == 165);
    for (unsigned i = 0; i < 26; i++) assert(channels[i].number < 100 || channels[i].number > 144);
    assert(lw_domain_command(domain, sizeof(domain), cn) == 220);
    assert(domain[0] == 'C' && domain[1] == 'N' && domain[8] == 13 && domain[9] == 13);
    assert(lw_domain_command(domain, sizeof(domain), world) == 324);
    assert(domain[8] == 14 && domain[9] == 25);
    assert(lw_channels(world, channels, 39) == 39);
    assert(channels[13].flags == (LW_CH_NO_IR | LW_CH_NO_OFDM));
    assert(channels[33].number == 144 && (channels[33].flags & LW_CH_RADAR));
    assert(!lw_domain_command(domain, 323, world));

    uint32_t length;
    assert(lw_rx_size(0x80000000u | (3840u << 16), 4096, &length) && length == 3840);
    assert(!lw_rx_size(3840u << 16, 4096, &length));
    assert(!lw_rx_size(0x80000000u | (4096u << 16), 4096, &length));
    assert(!lw_rx_size(0x80000000u | (30u << 16), 24, &length));
    assert(!lw_rx_size(0x80000000u | (15u << 16), 4096, &length));

    size_t size;
    uint8_t *data = load(argv[1], &size);
    struct lw_fw_plan plan;
    assert(lw_fw_parse(data, size, &plan) && plan.count == 5);
    assert(plan.regions[0].address == 0x57000 && plan.regions[0].length == 0xf90);
    assert(plan.regions[3].address == 0xf0177000 && plan.regions[3].feature == 0x80);
    assert(plan.regions[4].address == 0xf01fe000 && plan.regions[4].length == 0x22e00);
    for (size_t n = 0; n < 76; n++) assert(!lw_fw_parse(data, n, &plan));
    uint8_t *copy = malloc(size);
    assert(copy);
    memcpy(copy, data, size);
    copy[size - 34] = 255;
    assert(!lw_fw_parse(copy, size, &plan));
    memcpy(copy, data, size);
    size_t tailer = size - 36 - 5 * 40;
    memset(copy + tailer + 20, 255, 4);
    copy[tailer + 24] = 0x40; /* NOT_DOWNLOAD must not bypass the length check. */
    assert(!lw_fw_parse(copy, size, &plan));
    memcpy(copy, data, size);
    copy[tailer + 3 * 40 + 19] = 0xf1; /* EMI cannot escape its reserved remap. */
    assert(!lw_fw_parse(copy, size, &plan));
    free(copy);
    free(data);
    uint32_t slots[] = {0, 0x80000, 0x140000};
    for (unsigned i = 0; i < 3; i++) {
        data = load(argv[2 + i], &size);
        uint32_t offset;
        assert(lw_rom_image(data, size, slots[i], &offset) && offset == slots[i]);
        assert(!lw_rom_image(data, size, slots[i] + 0x4000, &offset));
        data[16] ^= 1;
        assert(!lw_rom_image(data, size, slots[i], &offset));
        free(data);
    }
    puts("PASS: real lancelot firmware, DMA bounds, STP framing/CRC, loader reservations and negative inputs");
    return 0;
}
