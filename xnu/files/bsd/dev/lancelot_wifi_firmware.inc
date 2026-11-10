/* The bootstrap image is mounted by stage0 on /redwood after rootvnode
 * already exists. init later moves it through /private/preboot/redwood and
 * binds it back on /redwood. Resolve the actual file, retaining its vnode
 * during the read; the existence of the synthetic root is not readiness.
 * This code is also exercised by the host VFS fault-injection test. */
#define CONNSYS_FILE_WAIT_MS 240000u
static uint32_t connsys_files_began;
static bool connsys_files_wait_said, connsys_files_ready_said;

static void
connsys_files_begin(void)
{
    connsys_files_began = connsys_ms();
    connsys_files_wait_said = connsys_files_ready_said = false;
}

static int
connsys_file_open(const char *name, char *path, size_t capacity, vnode_t *vp,
    vfs_context_t context)
{
    static const char *const directories[] = {
        "/redwood/fw/connsys", "/private/preboot/redwood/fw/connsys", "/fw/connsys",
    };
    int errors[3] = { ENOENT, ENOENT, ENOENT };
    *vp = NULLVP;
    for (;;) {
        if (lancelot_wifi_stopping()) return ECANCELED;
        if (rootvnode != NULLVP) {
            for (unsigned i = 0; i < 3; i++) {
                if (lancelot_wifi_stopping()) return ECANCELED;
                int n = snprintf(path, capacity, "%s/%s", directories[i], name);
                if (n < 0 || (size_t)n >= capacity) return ENAMETOOLONG;
                errors[i] = vnode_lookup(path, 0, vp, context);
                if (errors[i] == 0) return 0;
                if (errors[i] != ENOENT && errors[i] != ENOTDIR) {
                    lancelot_wifi_log("connsys: firmware lookup %s failed: errno=%d", path, errors[i]);
                    return errors[i];
                }
            }
        }
        if ((uint32_t)(connsys_ms() - connsys_files_began) >= CONNSYS_FILE_WAIT_MS) {
            lancelot_wifi_log("connsys: firmware mount timed out: %s, lookup errno=%d/%d/%d (redwood/preboot/root)",
                name, errors[0], errors[1], errors[2]);
            return ETIMEDOUT;
        }
        if (!connsys_files_wait_said) {
            connsys_files_wait_said = true;
            lancelot_wifi_log("connsys: waiting for bootstrap firmware files (up to 240 s)");
        }
        /* A sleep, not a busy poll; cancellation is observed within 100 ms. */
        delay_for_interval(100, NSEC_PER_MSEC);
    }
}

static uint8_t *
connsys_file(const char *name, uint32_t *size)
{
    if (!size) return NULL;
    *size = 0;
    if (!name || strchr(name, '/') || strlen(name) > 64 || lancelot_wifi_stopping()) return NULL;
    const struct lw_board *board = lancelot_wifi_board();
    if (!board) return NULL;
    unsigned index;
    for (index = 0; index < LW_FIRMWARE_FILES; index++)
        if (!strcmp(name, lw_firmware_name(index))) break;
    if (index == LW_FIRMWARE_FILES) return NULL;

    char path[128];
    vfs_context_t context = vfs_context_kernel();
    vnode_t vnode = NULLVP;
    if (connsys_file_open(name, path, sizeof(path), &vnode, context)) return NULL;
    if (lancelot_wifi_stopping()) { vnode_put(vnode); return NULL; }
    struct vnode_attr attributes = {0};
    VATTR_INIT(&attributes);
    VATTR_WANTED(&attributes, va_data_size);
    int error = vnode_getattr(vnode, &attributes, context);
    if (error || !vnode_isreg(vnode) || !VATTR_IS_SUPPORTED(&attributes, va_data_size) ||
        attributes.va_data_size < 48 || attributes.va_data_size > CONNSYS_EMI_SIZE) {
        lancelot_wifi_log("connsys: firmware attributes %s: errno=%d regular=%d size=%llu supported=%d",
            path, error, vnode_isreg(vnode), (unsigned long long)attributes.va_data_size,
            !!VATTR_IS_SUPPORTED(&attributes, va_data_size));
        vnode_put(vnode);
        return NULL;
    }
    size_t length = (size_t)attributes.va_data_size;
    if (lancelot_wifi_stopping()) { vnode_put(vnode); return NULL; }
    uint8_t *data = kalloc_data(length, Z_WAITOK);
    if (!data) {
        lancelot_wifi_log("connsys: firmware allocation %s failed: %zu bytes", path, length);
        vnode_put(vnode);
        return NULL;
    }
    if (lancelot_wifi_stopping()) { vnode_put(vnode); kfree_data(data, length); return NULL; }
    int left = 0;
    error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)length, 0, UIO_SYSSPACE, 0,
        vfs_context_ucred(context), &left, vfs_context_proc(context));
    vnode_put(vnode);
    if (error || left || lancelot_wifi_stopping()) {
        if (error || left) lancelot_wifi_log("connsys: firmware read %s: errno=%d residual=%d/%zu",
            path, error, left, length);
        kfree_data(data, length);
        return NULL;
    }
    uint8_t digest[32];
    sha256(data, length, digest);
    if (lancelot_wifi_stopping()) { kfree_data(data, length); return NULL; }
    if (memcmp(digest, board->firmware_sha256[index], sizeof(digest))) {
        lancelot_wifi_log("connsys: firmware/calibration digest mismatch: %s (%zu bytes)", name, length);
        kfree_data(data, length);
        return NULL;
    }
    if (!connsys_files_ready_said) {
        connsys_files_ready_said = true;
        lancelot_wifi_log("connsys: firmware readable at %s after %u ms; SHA-256 verified",
            path, (uint32_t)(connsys_ms() - connsys_files_began));
    }
    *size = (uint32_t)length;
    return data;
}

static bool
connsys_firmware_preflight(void)
{
    static const uint32_t offsets[] = { 0, 0x80000, 0x140000 };
    for (unsigned i = 0; i < LW_FIRMWARE_FILES; i++) {
        uint32_t size = 0, offset = 0;
        const char *name = lw_firmware_name(i);
        uint8_t *data = connsys_file(name, &size);
        if (!data) return false; /* The reader reports the exact failing stage. */
        bool ok;
        struct lw_fw_plan plan;
        if (i < 3) ok = lw_rom_image(data, size, offsets[i], &offset);
        else if (i == 3) ok = size > 30 && size <= 65536 && !memcmp(data + 16, "ALPS", 4) &&
            data[20] == 0x8a && data[21] == 0 && data[22] == 0x8a && data[23] == 0 &&
            lw_get_le32(data + 24) == 0x0001c011;
        else if (i == 4) ok = lw_fw_parse(data, size, &plan);
        else ok = size == LW_NVRAM_SIZE + 2 && data[0] == 1 && data[1] == 0 &&
            data[2] == 9 && data[3] == 0 && data[size - 2] == 0xaa && data[size - 1] == 0xac;
        kfree_data(data, size);
        if (!ok) {
            lancelot_wifi_log("connsys: firmware header/layout rejected: %s (%u bytes, type=%u)", name, size, i);
            return false;
        }
        if (lancelot_wifi_stopping()) return false;
    }
    lancelot_wifi_log("connsys: all six firmware/calibration files passed SHA-256 and format preflight");
    return true;
}
