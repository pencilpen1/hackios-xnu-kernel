/* Bounded read-only ext4 reader for bootstrap files. No allocator or write path.
 * Supports ordinary extent-based files/directories, 1/2/4 KiB blocks and 64-bit
 * block descriptors. Refuses journal recovery, meta_bg, bigalloc, inline data,
 * encryption and symlink traversal. File contents are hashed by the caller.
 */
#ifndef XNULOADER_EXT4_READ_H
#define XNULOADER_EXT4_READ_H
#include <stdint.h>
#include <stddef.h>
#ifndef XNULOADER_NO_LIBC
#include <string.h>
#endif

typedef int (*ext4_read_fn)(void *, uint64_t, size_t, void *);
struct ext4_fs {
	ext4_read_fn read;
	void *ctx;
	uint64_t bytes, blocks, descriptors;
	uint32_t block_size, inodes, inodes_per_group, blocks_per_group, first_block;
	uint16_t inode_size, desc_size;
	/* Caller-owned scratch keeps freestanding firmware off large stack frames. */
	uint8_t node[4096], directory[4096];
};
struct ext4_inode {
	uint64_t size;
	uint32_t flags;
	uint16_t mode;
	uint8_t extents[60];
};
static uint16_t e4_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t e4_u32(const uint8_t *p) { return (uint32_t)e4_u16(p) | (uint32_t)e4_u16(p + 2) << 16; }
static int
e4_read(struct ext4_fs *fs, uint64_t offset, size_t size, void *out)
{
	return offset <= fs->bytes && size <= fs->bytes - offset && fs->read(fs->ctx, offset, size, out);
}
static int
ext4_open(struct ext4_fs *fs, ext4_read_fn read, void *ctx, uint64_t bytes)
{
	uint8_t sb[1024];
	memset(fs, 0, sizeof(*fs));
	fs->read = read; fs->ctx = ctx; fs->bytes = bytes;
	if (!e4_read(fs, 1024, sizeof(sb), sb) || e4_u16(sb + 56) != 0xef53) return 0;
	uint32_t incompat = e4_u32(sb + 96), ro = e4_u32(sb + 100), shift = e4_u32(sb + 24);
	if (shift > 2 || (incompat & ~0x2c2u) || !(incompat & 0x40) || (ro & 0x200) ||
	    !(e4_u16(sb + 58) & 1) || (e4_u16(sb + 58) & 2)) return 0;
	fs->block_size = 1024u << shift;
	fs->blocks = e4_u32(sb + 4);
	if (incompat & 0x80) fs->blocks |= (uint64_t)e4_u32(sb + 336) << 32;
	if (!fs->blocks || fs->blocks > bytes / fs->block_size) return 0;
	fs->bytes = fs->blocks * fs->block_size;
	fs->first_block = e4_u32(sb + 20);
	if (fs->first_block != (fs->block_size == 1024 ? 1u : 0u)) return 0;
	fs->blocks_per_group = e4_u32(sb + 32);
	fs->inodes_per_group = e4_u32(sb + 40);
	fs->inodes = e4_u32(sb);
	fs->inode_size = e4_u32(sb + 76) ? e4_u16(sb + 88) : 128;
	fs->desc_size = (incompat & 0x80) ? e4_u16(sb + 254) : 32;
	if (!fs->blocks_per_group || fs->blocks_per_group > fs->block_size * 8 ||
	    !fs->inodes_per_group || fs->inodes_per_group > fs->block_size * 8 || fs->inodes < 2 ||
	    fs->inode_size < 128 || fs->inode_size > fs->block_size ||
	    (fs->inode_size & (fs->inode_size - 1)) ||
	    fs->desc_size < ((incompat & 0x80) ? 64 : 32) || fs->desc_size > fs->block_size ||
	    (fs->desc_size & (fs->desc_size - 1))) return 0;
	fs->descriptors = (uint64_t)(fs->first_block + 1) * fs->block_size;
	return 1;
}
static int
e4_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode)
{
	uint8_t desc[64], raw[128];
	if (!number || number > fs->inodes) return 0;
	uint64_t group = (number - 1) / fs->inodes_per_group;
	uint64_t groups = (fs->blocks - fs->first_block + fs->blocks_per_group - 1) / fs->blocks_per_group;
	if (group >= groups || !e4_read(fs, fs->descriptors + group * fs->desc_size,
	    fs->desc_size >= 64 ? 64 : 32, desc)) return 0;
	uint64_t table = e4_u32(desc + 8);
	if (fs->desc_size >= 64) table |= (uint64_t)e4_u32(desc + 40) << 32;
	uint64_t index = (number - 1) % fs->inodes_per_group;
	if (!table || table >= fs->blocks || !e4_read(fs, table * fs->block_size + index * fs->inode_size,
	    sizeof(raw), raw)) return 0;
	inode->mode = e4_u16(raw);
	inode->size = e4_u32(raw + 4) | (uint64_t)e4_u32(raw + 108) << 32;
	inode->flags = e4_u32(raw + 32);
	if (!(inode->flags & 0x80000) || (inode->flags & (0x10000000u | 0x800u)) ||
	    inode->size > fs->bytes) return 0;
	memcpy(inode->extents, raw + 40, sizeof(inode->extents));
	return 1;
}
/* Map a logical block. Holes/unwritten extents return physical block zero. */
static int
e4_map(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical)
{
	uint8_t *node = fs->node;
	size_t size = sizeof(inode->extents);
	memcpy(node, inode->extents, size);
	unsigned expected = e4_u16(node + 6);
	if (expected > 5) return 0;
	for (;;) {
		unsigned count = e4_u16(node + 2), max = e4_u16(node + 4), depth = e4_u16(node + 6);
		if (e4_u16(node) != 0xf30a || depth != expected || !max || max > (size - 12) / 12 || count > max) return 0;
		uint64_t chosen = 0, previous_end = 0;
		int have = 0;
		*physical = 0;
		for (unsigned i = 0; i < count; i++) {
			const uint8_t *entry = node + 12 + i * 12;
			uint32_t start = e4_u32(entry);
			if (i && start < previous_end) return 0;
			if (depth) {
				uint64_t child = e4_u32(entry + 4) | (uint64_t)e4_u16(entry + 8) << 32;
				if (!child || child >= fs->blocks) return 0;
				if (start <= logical) { chosen = child; have = 1; }
				previous_end = (uint64_t)start + 1;
			} else {
				uint32_t length = e4_u16(entry + 4);
				int unwritten = length > 32768;
				if (unwritten) length -= 32768;
				uint64_t block = e4_u32(entry + 8) | (uint64_t)e4_u16(entry + 6) << 32;
				if (!length || !block || block >= fs->blocks || length > fs->blocks - block ||
				    (uint64_t)start + length > (1ULL << 32)) return 0;
				previous_end = (uint64_t)start + length;
				if (logical >= start && (uint64_t)logical < previous_end && !unwritten)
					*physical = block + (logical - start);
			}
		}
		if (!depth || !have) return 1;
		if (!e4_read(fs, chosen * fs->block_size, fs->block_size, node)) return 0;
		size = fs->block_size;
		expected--;
	}
}
static int
ext4_read_file(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, size_t size, void *out)
{
	uint8_t *p = out;
	if (offset > inode->size || size > inode->size - offset) return 0;
	while (size) {
		uint64_t logical = offset / fs->block_size, physical;
		size_t within = (size_t)(offset % fs->block_size), n = fs->block_size - within;
		if (logical > UINT32_MAX || !e4_map(fs, inode, (uint32_t)logical, &physical)) return 0;
		if (n > size) n = size;
		if (physical) {
			if (!e4_read(fs, physical * fs->block_size + within, n, p)) return 0;
		} else memset(p, 0, n);
		p += n; offset += n; size -= n;
	}
	return 1;
}
static int
ext4_lookup(struct ext4_fs *fs, const char *path, struct ext4_inode *out)
{
	struct ext4_inode dir;
	uint8_t *data = fs->directory;
	uint32_t number = 2;
	if (!path || *path++ != '/') return 0;
	for (unsigned component = 0; component < 16; component++) {
		const char *name = path;
		size_t length = 0;
		while (*path && *path != '/' && length <= 255) { path++; length++; }
		if (!length || length > 255 || (length == 1 && name[0] == '.') ||
		    (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
		if (!e4_inode(fs, number, &dir) || (dir.mode & 0xf000) != 0x4000 ||
		    dir.size > 64ULL * 1024 * 1024 || dir.size % fs->block_size) return 0;
		uint32_t found = 0;
		for (uint64_t block = 0; block < dir.size && !found; block += fs->block_size) {
			if (!ext4_read_file(fs, &dir, block, fs->block_size, data)) return 0;
			for (size_t at = 0; at < fs->block_size;) {
				if (fs->block_size - at < 8) return 0;
				uint32_t ino = e4_u32(data + at);
				unsigned record = e4_u16(data + at + 4), namesize = data[at + 6];
				if (record < 8 || (record & 3) || record > fs->block_size - at || namesize > record - 8) return 0;
				if (ino && namesize == length && !memcmp(data + at + 8, name, length)) { found = ino; break; }
				at += record;
			}
		}
		if (!found) return 0;
		number = found;
		if (!*path) return e4_inode(fs, number, out) && (out->mode & 0xf000) == 0x8000;
		path++;
	}
	return 0;
}
#endif
