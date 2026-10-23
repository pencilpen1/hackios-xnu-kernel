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

/* Bounded, read-only walk of this kernel's 4 KiB bootstrap tables at checkpoint 4. */
static void
lancelot_walk_4k(uint64_t va, int upper)
{
	uint64_t tcr = loader_mmu_state[1];
	unsigned tsz = (unsigned)(tcr >> (upper ? 16 : 0)) & 63;
	unsigned bits = 64 - tsz;
	uint64_t table = loader_mmu_state[upper ? 3 : 2] & 0x0000fffffffff000ULL;
	if (bits < 31 || bits > 39) {
		print("mmu: unsupported bootstrap VA width %d\n", (uint64_t)bits);
		return;
	}
	for (unsigned level = 1; level <= 3; level++) {
		unsigned shift = 12 + 9 * (3 - level);
		uint64_t index = (va >> shift) & 511;
		if (level == 1) index &= (1ULL << (bits - 30)) - 1;
		uint64_t at = table + index * 8;
		if (at < win_base || at > win_base + win_size - 8) {
			print("mmu: table outside kernel window at %x\n", at);
			return;
		}
		/* Bootstrap wrote tables with caches disabled; discard the firmware view. */
		flush_dcache(at, at + 8);
		uint64_t desc = *(const uint64_t *)(uintptr_t)at;
		print("mmu: TTBR%d L%d [%x]=%x\n", (uint64_t)upper, (uint64_t)level, at, desc);
		if (!(desc & 1)) {
			print("mmu: translation INVALID for %x\n", va);
			return;
		}
		if (level == 3 || !(desc & 2)) {
			uint64_t mask = (1ULL << shift) - 1;
			uint64_t pa = (desc & 0x0000fffffffff000ULL & ~mask) | (va & mask);
			print("mmu: %x -> %x\n", va, pa);
			return;
		}
		table = desc & 0x0000fffffffff000ULL;
	}
}

static void
lancelot_mmu_report(uint64_t entry_pa, uint64_t entry_va)
{
	print("mmu: SCTLR=%x TCR=%x TTBR0=%x TTBR1=%x VBAR=%x MAIR=%x\n", loader_mmu_state[0],
	    loader_mmu_state[1], loader_mmu_state[2], loader_mmu_state[3], loader_mmu_state[4], loader_mmu_state[5]);
	if (((loader_mmu_state[1] >> 14) & 3) != 0 || ((loader_mmu_state[1] >> 30) & 3) != 2) return;
	lancelot_walk_4k(entry_pa, 0);
	lancelot_walk_4k(entry_va, 1);
	lancelot_walk_4k(loader_mmu_state[4], 1);
}

/* Position-independent test copied into XNU's mapped, owned window.
 * It enables the checkpoint-4 tables with XNU's SCTLR, then returns through
 * the existing loader stub. Its own vectors record faults without kernel C.
 * This isolates the MMU transition from XNU's checkpoint/exception code. */
extern char lancelot_probe_begin[], lancelot_probe_entry[], lancelot_probe_data[], lancelot_probe_end[];
__asm__(
    ".text\n.balign 2048\n.globl lancelot_probe_begin\nlancelot_probe_begin:\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    "  b lancelot_probe_fault\n.balign 128\n  b lancelot_probe_fault\n.balign 128\n"
    ".globl lancelot_probe_entry\nlancelot_probe_entry:\n"
    "  msr daifset, #0xf\n"
    "  adr x12, lancelot_probe_data\n"
    "  ldr x9, [x12, #0]\n   msr tcr_el1, x9\n"
    "  ldr x9, [x12, #8]\n   msr ttbr0_el1, x9\n"
    "  ldr x9, [x12, #16]\n  msr ttbr1_el1, x9\n"
    "  ldr x9, [x12, #24]\n  msr mair_el1, x9\n"
    "  adr x9, lancelot_probe_begin\n  msr vbar_el1, x9\n"
    "  isb\n  tlbi vmalle1\n  dsb sy\n  isb\n"
    "  mov x9, #1\n  str x9, [x12, #40]\n  dsb sy\n"
    "  ldr x9, [x12, #32]\n  msr sctlr_el1, x9\n  isb\n"
    "  mov x9, #2\n  str x9, [x12, #40]\n"
    "  dc cvac, x12\n  dsb sy\n"
    "  mrs x9, sctlr_el1\n  bic x9, x9, #1\n  bic x9, x9, #4\n  bic x9, x9, #0x1000\n"
    "  msr sctlr_el1, x9\n  isb\n"
    "  adr x12, lancelot_probe_data\n  mov x9, #3\n  str x9, [x12, #40]\n"
    "  ldr x9, [x12, #48]\n  mov x0, #0\n  mov x1, #0\n  mov x2, #2\n  mov x3, #997\n  br x9\n"
    "lancelot_probe_fault:\n"
    "  adr x12, lancelot_probe_data\n"
    "  mrs x3, esr_el1\n  mrs x4, elr_el1\n  mrs x5, far_el1\n"
    "  stp x3, x4, [x12, #64]\n  str x5, [x12, #80]\n"
    "  dc cvac, x12\n  add x9, x12, #64\n  dc cvac, x9\n  dsb sy\n"
    "  mrs x9, sctlr_el1\n  bic x9, x9, #1\n  bic x9, x9, #4\n  bic x9, x9, #0x1000\n"
    "  msr sctlr_el1, x9\n  isb\n"
    "  adr x12, lancelot_probe_data\n  ldr x9, [x12, #48]\n"
    "  mov x0, #0\n  mov x1, #0\n  mov x2, #1\n  mov x6, x30\n  mov x7, sp\n  br x9\n"
    ".balign 64\n.globl lancelot_probe_data\nlancelot_probe_data:\n.space 128\n"
    ".globl lancelot_probe_end\nlancelot_probe_end:\n"
);

static void
lancelot_mmu_probe(void)
{
	const uint64_t address = win_base + 0x08000000;
	const uint64_t bytes = (uint64_t)(lancelot_probe_end - lancelot_probe_begin);
	const uint64_t data_offset = (uint64_t)(lancelot_probe_data - lancelot_probe_begin);
	const uint64_t entry = address + (uint64_t)(lancelot_probe_entry - lancelot_probe_begin);
	if (!win_size || win_size < 0x08001000 || bytes > 4096 ||
	    ((loader_mmu_state[1] >> 14) & 3) != 0 || ((loader_mmu_state[1] >> 30) & 3) != 2) {
		print("mmu-probe: unsupported window/tables\n");
		return;
	}
	memcpy((void *)(uintptr_t)address, lancelot_probe_begin, (size_t)bytes);
	volatile uint64_t *data = (volatile uint64_t *)(uintptr_t)(address + data_offset);
	data[0] = loader_mmu_state[1];
	data[1] = loader_mmu_state[2];
	data[2] = loader_mmu_state[3];
	data[3] = loader_mmu_state[5];
	data[4] = 0x3454593d; /* SCTLR_EL1_DEFAULT in the pinned prebuilt */
	data[5] = 0;
	data[6] = (uint64_t)(uintptr_t)&loader_return_stub;
	print("mmu-probe: DIAGNOSTIC ONLY entry=%x data=%x SCTLR=%x\n", entry, address + data_offset, data[4]);
	lancelot_walk_4k(entry, 0);
	if (loader_save_ctx() == 0) {
		clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));
		clean_dcache((uint64_t)(uintptr_t)loader_mmu_state,
		    (uint64_t)(uintptr_t)loader_mmu_state + sizeof(loader_mmu_state));
		flush_dcache(address, address + bytes);
		jump_to_kernel(entry, 0);
	}
	flush_dcache(address + data_offset, address + bytes);
	print("mmu-probe: stage=%d (3=roundtrip) ESR=%x ELR=%x FAR=%x\n", data[5], data[8], data[9], data[10]);
	print("mmu-probe: restored firmware SCTLR=%x MMU=%d\n", READ_SYSREG(SCTLR_EL1), READ_SYSREG(SCTLR_EL1) & 1);
}

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

#include "lancelot_ubuntu.h"

static efi_status
lancelot_boot(void)
{
	lancelot_attach_log();
	print("lancelot: MTK ESP boot, PSCI SMC, GICv3, firmware GOP\n");
	if ((READ_SYSREG(CurrentEL) >> 2) != 1) {
		print("lancelot: loader needs EL1; refusing an unsafe EL2 handoff\n");
		return EFI_UNSUPPORTED;
	}
	print("MPIDR=%x TGran4=%x TGran16=%x\n", READ_SYSREG(MPIDR_EL1),
	    (READ_SYSREG(ID_AA64MMFR0_EL1) >> 28) & 15, (READ_SYSREG(ID_AA64MMFR0_EL1) >> 20) & 15);
	/* The pinned BCM2837 prebuilt uses TG0=4 KiB, TG1=4 KiB (confirmed in TCR). */
	if (((READ_SYSREG(ID_AA64MMFR0_EL1) >> 28) & 15) == 15) {
		print("lancelot: this CPU cannot run the 4 KiB kernel\n");
		return EFI_UNSUPPORTED;
	}
	if (!lancelot_load_payload()) {
		flush_log();
		if (esp_root) esp_root->close(esp_root);
		esp_root = NULL;
		return EFI_NOT_FOUND;
	}
	if (!lancelot_ubuntu_bootstrap()) {
		print("lancelot: Ubuntu bootstrap failed; root device not provided\n");
		if (esp_root) esp_root->close(esp_root);
		esp_root = NULL;
		return EFI_NOT_FOUND;
	}
	print_memory_map();
	flush_log();
	install_exception_hook();
	print("lancelot: same-core bring-up, no PSCI CPU_ON; USB capture window 30 s\n");
	BS->stall(30000000);
	struct payload_info pi;
	int valid = parse_payload(&pi, 0);
	if (valid && pi.checkpoint == 998) {
		/* A sequence ends at the first failed return, never masks a failed step. */
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
	if (valid && pi.mmu_test && pi.checkpoint == 4 && !pi.ramdisk_size && !pi.bootdisk_size &&
	    !pi.resident_rd && loader_ctx[CTX_REASON] == 2 && loader_ctx[CTX_ESR] == 4) {
		lancelot_mmu_probe(); /* Explicit diagnostic teardown, never executes iOS. */
	}
	print("lancelot: run finished; result=%s\n", last_result);
	/* Preserve the composed kernel log: flush_log() would replace it with loader text. */
	print("lancelot: resident for log retrieval; reboot to leave\n");
	for (;;) BS->stall(100000);
}
