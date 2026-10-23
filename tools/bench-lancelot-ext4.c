/* Count real mapper read requests against a synthetic valid large-file extent
 * tree. This measures I/O amplification, NOT physical eMMC throughput. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef EXT4_HEADER
#define EXT4_HEADER "../xnu/files/bsd/dev/lancelot_ext4.h"
#endif
#include EXT4_HEADER

struct model { uint8_t leaf[4096]; uint64_t calls, bytes, sectors, nodes; };
static void put16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void put32(uint8_t *p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }
static uint8_t value(uint64_t offset) { return (uint8_t)(offset ^ (offset >> 9) ^ (offset >> 17) ^ (offset >> 25)); }
static int read_model(void *ctx, uint64_t offset, size_t size, void *out)
{
	struct model *m = ctx;
	m->calls++; m->bytes += size; m->sectors += ((offset & 511) + size + 511) / 512;
	if (offset == 10 * 4096 && size == 4096) {
		m->nodes++; memcpy(out, m->leaf, size); return 1;
	}
	uint8_t *p = out;
	for (size_t i = 0; i < size; i++) {
		uint64_t block = (offset + i) / 4096;
		assert(block >= 1024);
		uint64_t relative = block - 1024, extent = relative / 32769, in = relative % 32769;
		assert(extent < 64 && in < 32768); /* coalesced reads must not cross physical gaps */
		p[i] = value((extent * 32768 + in) * 4096 + ((offset + i) & 4095));
	}
	return 1;
}
int main(void)
{
	static struct ext4_fs fs;
	struct ext4_inode inode = {0};
	struct model model = {0};
	uint8_t data[128 * 1024];
	fs.read = read_model; fs.ctx = &model; fs.block_size = 4096;
	fs.blocks = 8388608; fs.bytes = fs.blocks * 4096;
	inode.size = 8388608000ULL; inode.mode = 0x8000; inode.flags = 0x80000;
	put16(inode.extents, 0xf30a); put16(inode.extents + 2, 1);
	put16(inode.extents + 4, 4); put16(inode.extents + 6, 1);
	put32(inode.extents + 16, 10);
	put16(model.leaf, 0xf30a); put16(model.leaf + 2, 64); put16(model.leaf + 4, 340);
	for (unsigned i = 0; i < 64; i++) {
		uint8_t *entry = model.leaf + 12 + i * 12;
		put32(entry, i * 32768); put16(entry + 4, 32768);
		put32(entry + 8, 1024 + i * 32769);
	}
	const uint64_t offsets[] = {1048576, 1048576, 128 * 1024 * 1024 - 2048};
	const char *names[] = {"cold-128KiB", "warm-128KiB", "extent-boundary-128KiB"};
	for (unsigned test = 0; test < 3; test++) {
		model.calls = model.bytes = model.sectors = model.nodes = 0;
		assert(ext4_read_file(&fs, &inode, offsets[test], sizeof(data), data));
		for (size_t i = 0; i < sizeof(data); i++) assert(data[i] == value(offsets[test] + i));
		printf("%s bytes=%llu requests=%llu metadata_reads=%llu cmd17_equivalent=%llu\n", names[test],
		    (unsigned long long)model.bytes, (unsigned long long)model.calls,
		    (unsigned long long)model.nodes, (unsigned long long)model.sectors);
	}
}
