/* Compile the production firmware reader against an injected VFS/clock. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "lancelot_wifi_wire.h"
#include "lancelot_sha256.h"

#define CONNSYS_EMI_SIZE LW_EMI_SIZE
#define NSEC_PER_MSEC 1000000u
#define Z_WAITOK 0
#define UIO_READ 0
#define UIO_SYSSPACE 0
struct test_node { unsigned index, refs; uint8_t *data; size_t length; };
typedef struct test_node *vnode_t;
typedef void *vfs_context_t;
#define NULLVP NULL
struct vnode_attr { uint64_t va_data_size; bool supported; };
#define VATTR_INIT(a) memset((a), 0, sizeof(*(a)))
#define VATTR_WANTED(a, field) ((void)(a))
#define VATTR_IS_SUPPORTED(a, field) ((a)->supported)

static struct test_node nodes[LW_FIRMWARE_FILES];
static struct lw_board board;
static vnode_t rootvnode;
static uint32_t clock_base, elapsed, root_at, files_at, stop_at;
static int alias, lookup_error, attr_error, read_error, residual, missing;
static bool regular, supported, allocation_fails, stopping, pivot;
static int stop_stage;
static uint64_t reported_size;
static unsigned reads, lookups, waits, logs, live, held, cases;
static size_t allocated_bytes;
static char last_log[512];

static uint32_t connsys_ms(void) { return clock_base + elapsed; }
static bool lancelot_wifi_stopping(void) { return stopping || elapsed >= stop_at; }
static const struct lw_board *lancelot_wifi_board(void) { return &board; }
static void delay_for_interval(uint32_t n, uint32_t units)
{
    assert(n == 100 && units == NSEC_PER_MSEC);
    elapsed += n;
    waits++;
    if (elapsed >= root_at) rootvnode = &nodes[0];
}
static void lancelot_wifi_log(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsnprintf(last_log, sizeof(last_log), format, ap);
    va_end(ap);
    logs++;
}
static vfs_context_t vfs_context_kernel(void) { return &board; }
static void *vfs_context_ucred(vfs_context_t c) { return c; }
static void *vfs_context_proc(vfs_context_t c) { return c; }
static int vnode_lookup(const char *path, int flags, vnode_t *out, vfs_context_t context)
{
    static const char *const prefixes[] = {
        "/redwood/fw/connsys/", "/private/preboot/redwood/fw/connsys/", "/fw/connsys/",
    };
    assert(rootvnode && !flags && context == &board);
    lookups++;
    if (lookup_error) return lookup_error;
    const char *name = strrchr(path, '/') + 1;
    if (elapsed < files_at || strncmp(path, prefixes[alias], strlen(prefixes[alias]))) return ENOENT;
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        if (!strcmp(name, lw_firmware_name(i))) {
            if ((int)i == missing) return ENOENT;
            *out = &nodes[i];
            nodes[i].refs++;
            held++;
            if (stop_stage == 1) stopping = true;
            return 0;
        }
    }
    return ENOENT;
}
static int vnode_put(vnode_t v) { assert(v->refs && held); v->refs--; held--; return 0; }
static int vnode_isreg(vnode_t v) { assert(v->refs); return regular; }
static int vnode_getattr(vnode_t v, struct vnode_attr *a, vfs_context_t c)
{
    assert(v->refs && c == &board);
    if (attr_error) return attr_error;
    a->va_data_size = reported_size ? reported_size : v->length;
    a->supported = supported;
    if (stop_stage == 4) stopping = true;
    return 0;
}
static void *kalloc_data(size_t n, int flags)
{
    assert(flags == Z_WAITOK && !live && n <= LW_EMI_SIZE);
    if (allocation_fails) return NULL;
    void *p = malloc(n);
    assert(p);
    live++;
    allocated_bytes = n;
    if (stop_stage == 5) stopping = true;
    return p;
}
static void kfree_data(void *p, size_t n)
{
    assert(p && live == 1 && n == allocated_bytes);
    live--;
    free(p);
}
static int vn_rdwr(int op, vnode_t v, caddr_t out, int n, int off, int space,
    int flags, void *cred, int *left, void *proc)
{
    assert(op == UIO_READ && v->refs && n > 0 && off == 0 && space == UIO_SYSSPACE);
    assert(!flags && cred == &board && proc == &board);
    reads++;
    if (stop_stage == 2) stopping = true;
    if (pivot) { alias = 1; files_at = elapsed + 300; pivot = false; }
    if (read_error) return read_error;
    assert((size_t)n <= v->length && residual >= 0 && residual <= n);
    *left = residual;
    memcpy(out, v->data, (size_t)(n - residual));
    return 0;
}
static void tested_sha256(const void *data, size_t n, uint8_t *digest)
{
    sha256(data, n, digest);
    if (stop_stage == 3) stopping = true;
}
#define sha256 tested_sha256
#include "lancelot_wifi_firmware.inc"
#undef sha256

static void reset(void)
{
    assert(!held && !live);
    clock_base = 5000;
    elapsed = root_at = files_at = 0;
    stop_at = UINT32_MAX;
    rootvnode = &nodes[0];
    alias = lookup_error = attr_error = read_error = residual = stop_stage = 0;
    missing = -1;
    regular = supported = true;
    allocation_fails = stopping = pivot = false;
    reported_size = 0;
    reads = lookups = waits = logs = 0;
    last_log[0] = 0;
    connsys_files_begin();
    cases++;
}
static void expect_failure(const char *message)
{
    bool ok = connsys_firmware_preflight();
    if (ok) fprintf(stderr, "Unexpected firmware acceptance in scenario %u (%s)\n", cases, message ? message : "cancel");
    assert(!ok);
    assert(!held && !live && logs <= 3);
    if (message) assert(strstr(last_log, message));
}
static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n > 0 && n <= LW_EMI_SIZE);
    rewind(f);
    uint8_t *p = malloc((size_t)n);
    assert(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return p;
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    size_t bytes;
    uint8_t *raw = load(argv[1], &bytes);
    assert(bytes == sizeof(board));
    memcpy(&board, raw, bytes);
    free(raw);
    assert(lw_board_valid(&board));
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", argv[2], lw_firmware_name(i));
        assert(n > 0 && (size_t)n < sizeof(path));
        nodes[i].index = i;
        nodes[i].data = load(path, &nodes[i].length);
    }
    for (int a = 0; a < 3; a++) {
        reset(); alias = a;
        assert(connsys_firmware_preflight() && reads == LW_FIRMWARE_FILES && !waits && !held && !live);
    }
    reset(); rootvnode = NULLVP; root_at = 300; files_at = 2300;
    assert(connsys_firmware_preflight() && elapsed == files_at && waits == 23 && logs == 3);
    reset(); pivot = true;
    assert(connsys_firmware_preflight() && reads == LW_FIRMWARE_FILES && elapsed == 300 && !held && !live);
    reset(); files_at = UINT32_MAX;
    expect_failure("mount timed out");
    assert(elapsed == CONNSYS_FILE_WAIT_MS && !reads && logs == 2);
    reset(); missing = 1; files_at = 239700;
    expect_failure("mount timed out");
    assert(elapsed == CONNSYS_FILE_WAIT_MS && reads == 1); /* One budget for all files. */
    reset(); clock_base = UINT32_MAX - 400; connsys_files_begin(); files_at = 1000;
    assert(connsys_firmware_preflight() && elapsed == 1000); /* Timer wrap. */
    reset(); lookup_error = ENOTDIR;
    expect_failure("mount timed out");
    assert(elapsed == CONNSYS_FILE_WAIT_MS && !reads);
    reset(); lookup_error = EACCES;
    expect_failure("lookup"); assert(!waits && !reads && lookups == 1);
    reset(); attr_error = EIO;
    expect_failure("attributes"); assert(!waits && !reads);
    reset(); regular = false; expect_failure("attributes");
    reset(); supported = false; expect_failure("attributes");
    reset(); reported_size = 47; expect_failure("attributes");
    reset(); reported_size = LW_EMI_SIZE + 1; expect_failure("attributes");
    reset(); allocation_fails = true; expect_failure("allocation");
    reset(); read_error = EIO; expect_failure("read"); assert(reads == 1 && !waits);
    reset(); residual = 17; expect_failure("residual=17"); assert(reads == 1 && !waits);
    reset(); files_at = 5000; stop_at = 300;
    expect_failure(NULL); assert(elapsed == stop_at && !reads);
    reset(); stopping = true; expect_failure(NULL); assert(!lookups && !reads);
    for (int stage = 1; stage <= 5; stage++) {
        reset(); stop_stage = stage;
        expect_failure(NULL); assert(!waits && reads == (unsigned)(stage == 2 || stage == 3));
    }
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        reset();
        size_t at = nodes[i].length / 2;
        nodes[i].data[at] ^= 1;
        expect_failure("digest mismatch"); assert(!waits && reads == i + 1);
        nodes[i].data[at] ^= 1;
    }
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        reset();
        uint8_t original_digest[32];
        memcpy(original_digest, board.firmware_sha256[i], 32);
        size_t at = i < 4 ? 20 : i == 4 ? nodes[i].length - 36 + 2 : 0;
        uint8_t original = nodes[i].data[at];
        nodes[i].data[at] = i == 4 ? 0 : (uint8_t)(original ^ 1);
        sha256(nodes[i].data, nodes[i].length, board.firmware_sha256[i]);
        expect_failure("header/layout"); assert(!waits);
        nodes[i].data[at] = original;
        memcpy(board.firmware_sha256[i], original_digest, 32);
    }
    reset();
    uint32_t size = 42;
    assert(!connsys_file("../WIFI", &size) && !size && !lookups);
    size = 42;
    assert(!connsys_file("unknown", &size) && !size && !lookups);
    assert(!held && !live);
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) free(nodes[i].data);
    printf("PASS: %u production firmware/VFS scenarios; actual six-file bundle, mount/pivot, cancellation, bounded waits, I/O and corruption rejection\n", cases);
    return 0;
}
