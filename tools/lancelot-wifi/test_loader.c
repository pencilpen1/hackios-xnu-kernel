#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efi.h"
#include "lancelot_wifi_contract.h"

static efi_memory_descriptor descriptors[4];
static efi_status map_result;
static efi_status get_map(uint64_t *size, void *map, uint64_t *key, uint64_t *stride, uint32_t *version)
{
    assert(*size >= sizeof(descriptors));
    memcpy(map, descriptors, sizeof(descriptors));
    *size = sizeof(descriptors); *key = 1; *stride = sizeof(descriptors[0]); *version = 1;
    return map_result;
}
static efi_boot_services boot_services = {.get_memory_map = get_map};
static efi_boot_services *BS = &boot_services;
struct platform { int unused; };
static struct platform platform_lancelot, other;
static const struct platform *PLAT = &platform_lancelot;
struct payload_info { const char *cmdline; const uint8_t *wifi_board; uint32_t wifi_board_size; };
static void print(const char *format, ...) { (void)format; }
#include "lancelot_wifi.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct lw_board board;
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(&board, 1, sizeof(board), f) == sizeof(board));
    fclose(f);
    descriptors[0] = (efi_memory_descriptor){.type = 0, .phys_start = LW_DMA_PHYS, .pages = LW_DMA_SIZE / 4096};
    descriptors[1] = (efi_memory_descriptor){.type = 0, .phys_start = LW_EMI_PHYS, .pages = LW_EMI_SIZE / 4096};
    struct payload_info p = {.cmdline = "wifi=1", .wifi_board = (const uint8_t *)&board, .wifi_board_size = sizeof(board)};
    assert(lancelot_wifi_loader_prepare(&p));
    assert(lw_handoff_valid(&loader_wifi_handoff, 0xc0000000, 0x7c000000));
    descriptors[0].type = EfiConventionalMemory;
    assert(!lancelot_wifi_loader_prepare(&p));
    assert(!loader_wifi_handoff.flags);
    descriptors[0].type = 0;
    descriptors[0].pages--;
    assert(!lancelot_wifi_loader_prepare(&p));
    descriptors[0].pages++;
    board.firmware_sha256[0][0] ^= 1;
    assert(!lancelot_wifi_loader_prepare(&p));
    board.firmware_sha256[0][0] ^= 1;
    map_result = EFI_NOT_FOUND;
    assert(!lancelot_wifi_loader_prepare(&p));
    map_result = 0;
    p.wifi_board_size--;
    assert(!lancelot_wifi_loader_prepare(&p));
    p.wifi_board = NULL;
    p.cmdline = "connsys_step=2";
    assert(lancelot_wifi_loader_prepare(&p) && !loader_wifi_handoff.flags);
    p.cmdline = "wifi=0";
    assert(lancelot_wifi_loader_prepare(&p) && !loader_wifi_handoff.flags);
    p.cmdline = "wifi=1 wifi=0";
    assert(!lancelot_wifi_loader_prepare(&p));
    PLAT = &other;
    assert(lancelot_wifi_loader_prepare(&p) && !loader_wifi_handoff.flags);
    puts("PASS: production loader rejects missing/corrupt board, nonreserved/gapped pools, failed EFI map and duplicate mode");
    return 0;
}
