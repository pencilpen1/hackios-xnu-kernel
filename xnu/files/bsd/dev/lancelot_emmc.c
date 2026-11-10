/* Read-only MT6768 eMMC system access to /hackios/20H392/ios-containers.img on Ubuntu.
 * Firmware hands over partition geometry and a superblock digest, not callbacks.
 * Counted CMD18 PIO reads execute on the XNU CPU, serialized by a mutex. SDR
 * width/clock negotiation is verified before use. Persistent data writes are
 * separately opt-in and restricted by lancelot_data_disk.inc; no other CPU.
 */
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <arm64/proc_reg.h>
#include <miscfs/devfs/devfs.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/redwood_input.h>
#include "lancelot_storage.h"
#include "lancelot_msdc.h"
#include "lancelot_mmc_mode.h"
#include "lancelot_mmc_observation.h"
#define XNULOADER_NO_LIBC
#include "lancelot_ext4.h"
#include "lancelot_sha256.h"
#undef XNULOADER_NO_LIBC

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static LCK_GRP_DECLARE(lancelot_mmc_group, "lancelot-emmc");
static LCK_MTX_DECLARE(lancelot_mmc_lock, &lancelot_mmc_group);
static struct lancelot_storage offer;
static struct lancelot_msdc controller;
static struct ext4_fs filesystem;
static struct ext4_inode ios;
static vm_offset_t registers;
static bool ready, failed;
static uint32_t device_block_size = 512;
static uint8_t sector_buffer[512] __attribute__((aligned(64)));
static int block_major = -1;
static uint64_t counter_frequency, sector_reads, io_errors, logical_reads, logical_bytes;
static uint64_t pio_us, file_read_us, lock_wait_us, initial_cfg, bus_width;
static uint64_t max_read_blocks = 1, active_clock_hz, mode_fallbacks;
static uint64_t mmc_mask_began, mmc_mask_total, mmc_mask_max, mmc_mask_count;
static uint64_t mmc_idle_wait_count;
static uint32_t image_read_diagnostics;
static struct lmd_observer mmc_observer;
/* Immutable after the first transport failure; publish without console I/O
 * inside the controller mutex. The outer request reports it after unlocking. */
static struct {
    uint64_t lba;
    uint32_t count, command, stage, interrupt, response;
    int status, recovery;
} mmc_native_failure;
static _Atomic uint32_t mmc_native_failure_ready, mmc_native_failure_reported;
extern uint32_t lancelot_mmc_mutex_state(const void *);

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_sectors, CTLFLAG_RD | CTLFLAG_LOCKED, &sector_reads, "successful 512-byte native reads, including ext4 metadata");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &io_errors, "native transport errors");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &logical_reads, "APFS image read requests");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &logical_bytes, "successful requested image bytes");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_pio_us, CTLFLAG_RD | CTLFLAG_LOCKED, &pio_us, "microseconds spent in native PIO transactions");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_read_us, CTLFLAG_RD | CTLFLAG_LOCKED, &file_read_us, "microseconds reading image data including extent mapping");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_lock_us, CTLFLAG_RD | CTLFLAG_LOCKED, &lock_wait_us, "microseconds waiting for storage serialization");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_meta_hits, CTLFLAG_RD | CTLFLAG_LOCKED, &filesystem.metadata_hits, "extent metadata cache hits");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_meta_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &filesystem.metadata_reads, "extent metadata reads from storage");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_cfg, CTLFLAG_RD | CTLFLAG_LOCKED, &initial_cfg, "inherited MSDC_CFG register");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_bus_width, CTLFLAG_RD | CTLFLAG_LOCKED, &bus_width, "verified active data bus width in bits");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_clock_hz, CTLFLAG_RD | CTLFLAG_LOCKED, &active_clock_hz, "active SDR clock, zero if source/mode unknown");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_batch_blocks, CTLFLAG_RD | CTLFLAG_LOCKED, &max_read_blocks, "maximum sectors per counted read");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_commands, CTLFLAG_RD | CTLFLAG_LOCKED, &controller.commands_sent, "card commands including setup and recovery");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_multi_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &controller.multi_reads, "counted CMD18 transfers started");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_recoveries, CTLFLAG_RD | CTLFLAG_LOCKED, &controller.recoveries, "bounded read recovery attempts");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_mode_fallbacks, CTLFLAG_RD | CTLFLAG_LOCKED, &mode_fallbacks, "verified width/clock/multiblock fallbacks");

bool
lancelot_emmc_dev(dev_t dev)
{
	return ready && major(dev) == block_major && minor(dev) == 0;
}
static uint64_t elapsed_us(uint64_t start, uint64_t end)
{
	uint64_t ticks = end - start;
	return (ticks / counter_frequency) * 1000000 + (ticks % counter_frequency) * 1000000 / counter_frequency;
}
static void mmc_observe_begin(unsigned operation,uint64_t offset,uint64_t bytes) {
    lmd_begin(&mmc_observer,lancelot_mmc_mutex_state(&lancelot_mmc_lock)&0x0fffffffu,
        operation,offset,bytes,__builtin_arm_rsr64("CNTVCT_EL0"));
}
static void mmc_observe_end(void){lmd_end(&mmc_observer);}
static void mmc_observe_transfer(uint64_t bytes){os_atomic_add(&mmc_observer.transport_bytes,bytes,relaxed);}
void lancelot_mmc_dump_active_io(void);
size_t lancelot_mmc_format_io(char *,size_t);
size_t lancelot_mmc_format_io(char *out,size_t capacity) {
    struct lmd_observation v=lmd_snapshot(&mmc_observer);
    uint32_t owner=lancelot_mmc_mutex_state(&lancelot_mmc_lock)&0x0fffffffu;
    uint64_t now=__builtin_arm_rsr64("CNTVCT_EL0"),hold=0;
    if(v.operation&&v.began&&now>=v.began&&counter_frequency)hold=elapsed_us(v.began,now);
    uint32_t bounded_hold=hold>UINT32_MAX?UINT32_MAX:(uint32_t)hold;
    int consistent=v.consistent&&(!v.operation||owner==v.owner);
    /* Nine hexadecimal fields, <=60 bytes: coherence, owner, operation,
     * offset, request length, native transfer bytes, hold us, last command, phase.
     * Fastboot responses include a four-byte status within a 64-byte limit. */
    int n=snprintf(out,capacity,"%x,%x,%llx,%llx,%llx,%llx,%x,%llx,%llx",
        consistent,owner,v.operation,v.offset,v.bytes,v.transport_bytes,bounded_hold,v.command,v.phase);
    return n>0&&(size_t)n<capacity&&n<=60?(size_t)n:0;
}
void lancelot_mmc_dump_active_io(void) {
    struct lmd_observation v=lmd_snapshot(&mmc_observer);
    uint64_t now=__builtin_arm_rsr64("CNTVCT_EL0"),hold=0;
    if(v.operation&&v.began&&now>=v.began&&counter_frequency)hold=elapsed_us(v.began,now);
    printf("lancelot-mmc-active-io: consistent=%d owner_compact_tid=%llu operation=%llu "
        "offset=%llu request_bytes=%llu native_bytes=%llu hold_us=%llu raw_command=%#llx epoch=%llu one_shot=1\n",
        v.consistent,v.owner,v.operation,v.offset,v.bytes,v.transport_bytes,hold,v.command,v.epoch);
}

static uint32_t
mmc_read_register(__unused void *ctx, uint32_t offset)
{
	return *(volatile uint32_t *)(registers + offset);
}
static void
mmc_write_register(__unused void *ctx, uint32_t offset, uint32_t value)
{
	if(offset==LM_CMD)os_atomic_store(&mmc_observer.command,value,relaxed);
	*(volatile uint32_t *)(registers + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}
static uint64_t
mmc_ticks(__unused void *ctx)
{
	return __builtin_arm_rsr64("CNTVCT_EL0");
}
static uint64_t mmc_enter(__unused void *ctx) {
    os_atomic_store(&mmc_observer.phase,LMD_PHASE_MASK_IRQ,relaxed);
    uint64_t state=__builtin_arm_rsr64("DAIF");
    __builtin_arm_wsr("DAIFSet",DAIFSC_IRQF|DAIFSC_FIQF);
    mmc_mask_began=__builtin_arm_rsr64("CNTVCT_EL0");
    os_atomic_store(&mmc_observer.phase,LMD_PHASE_PIO,relaxed);
    return state;
}
static void mmc_leave(__unused void *ctx, uint64_t state) {
    os_atomic_store(&mmc_observer.phase,LMD_PHASE_RESTORE_IRQ,relaxed);
    /* ml_set_interrupts_enabled(TRUE) can run ast_taken_kernel() before
     * enabling IRQs. Restore the full caller state without entering that
     * scheduler path while the native controller mutex is still held. */
    uint64_t elapsed=__builtin_arm_rsr64("CNTVCT_EL0")-mmc_mask_began;
    __builtin_arm_wsr64("DAIF",state);
    os_atomic_add(&mmc_mask_total,elapsed,relaxed);
    os_atomic_inc(&mmc_mask_count,relaxed);
    if(elapsed>os_atomic_load(&mmc_mask_max,relaxed))os_atomic_store(&mmc_mask_max,elapsed,relaxed);
    os_atomic_store(&mmc_observer.phase,LMD_PHASE_IRQ_RESTORED,relaxed);
}
static void mmc_idle_wait(__unused void *ctx) {
    /* Called with the controller mutex held but no live FIFO payload. The
     * remaining width-negotiation/early callers may still mask interrupts. */
    if(ml_get_interrupts_enabled()) {
        os_atomic_inc(&mmc_idle_wait_count,relaxed);
        delay_for_interval(100,NSEC_PER_USEC);
    }
}
void lancelot_mmc_report_latency(void);
void lancelot_mmc_report_latency(void) {
    if(!counter_frequency)return;
    printf("lancelot-mmc-sched: masks=%llu total_us=%llu max_us=%llu card_yields=%llu batch=%llu\n",
        os_atomic_load(&mmc_mask_count,relaxed),elapsed_us(0,os_atomic_load(&mmc_mask_total,relaxed)),
        elapsed_us(0,os_atomic_load(&mmc_mask_max,relaxed)),
        os_atomic_load(&mmc_idle_wait_count,relaxed),max_read_blocks);
}
static void mmc_report_native_failure(void) {
    if(!os_atomic_load(&mmc_native_failure_ready,acquire) ||
       !os_atomic_cmpxchg(&mmc_native_failure_reported,0,1,acq_rel))return;
    printf("lancelot-emmc: read failed LBA=%llu count=%u status=%d cmd=%u stage=%u int=%x response=%x recovery=%d\n",
        mmc_native_failure.lba,mmc_native_failure.count,mmc_native_failure.status,
        mmc_native_failure.command,mmc_native_failure.stage,mmc_native_failure.interrupt,
        mmc_native_failure.response,mmc_native_failure.recovery);
}
static void
mmc_fifo_words(__unused void *ctx, uint8_t *out, unsigned words)
{
	volatile uint32_t *fifo = (volatile uint32_t *)(registers + LM_RX);
	if (((uintptr_t)out & 3) == 0) {
		/* One indirect callback per FIFO burst; aligned word stores on ARM64 LE. */
		for (unsigned i = 0; i < words; i++) {
			uint32_t value = *fifo;
			__builtin_memcpy(__builtin_assume_aligned(out + i * 4, 4), &value, 4);
		}
	} else {
		for (unsigned i = 0; i < words; i++) {
			uint32_t value = *fifo;
			for (unsigned j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(value >> (j * 8));
		}
	}
}
static void
mmc_fifo_write_words(__unused void *ctx,const uint8_t *in,unsigned words)
{
	volatile uint32_t *fifo=(volatile uint32_t *)(registers+LM_TX);
	for(unsigned i=0;i<words;i++) {
		uint32_t value;
		__builtin_memcpy(&value,in+i*4,sizeof(value));
		*fifo=value;
	}
	/* Device writes within one bounded FIFO burst are ordered together. The
	 * control-register adapter retains its barrier for every control write. */
	__asm__ volatile ("dsb sy" ::: "memory");
}
/* Probe reads do not poison the device on a rejected mode: negotiation must
 * restore and verify a known configuration before it is allowed to fall back. */
static int
mmc_verify_super(void *ctx)
{
	uint8_t super[1024], digest[32];
	uint32_t count = ctx ? *(const uint32_t *)ctx : 1;
	for (unsigned i = 0; i < 2; i += count) {
		uint32_t sector = (uint32_t)((offer.partition_offset + 1024) / 512) + i;
		if (lancelot_msdc_read(&controller, sector, count, super + i * 512, sizeof(super) - i * 512) != LM_OK) return 0;
	}
	sha256(super, sizeof(super), digest);
	return memcmp(digest, offer.superblock_sha256, sizeof(digest)) == 0;
}
static int
partition_read(__unused void *ctx, uint64_t offset, size_t size, void *out)
{
	uint8_t *p = out;
	if (failed || offset > offer.partition_bytes || size > offer.partition_bytes - offset) return 0;
	while (size) {
		uint64_t absolute = offer.partition_offset + offset;
		size_t within = (size_t)(absolute & 511), n;
		uint32_t count = 1;
		uint8_t *target = sector_buffer;
		if (!within && size >= 512) {
			count = (uint32_t)((size / 512 > max_read_blocks) ? max_read_blocks : size / 512);
			n = (size_t)count * 512;
			target = p;
		} else {
			n = 512 - within;
			if (n > size) n = size;
		}
		uint64_t start = mmc_ticks(NULL);
		int status = lancelot_msdc_read(&controller, (uint32_t)(absolute >> 9), count, target, (size_t)count * 512);
		uint64_t finish = mmc_ticks(NULL);
		pio_us += elapsed_us(start, finish);
		if (status != LM_OK) {
			io_errors++;
            mmc_native_failure.lba=absolute>>9;mmc_native_failure.count=count;
            mmc_native_failure.status=status;mmc_native_failure.command=controller.last_command;
            mmc_native_failure.stage=controller.last_stage;mmc_native_failure.interrupt=controller.last_int;
            mmc_native_failure.response=controller.last_response;mmc_native_failure.recovery=controller.recovery_status;
            os_atomic_store(&mmc_native_failure_ready,1,release);
            failed = true;
			return 0;
		}
		sector_reads += count;
		mmc_observe_transfer((uint64_t)count*512);
		if (target != p) memcpy(p, sector_buffer + within, n);
		p += n; offset += n; size -= n;
	}
	return 1;
}

static int
image_read(uint64_t offset, size_t size, void *out)
{
    if(!size)return 0;
    /* The device advertises 128 KiB, and APFS/native character reads honor it.
     * Allocate resident delivery storage before taking the controller mutex:
     * PIO runs with IRQs masked and must not fault on the caller's mapping. */
    if(!out || size>128*1024)return EINVAL;
    uint8_t *staging=kalloc_data(size,Z_WAITOK);
    if(!staging)return ENOMEM;
	uint64_t start = mmc_ticks(NULL);
	lck_mtx_lock(&lancelot_mmc_lock);
	uint64_t acquired = mmc_ticks(NULL);
	lock_wait_us += elapsed_us(start, acquired);
	logical_reads++;
	mmc_observe_begin(LMD_IO_SYSTEM_READ,offset,size);
	int ok = ready && !failed && ext4_read_file(&filesystem, &ios, offset, size, staging);
    os_atomic_store(&mmc_observer.phase,LMD_PHASE_FILE_DONE,relaxed);
	file_read_us += elapsed_us(acquired, mmc_ticks(NULL));
	if (ok) logical_bytes += size;
	bool report = !ok && os_atomic_inc_orig(&image_read_diagnostics, relaxed) < 8;
	bool native_failed = failed;
	uint32_t command = controller.last_command, stage = controller.last_stage;
	uint32_t irq = controller.last_int, response = controller.last_response;
	mmc_observe_end();
	lck_mtx_unlock(&lancelot_mmc_lock);
    /* Delivery may touch a pageable caller address. It is outside both the
     * PIO IRQ mask and the mutex needed by another fault's storage request. */
    if(ok)memcpy(out,staging,size);
    kfree_data(staging,size);
    mmc_report_native_failure();
	if (report) {
		printf("lancelot-emmc-image-error: offset=%llu bytes=%llu file_bytes=%llu ready=%d "
		    "native_failed=%d command=%u stage=%u irq=%#x response=%#x\n",
		    offset, (uint64_t)size, ios.size, ready, native_failed,
		    command, stage, irq, response);
	}
	return ok ? 0 : EIO;
}
static int
mmc_open(dev_t dev, int flags, __unused int type, __unused proc_t p)
{
	if (!ready || failed || minor(dev)) return ENXIO;
	return flags & FWRITE ? EROFS : 0;
}
static int
mmc_close(__unused dev_t dev, __unused int flags, __unused int type, __unused proc_t p)
{
	return 0;
}
static void
mmc_strategy(buf_t bp)
{
	uint32_t count = buf_count(bp);
	uint64_t offset = 0, size = count;
	int error = 0;
	buf_setresid(bp, count);
	if (!ready || failed || minor(buf_device(bp))) error = ENXIO;
	else if (!(buf_flags(bp) & B_READ)) error = EROFS;
	else if (buf_blkno(bp) < 0 || (uint64_t)buf_blkno(bp) > UINT64_MAX / device_block_size) error = EINVAL;
	else {
		offset = (uint64_t)buf_blkno(bp) * device_block_size;
		if (offset > ios.size) error = EINVAL;
		else {
			if (size > ios.size - offset) size = ios.size - offset;
			if (size) error = image_read(offset, (size_t)size, (void *)buf_dataptr(bp));
			if (!error) buf_setresid(bp, (uint32_t)(count - size));
		}
	}
	if (error) buf_seterror(bp, error);
	buf_biodone(bp);
}
static int
mmc_ioctl(dev_t dev, u_long command, caddr_t data, __unused int flags, __unused proc_t p)
{
	if (!ready || failed || minor(dev)) return ENXIO;
	switch (command) {
	case DKIOCGETBLOCKSIZE: *(uint32_t *)data = device_block_size; return 0;
	case DKIOCSETBLOCKSIZE:
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > 4096 ||
		    (*(uint32_t *)data & (*(uint32_t *)data - 1))) return EINVAL;
		device_block_size = *(uint32_t *)data; return 0;
	case DKIOCGETPHYSICALBLOCKSIZE: *(uint32_t *)data = 512; return 0;
	case DKIOCGETBLOCKCOUNT: *(uint64_t *)data = ios.size / device_block_size; return 0;
	case DKIOCISWRITABLE: *(uint32_t *)data = 0; return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD: *(uint64_t *)data = (128 * 1024) / device_block_size; return 0;
	default: return ENOTTY;
	}
}
static int mmc_size(__unused dev_t dev) { return ready ? (int)device_block_size : -1; }
static int
mmc_cread(dev_t dev, struct uio *uio, __unused int flags)
{
	if (!ready || failed || minor(dev)) return ENXIO;
	if (uio_offset(uio) < 0) return EINVAL;
	const size_t chunk = 16384;
	uint8_t *data = kalloc_data(chunk, Z_WAITOK);
	if (!data) return ENOMEM;
	int error = 0;
	while (uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < ios.size && !error) {
		uint64_t offset = (uint64_t)uio_offset(uio), n = (uint64_t)uio_resid(uio);
		if (n > chunk) n = chunk;
		if (n > ios.size - offset) n = ios.size - offset;
		error = image_read(offset, (size_t)n, data);
		if (!error) error = uiomove((caddr_t)data, (int)n, uio);
	}
	kfree_data(data, chunk);
	return error;
}
static int
mmc_cwrite(__unused dev_t dev, __unused struct uio *uio, __unused int flags)
{
	return EROFS;
}
/* The separate data device is enabled only by its explicit boot argument. */
#include "lancelot_data_disk.inc"
static const struct bdevsw mmc_bdev = {
	.d_open = mmc_open, .d_close = mmc_close, .d_strategy = mmc_strategy,
	.d_ioctl = mmc_ioctl, .d_dump = eno_dump, .d_psize = mmc_size, .d_type = D_DISK,
};
static const struct cdevsw mmc_cdev = {
	.d_open = mmc_open, .d_close = mmc_close, .d_read = mmc_cread, .d_write = mmc_cwrite,
	.d_ioctl = mmc_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
	.d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat,
	.d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = D_DISK,
};

void
lancelot_emmc_init(void)
{
	DTEntry chosen;
	const void *property;
	unsigned int size;
	if (ready || registers || SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "lancelot-emmc", &property, &size) != kSuccess) return;
	if (size != LANCELOT_STORAGE_V1_SIZE && size != sizeof(offer)) return;
	memset(&offer, 0, sizeof(offer));
	memcpy(&offer, property, size);
	if (offer.magic != LANCELOT_STORAGE_MAGIC || offer.size != size ||
	    !((offer.version == 1 && size == LANCELOT_STORAGE_V1_SIZE) || (offer.version == 2 && size == sizeof(offer))) ||
	    offer.mmio != 0x11230000 || offer.partition_bytes != LANCELOT_UBUNTU_BYTES ||
	    !offer.partition_offset || (offer.partition_offset & 511) ||
	    offer.partition_offset >= (1ULL << 41) || offer.partition_bytes > (1ULL << 41) - offer.partition_offset) return;
	if (offer.version == 1) { offer.rca = 1; offer.max_bus_width = 1; offer.hs_timing = 1; } /* known legacy firmware contract */
	if (!offer.rca || offer.rca > 65535 || offer.source_clock_hz > 1000000000 ||
	    offer.hs_timing > 3 || offer.reserved || (offer.max_bus_width != 1 && offer.max_bus_width != 4 && offer.max_bus_width != 8)) return;
	uint64_t frequency = __builtin_arm_rsr64("CNTFRQ_EL0");
	if (!frequency || frequency > 1000000000) return;
	counter_frequency = frequency;
	registers = ml_io_map((vm_offset_t)offer.mmio, 4096);
	if (!registers) return;
	controller.ctx = NULL; controller.read = mmc_read_register; controller.write = mmc_write_register;
	controller.ticks = mmc_ticks; controller.timeout = frequency / 2; /* 500 ms maximum per phase */
	controller.read_fifo = mmc_fifo_words; controller.write_fifo = mmc_fifo_write_words;
	controller.critical_enter = mmc_enter; controller.critical_exit = mmc_leave;
	controller.idle_wait = mmc_idle_wait;
	controller.rca = offer.rca;
	initial_cfg = mmc_read_register(NULL, LM_CFG);
	uint32_t width = (mmc_read_register(NULL, LM_BUS_CFG) >> 16) & 3;
	bus_width = width == 0 ? 1 : width == 1 ? 4 : width == 2 ? 8 : 0;
	printf("lancelot-emmc: inherited cfg=%llx width=%llu clock-mode=%llu divisor=%llu, Ubuntu byte %llu; CMD17 read-only\n",
	    initial_cfg, bus_width, (initial_cfg >> 20) & 3, (initial_cfg >> 8) & 0xfff, offer.partition_offset);
	static uint8_t header[4096];
	if (!mmc_verify_super(NULL)) {
		printf("lancelot-emmc: native superblock does not match firmware digest\n");
		failed = true; return;
	}
	/* At 48 MHz SDR a 64 KiB masked PIO transfer already exceeds 1 ms
	 * before software overhead. Split at 8 KiB to bound timer/IPI latency. */
	uint32_t tune = 1, wanted_width = offer.max_bus_width, wanted_blocks = 16;
	(void)PE_parse_boot_argn("lancelot_mmc_tune", &tune, sizeof(tune));
	(void)PE_parse_boot_argn("lancelot_mmc_width", &wanted_width, sizeof(wanted_width));
	(void)PE_parse_boot_argn("lancelot_mmc_blocks", &wanted_blocks, sizeof(wanted_blocks));
	if ((wanted_width != 1 && wanted_width != 4 && wanted_width != 8) || wanted_width > offer.max_bus_width ||
	    !wanted_blocks || wanted_blocks > LM_MAX_BLOCKS) { printf("lancelot-emmc: invalid transfer settings\n"); return; }
	active_clock_hz = lm_clock_hz((uint32_t)initial_cfg, offer.source_clock_hz);
	if (tune) {
		struct lancelot_mmc_mode mode;
		int status = lancelot_msdc_negotiate(&controller, wanted_width, offer.source_clock_hz, offer.hs_timing, mmc_verify_super, NULL, &mode);
		if (status != LM_OK) { printf("lancelot-emmc: could not verify negotiated/rollback mode (%d)\n", status); failed = true; return; }
		bus_width = mode.width; active_clock_hz = mode.clock_hz; mode_fallbacks = mode.fallbacks;
	}
	if (wanted_blocks > 1) {
		uint32_t probe_blocks = 2;
		if (mmc_verify_super(&probe_blocks)) max_read_blocks = wanted_blocks;
		else {
			mode_fallbacks++;
			if (controller.recovery_status != LM_OK || !mmc_verify_super(NULL)) {
				printf("lancelot-emmc: counted-read probe and recovery failed\n"); failed = true; return;
			}
			printf("lancelot-emmc: counted reads rejected; verified CMD17 fallback\n");
		}
	}
	printf("lancelot-emmc: verified width=%llu clock=%llu Hz batch=%llu sectors fallbacks=%llu\n",
	    bus_width, active_clock_hz, max_read_blocks, mode_fallbacks);
	if (!ext4_open(&filesystem, partition_read, NULL, offer.partition_bytes) ||
	    !ext4_lookup(&filesystem, "/hackios/20H392/ios-containers.img", &ios) ||
	    ios.size != LANCELOT_IOS_BYTES || !ext4_read_file(&filesystem, &ios, 0, sizeof(header), header) ||
	    memcmp(header + 32, "NXSB", 4)) {
		printf("lancelot-emmc: cannot open the deployed iOS APFS container file\n");
		failed = true; return;
	}
	int bmajor = bdevsw_add(-1, &mmc_bdev);
	int cmajor = bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &mmc_cdev, bmajor);
	if (cmajor < 0) { printf("lancelot-emmc: device number allocation failed\n"); return; }
	if (!devfs_make_node(makedev(bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0440, "lancelot_ios") ||
	    !devfs_make_node(makedev(cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0440, "rlancelot_ios")) {
		printf("lancelot-emmc: device node allocation failed\n"); return;
	}
	block_major = bmajor;
	ready = true;
	printf("lancelot-emmc: /dev/lancelot_ios ready, %llu bytes; native superblock SHA256 matched\n", ios.size);
    lancelot_data_init();
}
