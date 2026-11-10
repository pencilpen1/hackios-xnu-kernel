#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lancelot_wifi_contract.h"
#include "lancelot_sha256.h"

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length > 0 && length <= 4 * 1024 * 1024);
    rewind(f);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    *size = (size_t)length;
    return bytes;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    size_t length;
    uint8_t *raw = read_file(argv[1], &length);
    struct lw_board board;
    assert(length == sizeof(board));
    memcpy(&board, raw, sizeof(board));
    free(raw);
    assert(lw_board_valid(&board));
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", argv[2], lw_firmware_name(i));
        assert(n > 0 && (size_t)n < sizeof(path));
        raw = read_file(path, &length);
        uint8_t digest[32];
        sha256(raw, length, digest);
        assert(!memcmp(digest, board.firmware_sha256[i], sizeof(digest)));
        raw[length / 2] ^= 1;
        sha256(raw, length, digest);
        assert(memcmp(digest, board.firmware_sha256[i], sizeof(digest)));
        free(raw);
    }
    board.firmware_sha256[0][0] ^= 1;
    assert(!lw_board_valid(&board));
    puts("PASS: actual 224-byte private board ABI; all six firmware/NVRAM SHA-256 bindings and corruption rejection");
    return 0;
}
