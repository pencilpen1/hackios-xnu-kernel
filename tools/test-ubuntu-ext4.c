/* Host adapter for the exact freestanding loader ext4/SHA-256 implementation. */
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../xnuloader/ext4_read.h"
#include "../xnuloader/sha256.h"
struct disk { FILE *file; uint64_t bytes; };
static int read_disk(void *ctx, uint64_t offset, size_t size, void *out)
{
	struct disk *disk = ctx;
	assert(offset <= disk->bytes && size <= disk->bytes - offset);
	return !fseeko(disk->file, (off_t)offset, SEEK_SET) && fread(out, 1, size, disk->file) == size;
}
int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	struct disk disk;
	struct stat statbuf;
	int hash_only = !strcmp(argv[1], "--sha");
	const char *path = argv[hash_only ? 2 : 1];
	if (stat(path, &statbuf) || !(disk.file = fopen(path, "rb"))) return 2;
	disk.bytes = (uint64_t)statbuf.st_size;
	struct ext4_fs fs;
	struct ext4_inode inode;
	uint64_t offset = argc > 3 ? strtoull(argv[3], NULL, 0) : 0;
	uint64_t bytes;
	if (!hash_only) {
		if (!ext4_open(&fs, read_disk, &disk, disk.bytes) || !ext4_lookup(&fs, argv[2], &inode)) {
			fclose(disk.file); return 1;
		}
		bytes = argc > 4 ? strtoull(argv[4], NULL, 0) : inode.size;
	} else bytes = disk.bytes;
	if (bytes > 64 * 1024 * 1024) { fclose(disk.file); return 2; }
	uint8_t *data = malloc(bytes ? (size_t)bytes : 1);
	assert(data);
	int ok = hash_only ? read_disk(&disk, 0, bytes, data) : ext4_read_file(&fs, &inode, offset, bytes, data);
	if (ok && hash_only) {
		uint8_t digest[32]; sha256(data, bytes, digest);
		for (unsigned i = 0; i < 32; i++) printf("%02x", digest[i]);
		printf("\n");
	} else if (ok) ok = fwrite(data, 1, bytes, stdout) == bytes;
	free(data); fclose(disk.file);
	return ok ? 0 : 1;
}
