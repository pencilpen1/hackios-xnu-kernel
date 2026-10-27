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
		if (mtk_log->usb_active) {
			mtk_usb_offer[0] = 0x31474f4c4253554cULL;
			mtk_usb_offer[1] = 0x11200000;
			mtk_usb_offer[2] = mtk_log->boot_id;
		}
		for (uint32_t i = 0; i < log_len; i++) platform_log_char(log_buf[i]);
		print("lancelot: attached UEFI MUSB LLOG1 ring, USB active=%d\n", (uint64_t)mtk_log->usb_active);
		return;
	}
}

/* XNU leaves USB detached on return. Firmware regains its own state machine
 * on the next host bus reset. This is an EFI application change only. */
static void
lancelot_usb_resume(void)
{
	if (!mtk_usb_offer[0]) return;
	volatile uint8_t *mac = (volatile uint8_t *)(uintptr_t)mtk_usb_offer[1];
	mac[1] = 0;
	BS->stall(30000);
	*(volatile uint16_t *)(mac + 6) = 3;
	*(volatile uint16_t *)(mac + 8) = 2;
	mac[11] = 4;
	__asm__ volatile ("dsb sy" ::: "memory");
	mac[1] = 0x40;
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
#include "lancelot_pmic.h"
#include "lancelot_gpu_prepare.h"
#include "lancelot_gpu_dma_native.h"

static void lancelot_gpu_save_receipt(const struct lpg_result*r,int pool_error,bool dma) {
	struct lmbr_receipt*p=&mtk_gpu_receipt;struct lfd_context*f=&lancelot_dma_context;
	struct lm_private_dma*s=&f->transaction;memset(p,0,sizeof(*p));
	p->pool_pa=f->pa;p->sequence=s->job_result.sequence;p->fault_pa=s->job_result.fault_address;
	uint32_t*w=p->word;
	w[LMBR_ERROR]=(uint32_t)r->error;w[LMBR_TEARDOWN]=(uint32_t)r->teardown_error;
	w[LMBR_GPU_ID]=r->gpu_id;w[LMBR_GPU_STATUS]=r->gpu_status;w[LMBR_HZ]=r->source_hz;
	w[LMBR_PWR_WRITES]=0;w[LMBR_STARTED]=r->started;w[LMBR_OFF]=r->domains_restored;
	w[LMBR_FEATURE_VALID]=r->features_valid;w[LMBR_DMA_REQUEST]=dma;
	w[LMBR_DMA_ATTEMPT]=r->dma_attempted;w[LMBR_DMA_SAFE]=r->dma_safe_to_teardown;
	w[LMBR_DMA_HALT]=dma&&r->dma_halt_required;w[LMBR_POOL_ERROR]=(uint32_t)pool_error;
	w[LMBR_RETAINED]=f->allocated;w[LMBR_PEAK]=f->peak_pages;w[LMBR_CLEANS]=f->cleans;
	w[LMBR_INVALIDATES]=f->invalidates;w[LMBR_GPU_READS]=f->gpu_reads;w[LMBR_GPU_WRITES]=f->gpu_writes;
	w[LMBR_IO_ERROR]=(uint32_t)f->io_error;w[LMBR_RELEASE_ERROR]=(uint32_t)f->release_error;
	w[LMBR_SUBMITTED]=s->submitted;w[LMBR_GUARDS]=s->guards_valid;w[LMBR_BAD_OFFSET]=s->bad_offset;
	w[LMBR_BAD_JOB]=s->bad_job;w[LMBR_CLEANUP]=(uint32_t)s->cleanup_error;w[LMBR_VALUE]=s->observed_value;
	w[LMBR_JOB_IRQ]=s->job_result.job_irq;w[LMBR_JOB_STATUS]=s->job_result.job_status;
	w[LMBR_MMU]=s->job_result.mmu_fault;w[LMBR_GPU_FAULT]=s->job_result.gpu_fault;
	w[LMBR_JOB_STAGE]=s->job_result.failed_stage;w[LMBR_MISMATCH_REG]=s->job_result.mismatch_reg;
	w[LMBR_EXPECTED_LO]=(uint32_t)s->job_result.expected;w[LMBR_EXPECTED_HI]=(uint32_t)(s->job_result.expected>>32);
	w[LMBR_OBSERVED_LO]=(uint32_t)s->job_result.observed;w[LMBR_OBSERVED_HI]=(uint32_t)(s->job_result.observed>>32);
	w[LMBR_RESET_COMPLETED]=s->job_result.reset_completed;w[LMBR_QUARANTINED]=s->job_result.quarantined;
	w[LMBR_TEARDOWN_STATE]=r->teardown_state;
	w[LMBR_FIRST_IO_ERROR]=(uint32_t)f->first_io_error;w[LMBR_FIRST_IO_REG]=f->first_io_reg;
	w[LMBR_FIRST_IO_PHASE]=f->first_io_phase;w[LMBR_RECOVERY_ATTEMPT]=f->recovery_attempted;
	w[LMBR_RECOVERY_ACTIVE]=f->recovering;w[LMBR_LAST_PHASE]=f->phase;
	w[LMBR_TRANSPORT_ERROR]=(uint32_t)(f->platform?f->platform->transport_error:0);
	w[LMBR_TRANSPORT_ADDRESS]=f->platform?f->platform->transport_address:0;
	w[LMBR_JOB_STATE]=s->job_result.job_state;
	w[LMBR_MMU_FAULT_TEST]=s->mmu_fault_test;w[LMBR_EXPECTED_FAULT]=s->expected_fault;
	w[LMBR_EXPECTED_VA_LO]=(uint32_t)s->expected_fault_va;
	w[LMBR_EXPECTED_VA_HI]=(uint32_t)(s->expected_fault_va>>32);
	if(r->features_valid)memcpy(w+LMBR_FEATURE_FIRST,r->features.word,sizeof(r->features.word));
	lmbr_seal(p,mtk_usb_offer[2]);mtk_gpu_receipt_ready=true;
}
static void lancelot_gpu_print_receipt(void) {
	const struct lmbr_receipt*r=&mtk_gpu_receipt;
	if(!lmbr_valid(r,sizeof(*r),r->boot_id)) {
		print("lancelot-gpu-receipt: invalid local CRC/boot; no evidence published\n");return;
	}
	print("lancelot-gpu-receipt: source=UEFI boot=%x pool=%x sequence=%d fault=%x size=%d crc=%x verified=1 version=%d\n",
	    r->boot_id,r->pool_pa,r->sequence,r->fault_pa,(uint64_t)r->size,(uint64_t)r->crc,(uint64_t)r->version);
	int quiet=screen_quiet;screen_quiet=1;
	for(unsigned i=0;i<96;i++)print("lancelot-gpu-receipt: word=%d value=%x\n",(uint64_t)i,(uint64_t)r->word[i]);
	screen_quiet=quiet; /* Full CRC record stays in both RAM logs, not 96 screen scrolls. */
}

static uint64_t lancelot_gpu_now(void *cookie) {
	(void)cookie;
	uint64_t ticks=READ_SYSREG(CNTVCT_EL0),frequency=READ_SYSREG(CNTFRQ_EL0);
	return (ticks/frequency)*1000000000u+(ticks%frequency)*1000000000u/frequency;
}
static void lancelot_gpu_delay(void *cookie,uint32_t us) {(void)cookie;BS->stall(us);}
static int lancelot_gpu_read(void *cookie,uint64_t pa,uint32_t *value) {
	(void)cookie;
	__asm__ volatile("dsb sy" ::: "memory");
	*value=*(const volatile uint32_t *)(uintptr_t)pa;
	__asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int lancelot_gpu_write(void *cookie,uint64_t pa,uint32_t value) {
	(void)cookie;
	*(volatile uint32_t *)(uintptr_t)pa=value;
	__asm__ volatile("dsb sy" ::: "memory");
	/* GPU accesses are recorded once by lfd_read/write. Platform writes join
	 * that bounded RAM trace; no framebuffer/USB printing inside the deadline. */
	if(!(pa>=lpg_base[3] && pa<lpg_base[3]+0x4000u)) {
		if(lancelot_dma_context.initialized)
			lfd_record_trace(&lancelot_dma_context,false,(uint32_t)pa,value,0);
		else print("lancelot-gpu-power: write addr=%x value=%x\n",pa,(uint64_t)value);
	}
	return 0;
}
static bool lancelot_gpu_power_probe(const char *cmdline) {
	int requested=lpm_requested_named(cmdline,"lancelot_gpu_power=");
	int features=lpm_requested_named(cmdline,"lancelot_gpu_features=");
	int dma=lpm_requested_named(cmdline,"lancelot_gpu_dma=");
	int fault=lpm_requested_named(cmdline,"lancelot_gpu_fault=");
	int clear=lpm_requested_named(cmdline,"lancelot_gpu_clear=");
	int chain=lpm_requested_named(cmdline,"lancelot_gpu_clear_chain=");
	int triangle=lpm_requested_named(cmdline,"lancelot_gpu_triangle=");
	if(features<0 || dma<0 || fault<0 || clear<0 || chain<0 || triangle<0 || (features && requested!=1) || (dma && features!=1) ||
	   (fault && dma!=1) || (clear && dma!=1) || (clear && fault) || (chain && clear!=1) ||
	   (triangle && dma!=1) || (triangle && (fault || clear || chain))) {
		print("lancelot-gpu-features: invalid/duplicate/dependency mode rejected; no MMIO\n");return true;
	}
	if(!requested)return true;
	if(requested!=1){print("lancelot-gpu-power: invalid/duplicate mode rejected; no MMIO\n");return true;}
	efi_mtk_gpu_map *map=NULL;efi_mtk_pwrap_checked *pmic=NULL;
	const efi_guid map_guid=EFI_MTK_GPU_MAP_GUID,pmic_guid=EFI_MTK_PWRAP_CHECKED_GUID;
	if(EFI_ERROR(BS->locate_protocol(&map_guid,NULL,(void**)&map)) || !lpg_map_valid(map) ||
	   EFI_ERROR(BS->locate_protocol(&pmic_guid,NULL,(void**)&pmic)) || !pmic || !pmic->read ||
	   !BS->raise_tpl || !BS->restore_tpl || READ_SYSREG(CNTFRQ_EL0)<1000000 || READ_SYSREG(CNTFRQ_EL0)>1000000000) {
		print("lancelot-gpu-power: checked mapping/provider contract absent; no MMIO\n");return !dma;
	}
	struct lpg_context context={.map=map,.pmic=pmic,.read=lancelot_gpu_read,.write=lancelot_gpu_write,
	    .now_ns=lancelot_gpu_now,.delay_us=lancelot_gpu_delay,.features_requested=features==1,
	    .dma_requested=dma==1,.dma_cookie=&lancelot_dma_context,.dma_run=lfd_run};
	if(dma) {
		lancelot_dma_context.transaction.mmu_fault_test=fault==1;
		lancelot_dma_context.clear_mode=clear==1;
		lancelot_dma_context.clear_chain=chain==1;
		lancelot_dma_context.triangle_mode=triangle==1;
		int error=lfd_prepare(&lancelot_dma_context,&context,&lancelot_dma_memory);
		print("lancelot-gpu-dma: pool error=%x base=%x pages=%d clear=%d chain=%d triangle=%d; no GPU writes unless normal-WB CPU/PA checks pass\n",
		    (uint64_t)(uint32_t)error,lancelot_dma_context.pa,(uint64_t)LFD_PAGES,
		    (uint64_t)(clear==1),(uint64_t)(chain==1),(uint64_t)(triangle==1));
		if(error) {
			struct lpg_result failed={.error=error,.dma_halt_required=true};lancelot_gpu_save_receipt(&failed,error,true);
			lancelot_gpu_print_receipt();
			return false;
		}
	}
	uint64_t previous_tpl=BS->raise_tpl(16);
	struct lpg_result result=lpg_run(&context);
	uint64_t lifecycle_ns=lancelot_gpu_now(NULL)-context.start;
	BS->restore_tpl(previous_tpl);
	if(dma)lpg_dma_finish_handoff(&result,!lancelot_dma_context.allocated &&
	    !lancelot_dma_context.release_error && !lancelot_dma_context.io_error);
	lancelot_gpu_save_receipt(&result,0,dma==1);
	mtk_gpu_receipt.word[LMBR_PWR_WRITES]=context.writes;
	lmbr_seal(&mtk_gpu_receipt,mtk_usb_offer[2]);
	if(dma) {
		print("lancelot-gpu-dma: lifecycle elapsed=%dms; test-budget=2000ms stop-only-budget=1000ms\n",
		    lifecycle_ns/1000000u);
		struct lfd_context*f=&lancelot_dma_context;
		struct lm_private_dma*s=&f->transaction;
		print("lancelot-gpu-dma: mmu-fault-test=%d expected-fault=%d expected-va=%x; original error preserved\n",
		    (uint64_t)s->mmu_fault_test,(uint64_t)s->expected_fault,s->expected_fault_va);
		print("lancelot-gpu-dma: first-io=%x reg=%x phase=%d recovery=%d active=%d last-phase=%d\n",
		    (uint64_t)(uint32_t)f->first_io_error,(uint64_t)f->first_io_reg,(uint64_t)f->first_io_phase,
		    (uint64_t)f->recovery_attempted,(uint64_t)f->recovering,(uint64_t)f->phase);
		print("lancelot-gpu-dma: transport-error=%x address=%x; never cleared by deadline recovery\n",
		    (uint64_t)(uint32_t)context.transport_error,(uint64_t)context.transport_address);
		print("lancelot-gpu-dma: job-state-snapshot=%x; CURRENT and NEXT must both be idle\n",(uint64_t)s->job_result.job_state);
		print("lancelot-gpu-dma: failed-stage=%d reg=%x expected=%x observed=%x reset=%d quarantined=%d teardown-state=%x\n",
		    (uint64_t)s->job_result.failed_stage,(uint64_t)s->job_result.mismatch_reg,s->job_result.expected,
		    s->job_result.observed,(uint64_t)s->job_result.reset_completed,(uint64_t)s->job_result.quarantined,
		    (uint64_t)result.teardown_state);
		print("lancelot-gpu-dma: fault-extra=%x/%x js-head=%x/%x js-tail=%x/%x gpu-fault-status=%x gpu-fault=%x/%x\n",
		    (uint64_t)s->job_result.fault_extra_lo,(uint64_t)s->job_result.fault_extra_hi,
		    (uint64_t)s->job_result.js_head_lo,(uint64_t)s->job_result.js_head_hi,
		    (uint64_t)s->job_result.js_tail_lo,(uint64_t)s->job_result.js_tail_hi,
		    (uint64_t)s->job_result.gpu_fault_status,(uint64_t)s->job_result.gpu_fault_lo,
		    (uint64_t)s->job_result.gpu_fault_hi);
		print("lancelot-gpu-dma: error=%x attempted=%d submitted=%d sequence=%d job-irq=%x job-status=%x mmu=%x gpu=%x fault-pa=%x\n",
		    (uint64_t)(uint32_t)result.error,(uint64_t)result.dma_attempted,(uint64_t)s->submitted,
		    s->job_result.sequence,(uint64_t)s->job_result.job_irq,(uint64_t)s->job_result.job_status,
		    (uint64_t)s->job_result.mmu_fault,(uint64_t)s->job_result.gpu_fault,s->job_result.fault_address);
		print("lancelot-gpu-dma: value=%x guards=%d bad-offset=%d bad-job=%d cleanup=%x safe=%d retained=%d halt=%d\n",
		    (uint64_t)s->observed_value,(uint64_t)s->guards_valid,(uint64_t)s->bad_offset,(uint64_t)s->bad_job,
		    (uint64_t)(uint32_t)s->cleanup_error,(uint64_t)result.dma_safe_to_teardown,
		    (uint64_t)f->allocated,(uint64_t)result.dma_halt_required);
		if(clear)
			print("lancelot-gpu-clear: fragment-jobs=%d colors=%x/%x guards=%d bad-offset=%x actual=%x; GPU writes RGBA8 64x64 pixels, not WRITE_VALUE\n",
			    (uint64_t)(s->clear_chain?2:1),(uint64_t)s->clear_value,(uint64_t)(s->clear_chain?s->clear_value2:s->clear_value),
			    (uint64_t)s->guards_valid,(uint64_t)s->bad_offset,(uint64_t)s->clear_actual);
		if(triangle)
			print("lancelot-gpu-triangle: jobs=3 vertex+tiler+fragment color=%x guards=%d bad-stage=%d bad-offset=%x actual=%x; real vertex shader, tiler and fragment shader, not a clear\n",
			    (uint64_t)LM_TRI_VALUE,(uint64_t)s->guards_valid,(uint64_t)s->tri_bad_stage,
			    (uint64_t)s->tri_bad_word,(uint64_t)s->tri_actual);
		print("lancelot-gpu-dma: peak-pages=%d cache-clean=%d cache-invalidate=%d reads=%d writes=%d io-error=%x release=%x; %s\n",
		    (uint64_t)f->peak_pages,(uint64_t)f->cleans,(uint64_t)f->invalidates,(uint64_t)f->gpu_reads,
		    (uint64_t)f->gpu_writes,(uint64_t)(uint32_t)f->io_error,(uint64_t)(uint32_t)f->release_error,
		    triangle?"shader triangle writes pixels; a full renderer/desktop is not proved yet":
		    clear?"fragment clear writes pixels; a full renderer is not proved yet":"WRITE_VALUE is not rendering");
	}
	if(features) {
		if(result.features_valid) {
			int quiet=screen_quiet;screen_quiet=1;
			for(unsigned i=0;i<LPG_FEATURE_WORDS;i++)
				print("lancelot-gpu-features: reg=%x value=%x\n",
				    (uint64_t)lpg_feature_offsets[i],(uint64_t)result.features.word[i]);
			screen_quiet=quiet;
		}
		print("lancelot-gpu-features: valid=%d stable-passes=%d words=%d private-dma-candidate=%d; %s\n",
		    (uint64_t)result.features_valid,(uint64_t)(result.features_valid?2:0),
		    (uint64_t)(result.features_valid?LPG_FEATURE_WORDS:0),(uint64_t)result.private_dma_candidate,
		    dma?"read-only feature sample; DMA reported separately":"no job/DMA");
	}
	print("lancelot-gpu-power: before-power SWCID=%x VGPU EN/DA/SEL/DASEL=%x/%x/%x/%x VSRAM=%x/%x/%x/%x; inherited rails are not changed\n",
	    (uint64_t)result.pmic.raw[0],(uint64_t)result.pmic.raw[1],(uint64_t)result.pmic.raw[2],
	    (uint64_t)result.pmic.raw[3],(uint64_t)result.pmic.raw[4],(uint64_t)result.pmic.raw[5],
	    (uint64_t)result.pmic.raw[6],(uint64_t)result.pmic.raw[7],(uint64_t)result.pmic.raw[8]);
	print("lancelot-gpu-power: error=%x GPU_ID=%x STATUS=%x source=%dHz started=%d off-restored=%d teardown=%x writes=%d; %s\n",
	    (uint64_t)(uint32_t)result.error,(uint64_t)result.gpu_id,(uint64_t)result.gpu_status,(uint64_t)result.source_hz,
	    (uint64_t)result.started,(uint64_t)result.domains_restored,(uint64_t)(uint32_t)result.teardown_error,(uint64_t)context.writes,
	    dma?"private GPU command/DMA results reported separately":"no GPU command/DMA");
	print("lancelot-gpu-power: firmware-only diagnostic; no XNU WACS2 owner or render service published\n");
	if(dma && !lpg_dma_handoff_safe(&result) && lpg_dma_readonly_resume_safe(&result))
		print("lancelot-gpu-dma: full stage FAILED; DMA/VM resources released, resume XNU read-only only; no GPU service\n");
	lancelot_gpu_print_receipt();
	if(dma) {
		print("lancelot-gpu-dma: deferred trace events=%d dropped=%d; outside lifecycle deadline\n",
		    (uint64_t)lancelot_dma_context.trace_count,(uint64_t)lancelot_dma_context.trace_dropped);
		int quiet=screen_quiet;screen_quiet=1;
		lfd_flush_trace(&lancelot_dma_context);
		screen_quiet=quiet;
	}
	if(features) {
		/* Feature evidence is produced after the initial capture window. Give
		 * the real firmware USB reader time to drain it before XNU changes
		 * protocol/cursor domains. TPL is already restored; no extra GPU access. */
		print("lancelot-gpu-features: post-probe LLOG1 capture window 15 s; no further GPU access\n");
		BS->stall(15000000);
	}
	return !dma || lpg_dma_handoff_safe(&result) || lpg_dma_readonly_resume_safe(&result);
}

static void
lancelot_observe_pmic(const char *cmdline)
{
	int mode = lpm_requested(cmdline);
	if (!mode) return;
	if (mode != 1) { print("lancelot-pmic: invalid/duplicate probe mode; no protocol/MMIO access\n"); return; }
	const efi_guid guid = EFI_MTK_PWRAP_CHECKED_GUID;
	efi_mtk_pwrap_checked *io = NULL;
	efi_status status = BS->locate_protocol(&guid, NULL, (void **)&io);
	if (EFI_ERROR(status) || !io || !io->read) {
		print("lancelot-pmic: bounded Mu PWRAP contract absent; no PMIC access\n");
		return;
	}
	struct lpm_snapshot s;
	status = lpm_capture(io, &s);
	if (status) {
		print("lancelot-pmic: UEFI snapshot failed=%x; no partial result, no retry or GPU power\n", status);
		return;
	}
	print("lancelot-pmic: UEFI real SWCID=%x confirmed before/after 8 rail reads; no PMIC write\n", (uint64_t)s.raw[0]);
	for (unsigned i = 1; i < 9; i++)
		print("lancelot-pmic: rail raw addr=%x value=%x; status/selector, NOT measured voltage\n",
		    (uint64_t)lpm_read_addresses[i], (uint64_t)s.raw[i]);
	print("lancelot-pmic: before-handoff observation only; XNU/secure-world ownership still unproven, no GPU power/DMA\n");
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
	if (valid) lancelot_observe_pmic(pi.cmdline);
	if (valid && !lancelot_gpu_power_probe(pi.cmdline)) {
		print("lancelot-gpu-dma: unsafe or unprepared DMA state; keeping backing and firmware, kernel handoff refused\n");
		for (;;) BS->stall(100000);
	}
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
