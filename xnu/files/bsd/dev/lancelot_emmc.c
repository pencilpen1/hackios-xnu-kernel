/* Read-only MT6768 eMMC access to /hackios/20H392/ios-containers.img on Ubuntu.
 * Firmware hands over partition geometry and a superblock digest, not callbacks.
 * CMD17 PIO reads execute on the XNU CPU, serialized by a mutex. No media writes,
 * card reconfiguration, DMA or secondary firmware CPU are supported.
 */
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
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
static uint8_t sector_buffer[512];
static int block_major = -1;
static uint64_t counter_frequency, sector_reads, io_errors, logical_reads, logical_bytes;
static uint64_t pio_us, file_read_us, lock_wait_us, initial_cfg, bus_width;

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
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lancelot_mmc_bus_width, CTLFLAG_RD | CTLFLAG_LOCKED, &bus_width, "inherited data bus width in bits, zero if reserved");

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

static uint32_t
mmc_read_register(__unused void *ctx, uint32_t offset)
{
	return *(volatile uint32_t *)(registers + offset);
}
static void
mmc_write_register(__unused void *ctx, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(registers + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}
static uint64_t
mmc_ticks(__unused void *ctx)
{
	return __builtin_arm_rsr64("CNTVCT_EL0");
}
static int
partition_read(__unused void *ctx, uint64_t offset, size_t size, void *out)
{
	uint8_t *p = out;
	if (failed || offset > offer.partition_bytes || size > offer.partition_bytes - offset) return 0;
	while (size) {
		uint64_t absolute = offer.partition_offset + offset;
		size_t within = (size_t)(absolute & 511), n = 512 - within;
		if (n > size) n = size;
		/* Avoid scheduler/IRQ delays while a sector fills the small hardware FIFO. */
		boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
		uint64_t start = mmc_ticks(NULL);
		int status = lancelot_msdc_sector(&controller, (uint32_t)(absolute >> 9), sector_buffer);
		uint64_t finish = mmc_ticks(NULL);
		ml_set_interrupts_enabled(interrupts);
		pio_us += elapsed_us(start, finish);
		if (status != LM_OK) {
			io_errors++;
			failed = true;
			printf("lancelot-emmc: read failed LBA=%llu status=%d stage=%u int=%x response=%x\n",
			    absolute >> 9, status, controller.last_stage, controller.last_int, controller.last_response);
			return 0;
		}
		sector_reads++;
		memcpy(p, sector_buffer + within, n);
		p += n; offset += n; size -= n;
	}
	return 1;
}

static int
image_read(uint64_t offset, size_t size, void *out)
{
	uint64_t start = mmc_ticks(NULL);
	lck_mtx_lock(&lancelot_mmc_lock);
	uint64_t acquired = mmc_ticks(NULL);
	lock_wait_us += elapsed_us(start, acquired);
	logical_reads++;
	int ok = ready && !failed && ext4_read_file(&filesystem, &ios, offset, size, out);
	file_read_us += elapsed_us(acquired, mmc_ticks(NULL));
	if (ok) logical_bytes += size;
	lck_mtx_unlock(&lancelot_mmc_lock);
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
	if (size != sizeof(offer)) return;
	memcpy(&offer, property, sizeof(offer));
	if (offer.magic != LANCELOT_STORAGE_MAGIC || offer.version != 1 || offer.size != sizeof(offer) ||
	    offer.mmio != 0x11230000 || offer.partition_bytes != LANCELOT_UBUNTU_BYTES ||
	    !offer.partition_offset || (offer.partition_offset & 511) ||
	    offer.partition_offset >= (1ULL << 41) || offer.partition_bytes > (1ULL << 41) - offer.partition_offset) return;
	uint64_t frequency = __builtin_arm_rsr64("CNTFRQ_EL0");
	if (!frequency || frequency > 1000000000) return;
	counter_frequency = frequency;
	registers = ml_io_map((vm_offset_t)offer.mmio, 4096);
	if (!registers) return;
	controller.ctx = NULL; controller.read = mmc_read_register; controller.write = mmc_write_register;
	controller.ticks = mmc_ticks; controller.timeout = frequency / 2; /* 500 ms maximum per phase */
	initial_cfg = mmc_read_register(NULL, LM_CFG);
	uint32_t width = (mmc_read_register(NULL, 0x30) >> 16) & 3;
	bus_width = width == 0 ? 1 : width == 1 ? 4 : width == 2 ? 8 : 0;
	printf("lancelot-emmc: inherited cfg=%llx width=%llu clock-mode=%llu divisor=%llu, Ubuntu byte %llu; CMD17 read-only\n",
	    initial_cfg, bus_width, (initial_cfg >> 20) & 3, (initial_cfg >> 8) & 0xfff, offer.partition_offset);
	uint8_t super[1024], digest[32];
	static uint8_t header[4096];
	if (!partition_read(NULL, 1024, sizeof(super), super)) return;
	sha256(super, sizeof(super), digest);
	if (memcmp(digest, offer.superblock_sha256, sizeof(digest))) {
		printf("lancelot-emmc: native superblock does not match firmware digest\n");
		failed = true; return;
	}
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
}
