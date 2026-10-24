/* Read the verified bootstrap APFS files through existing firmware Block I/O.
 * Runs before XNU owns the CPU. Large iOS containers remain on Ubuntu; this is
 * not a firmware-mailbox or runtime native eMMC implementation.
 */
#define XNULOADER_NO_LIBC
#include "ext4_read.h"
#include "sha256.h"
#undef XNULOADER_NO_LIBC

#define UBUNTU_PARTITION_BYTES 33285963776ULL
static uint64_t ubuntu_partition_offset;

static efi_block_io *
lancelot_ubuntu_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID, bio_guid = EFI_BLOCK_IO_GUID;
	static const char16 name[] = u"Ubuntu";
	efi_handle *handles = NULL;
	uint64_t count = 0;
	efi_block_io *found = NULL;
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) return NULL;
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *part;
		efi_block_io *bio;
		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&part)) || part->type != 2 ||
		    memcmp(part->name, name, sizeof(name))) continue;
		if (EFI_ERROR(BS->handle_protocol(handles[i], &bio_guid, (void **)&bio)) || !bio->media) continue;
		efi_block_media *m = bio->media;
		if (!m->present || !m->logical || m->block_size < 512 || m->block_size > 4096 ||
		    (m->block_size & (m->block_size - 1)) || m->io_align > 8192 ||
		    (m->io_align && (m->io_align & (m->io_align - 1))) ||
		    m->last_block != UBUNTU_PARTITION_BYTES / m->block_size - 1) continue;
		if (part->end_lba < part->start_lba || part->end_lba - part->start_lba != m->last_block ||
		    part->start_lba >= ((1ULL << 41) - UBUNTU_PARTITION_BYTES) / m->block_size) continue;
		if (found) { found = NULL; print("ubuntu: ambiguous partition handles\n"); break; }
		found = bio;
		ubuntu_partition_offset = part->start_lba * m->block_size;
	}
	BS->free_pool(handles);
	return found;
}

static int
lancelot_ubuntu_read(void *ctx, uint64_t offset, size_t size, void *out)
{
	efi_block_io *bio = ctx;
	uint64_t bs = bio->media->block_size, at = offset & ~(bs - 1), skip = offset - at;
	if (offset > UBUNTU_PARTITION_BYTES || size > UBUNTU_PARTITION_BYTES - offset ||
	    size > BLOCK_BUFFER_SIZE - bs) return 0;
	uint64_t bytes = (skip + size + bs - 1) & ~(bs - 1);
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, at / bs, bytes, block_buffer))) return 0;
	memcpy(out, block_buffer + skip, size);
	return 1;
}

static int
lancelot_ubuntu_bootstrap(void)
{
	struct payload_info pi;
	if (!parse_payload(&pi, 0)) return 0;
	if (!pi.ubuntu_boot) return 1;
	if (pi.ramdisk_size || pi.bootdisk_size || pi.resident_rd) {
		print("ubuntu: bootstrap conflicts with embedded/resident disks\n");
		return 0;
	}
	efi_block_io *bio = lancelot_ubuntu_partition();
	static struct ext4_fs fs;
	if (!bio || !ext4_open(&fs, lancelot_ubuntu_read, bio, UBUNTU_PARTITION_BYTES)) {
		print("ubuntu: partition absent or unsupported/unclean ext4\n");
		return 0;
	}
	struct payload_header *header = (void *)(uintptr_t)PAYLOAD_BASE;
	/* The host packer puts the terminator last. Refuse hidden trailing data. */
	uint8_t zero[sizeof(struct payload_entry)] = {0};
	if (header->size < sizeof(zero)) return 0;
	uint8_t *end = (uint8_t *)(header + 1) + header->size - sizeof(struct payload_entry);
	if (memcmp(end, zero, sizeof(zero))) return 0;
	uint8_t *cursor = (uint8_t *)(header + 1);
	while (cursor < end) {
		struct payload_entry *entry = (void *)cursor;
		if ((size_t)(end - cursor) < sizeof(*entry) || !entry->name[0]) return 0;
		uint64_t span = sizeof(*entry) + (((uint64_t)entry->size + 15) & ~15ULL);
		if (span > (uint64_t)(end - cursor)) return 0;
		cursor += span;
	}
	if (cursor != end) return 0;
	static const struct {
		const char *entry, *path, *hash;
	} files[] = {
		{"ramdisk", "/hackios/20H392/touch-hardware-20261024/root.apfs", "81a86d265596387f227c7b8a3e021f991293c2900b69a89a38b0f29be6436763"},
		{"bootdisk", "/hackios/20H392/flashboot.apfs", "14485e7c5ce125ef516168445c2500ac04ca5e40a908c2aef516b2d92c24ba0c"},
	};
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		struct ext4_inode inode;
		const uint32_t expected_size = 4194304;
		if (!ext4_lookup(&fs, files[i].path, &inode) || inode.size != expected_size) {
			print("ubuntu: missing/wrong-size %s\n", files[i].path);
			return 0;
		}
		uint64_t offset = (uint64_t)(end - (uint8_t *)header);
		if (offset > PAYLOAD_MAX_SIZE - 0x100000 - expected_size - 2 * sizeof(struct payload_entry)) return 0;
		struct payload_entry *entry = (void *)end;
		memset(entry, 0, sizeof(*entry));
		memcpy(entry->name, files[i].entry, str_len(files[i].entry));
		entry->size = expected_size;
		uint8_t *data = end + sizeof(*entry), hash[32];
		if (!ext4_read_file(&fs, &inode, 0, expected_size, data) || memcmp(data + 32, "NXSB", 4)) {
			print("ubuntu: failed APFS read %s\n", files[i].path);
			return 0;
		}
		sha256(data, expected_size, hash);
		static const char digits[] = "0123456789abcdef";
		for (unsigned j = 0; j < 32; j++) {
			if (digits[hash[j] >> 4] != files[i].hash[j * 2] || digits[hash[j] & 15] != files[i].hash[j * 2 + 1]) {
				print("ubuntu: SHA256 mismatch %s\n", files[i].path);
				return 0;
			}
		}
		end = data + expected_size;
		print("ubuntu: %s %d bytes SHA256 verified (%s)\n", files[i].path, (uint64_t)expected_size, files[i].hash);
	}
	memset(end, 0, sizeof(struct payload_entry));
	header->size = (uint32_t)(end + sizeof(struct payload_entry) - (uint8_t *)(header + 1));
	uint8_t superblock[1024];
	if (!lancelot_ubuntu_read(bio, 1024, sizeof(superblock), superblock)) return 0;
	memset(&mtk_storage_info, 0, sizeof(mtk_storage_info));
	mtk_storage_info.magic = LANCELOT_STORAGE_MAGIC;
	mtk_storage_info.version = 2;
	mtk_storage_info.size = sizeof(mtk_storage_info);
	mtk_storage_info.mmio = 0x11230000;
	mtk_storage_info.partition_offset = ubuntu_partition_offset;
	mtk_storage_info.partition_bytes = UBUNTU_PARTITION_BYTES;
	sha256(superblock, sizeof(superblock), mtk_storage_info.superblock_sha256);
	/* Pinned MT6768 firmware assigns RCA 1 and muxes DAT0..DAT7; the Android
	 * board DT also declares an 8-bit eMMC bus. No firmware source is changed. */
	mtk_storage_info.rca = 1;
	mtk_storage_info.max_bus_width = 8;
	mtk_storage_info.hs_timing = 1; /* CardSetBusMode in the pinned firmware selects HS timing. */
	static const efi_guid clock_guid = EFI_MTK_CLOCK_GUID;
	efi_mtk_clock *clock;
	uint32_t clock_id;
	uint64_t source_hz;
	if (!EFI_ERROR(BS->locate_protocol(&clock_guid, NULL, (void **)&clock)) && clock && clock->get_id && clock->get_frequency &&
	    !EFI_ERROR(clock->get_id("TOP_MSDC50_0", &clock_id)) &&
	    !EFI_ERROR(clock->get_frequency(clock_id, &source_hz)) && source_hz > 0 && source_hz <= 1000000000)
		mtk_storage_info.source_clock_hz = source_hz;
	print("ubuntu: bootstrap ready: flashboot=md0, root=md1; firmware reads complete before XNU\n");
	print("ubuntu: native MSDC handoff: partition byte %x, superblock digest recorded\n", ubuntu_partition_offset);
	print("ubuntu: eMMC RCA=1 wired-width=8 source-clock=%d Hz\n", mtk_storage_info.source_clock_hz);
	return 1;
}
