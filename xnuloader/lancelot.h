/* MT6768 ESP boot path. Included after the common loader implementation. */
static void
save_esp_log(const char *data, uint64_t size)
{
	efi_file *file;
	static const char16 path[] = u"\\EFI\\HackiOS\\last.log";
	if (!EFI_ERROR(esp_root->open(esp_root, &file, path, 3, 0))) {
		if (EFI_ERROR(file->delete_file(file))) {
			print("esp: could not replace last.log\n");
			return;
		}
	}
	efi_status s = esp_root->open(esp_root, &file, path, 0x8000000000000003ULL, 0);
	if (EFI_ERROR(s)) {
		print("esp: opening last.log failed %x\n", s);
		return;
	}
	uint64_t written = size;
	s = file->write(file, &written, data);
	efi_status flushed = file->flush(file);
	file->close(file);
	if (EFI_ERROR(s) || EFI_ERROR(flushed) || written != size)
		print("esp: log write=%x flush=%x bytes=%d/%d\n", s, flushed, written, size);
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
	print("lancelot: MTK ESP boot, PSCI SMC, GICv3, firmware GOP\n");
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
	run_kernel(0, 0);
	print("lancelot: run finished; result=%s; log=EFI/HackiOS/last.log\n", last_result);
	/* Preserve the composed kernel log: flush_log() would replace it with loader text. */
	print("lancelot: resident for log retrieval; reboot to leave\n");
	for (;;) BS->stall(100000);
}
