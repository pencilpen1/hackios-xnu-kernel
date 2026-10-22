/* MT6768 ESP boot path. Included after the common loader implementation. */
/* ABI of SiliciumPkg/Library/BootLogLib (BSD-2-Clause-Patent), revision 1.
 * Only attach to an initialized ring in a UEFI reserved-memory descriptor.
 * The driver owns USB and its IRQs; the loader only produces log characters. */
#define MTK_LOG_BASE 0x512c0000ULL
#define MTK_LOG_SIZE 0x30000u
struct mtk_boot_log {
	uint32_t magic, version;
	uint64_t boot_id, written;
	uint32_t busy, usb_active;
	uint8_t data[MTK_LOG_SIZE - 32];
};
_Static_assert(sizeof(struct mtk_boot_log) == MTK_LOG_SIZE, "MTK boot log ABI");
static volatile struct mtk_boot_log *mtk_log;

static void
lancelot_log_char(char c)
{
	uint64_t daif = READ_SYSREG(DAIF);
	__asm__ volatile ("msr daifset, #2" ::: "memory");
	if (mtk_log->magic == 0x474f4c4c && mtk_log->version == 1 && !mtk_log->busy &&
	    mtk_log->written != UINT64_MAX) {
		mtk_log->busy = 1;
		uint64_t pos = mtk_log->written;
		mtk_log->data[pos % sizeof(mtk_log->data)] = (uint8_t)c;
		__asm__ volatile ("dmb ishst" ::: "memory");
		mtk_log->written = pos + 1;
		mtk_log->busy = 0;
	}
	__asm__ volatile ("msr daif, %0" :: "r" (daif) : "memory");
}

static void
lancelot_attach_log(void)
{
	static uint8_t map[65536];
	uint64_t size = sizeof(map), key, stride;
	uint32_t version;
	if (EFI_ERROR(BS->get_memory_map(&size, map, &key, &stride, &version)) ||
	    stride < sizeof(efi_memory_descriptor)) return;
	for (uint64_t off = 0; off + stride <= size; off += stride) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map + off);
		if (d->type != 0 || d->phys_start > MTK_LOG_BASE ||
		    d->phys_start + (d->pages << 12) < MTK_LOG_BASE + MTK_LOG_SIZE) continue;
		mtk_log = (volatile struct mtk_boot_log *)(uintptr_t)MTK_LOG_BASE;
		if (mtk_log->magic != 0x474f4c4c || mtk_log->version != 1) {
			mtk_log = NULL;
			return;
		}
		platform_log_char = lancelot_log_char;
		for (uint32_t i = 0; i < log_len; i++) platform_log_char(log_buf[i]);
		print("lancelot: attached UEFI MUSB LLOG1 ring, USB active=%d\n", (uint64_t)mtk_log->usb_active);
		return;
	}
}

static int
lancelot_load_payload(void)
{
	static const efi_guid image_guid = EFI_LOADED_IMAGE_GUID, fs_guid = EFI_SIMPLE_FS_GUID;
	efi_loaded_image *image;
	efi_simple_fs *fs;
	efi_file *file;
	efi_status s;
	struct payload_header header;
	uint64_t size = sizeof(header), address = 0;

	if (EFI_ERROR(BS->handle_protocol(IMAGE, &image_guid, (void **)&image)) ||
	    EFI_ERROR(BS->handle_protocol(image->device, &fs_guid, (void **)&fs)) ||
	    EFI_ERROR(fs->open_volume(fs, &esp_root))) {
		print("esp: cannot open loader's filesystem\n");
		return 0;
	}
	s = esp_root->open(esp_root, &file, u"\\EFI\\HackiOS\\boot.xnup", 1, 0);
	if (EFI_ERROR(s)) {
		print("esp: boot.xnup not found (%x)\n", s);
		return 0;
	}
	s = file->read(file, &size, &header);
	/* The last MiB belongs to the panic log, not the file. */
	if (EFI_ERROR(s) || size != sizeof(header) || header.magic != PAYLOAD_MAGIC ||
	    header.size < sizeof(struct payload_entry) ||
	    header.size > PAYLOAD_MAX_SIZE - 0x100000 - sizeof(header)) {
		print("esp: invalid/oversize XNUP header\n");
		file->close(file);
		return 0;
	}
	s = BS->allocate_pages(AllocateAnyPages, EfiLoaderData, PAYLOAD_MAX_SIZE >> 12, &address);
	if (EFI_ERROR(s)) {
		print("esp: cannot reserve payload memory (%x)\n", s);
		file->close(file);
		return 0;
	}
	memcpy((void *)(uintptr_t)address, &header, sizeof(header));
	uint64_t remaining = header.size, offset = sizeof(header);
	while (remaining) {
		size = remaining > 0x100000 ? 0x100000 : remaining;
		uint64_t requested = size;
		s = file->read(file, &size, (void *)(uintptr_t)(address + offset));
		if (EFI_ERROR(s) || size == 0 || size > requested) {
			print("esp: short/failed payload read at %x (%x)\n", offset, s);
			file->close(file);
			BS->free_pages(address, PAYLOAD_MAX_SIZE >> 12);
			return 0;
		}
		offset += size;
		remaining -= size;
	}
	file->close(file);
	payload_base = address;
	print("esp: loaded %d bytes at %x; panic log %x\n", offset, address, PANIC_LOG_BASE);
	return 1;
}

static efi_status
lancelot_boot(void)
{
	lancelot_attach_log();
	print("lancelot: MTK ESP boot, PSCI SMC, GICv3, firmware GOP\n");
	if ((READ_SYSREG(CurrentEL) >> 2) != 1) {
		print("lancelot: loader needs EL1; refusing an unsafe EL2 handoff\n");
		return EFI_UNSUPPORTED;
	}
	print("MPIDR=%x TGran16=%x\n", READ_SYSREG(MPIDR_EL1), (READ_SYSREG(ID_AA64MMFR0_EL1) >> 20) & 15);
	/* TGran16=0 means unsupported. XNU here uses 16 KiB stage-1 pages. */
	if (((READ_SYSREG(ID_AA64MMFR0_EL1) >> 20) & 15) == 0) {
		print("lancelot: this CPU cannot run the 16 KiB kernel\n");
		return EFI_UNSUPPORTED;
	}
	if (!lancelot_load_payload()) {
		flush_log();
		if (esp_root) esp_root->close(esp_root);
		esp_root = NULL;
		return EFI_NOT_FOUND;
	}
	print_memory_map();
	flush_log();
	install_exception_hook();
	struct payload_info pi;
	if (parse_payload(&pi, 0) && pi.checkpoint == 998) {
		/* A sequence ends at the first failed return, never masks a failed step. */
		/* Avoid the same-core context selftest: keep USB alive on firmware core. */
		static const uint64_t checkpoints[] = { 1, 2, 3, 4, 5, 10 };
		for (unsigned i = 0; i < sizeof(checkpoints) / sizeof(checkpoints[0]); i++) {
			uint64_t n = checkpoints[i];
			print("lancelot: begin checkpoint %d\n", n);
			if (!run_kernel(1, n) || loader_ctx[CTX_REASON] != 2 || loader_ctx[CTX_ESR] != n) {
				print("lancelot: checkpoint sequence stopped at %d\n", n);
				break;
			}
		}
	} else {
		run_kernel(0, 0);
	}
	print("lancelot: run finished; result=%s\n", last_result);
	/* Preserve the composed kernel log: flush_log() would replace it with loader text. */
	print("lancelot: resident for log retrieval; reboot to leave\n");
	for (;;) BS->stall(100000);
}
