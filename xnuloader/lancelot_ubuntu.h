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
	/* Each slot accepts one of a few known images: the original 4 MiB RAM
	 * disk, or the GPU userland image that carries libOSMesa and gl_test.
	 * The old variant stays valid so restoring the original profile cannot
	 * be refused. Sizes are exact file lengths. */
	struct lancelot_ramdisk_variant {
		uint32_t size;
		const char *hash;
	};
	static const struct {
		const char *entry, *path;
		struct lancelot_ramdisk_variant variants[22];
	} files[] = {
		{"ramdisk", "/hackios/20H392/touch-hardware-20261024/root.apfs",
		 {{4194304u, "81a86d265596387f227c7b8a3e021f991293c2900b69a89a38b0f29be6436763"},
		  {6373376u, "537d98abb1a73234e62068f1e18dcc8ceffd3e5f0e55cb439423b4a1b9bc8536"},
		  {6377472u, "25049136ec4c6727ad7c85335cb1409b32d4f3893b6d5afc26cb090a8806e121"},
		  {6377472u, "0fcc4ff64fb44cc1caff0c26fd073c12ddb25c27ef6284719cd32abac8dbd901"},
		  {6377472u, "08dc268f9d040aa2acc1ad934de501a1addfa6be6800226ec1c30e3254baef4f"},
		  {6377472u, "50f5b99f8840236bd862fee6775459495a0c4953c65cef718404745498da2981"},
		  {6385664u, "8fe5a98ad9eed9206fd6a671be0112053a38e787bdf593c82b8a333d21736427"},
		  {6385664u, "81ba3afaffd06e4d30087b242db1da307e05d78675d8316ee1bb2c7d28d51a65"},
		  {6389760u, "ca6fcd66f125c9cdcca27557df97bddb6fec786c5562b3cb41da5f249cb03e16"},
		  {6402048u, "c077e683899ab6c6cddff8f3b894eb2c261de3bfca4f62476a97816232725578"},
		  {6406144u, "ebf267c728303b83b4a8eb6610852b4b0a9d2ee5482b3b12ecad67c6a863830f"},
		  {6406144u, "2dabe8042075985b1c17e0a8cf1e5351b1273a8dfce3e94e65accf62950eeb77"},
		  {6406144u, "ae7739193e5d729212987fc759119951cbd40e27c04b8753c1d9c1285bd04ff1"},
		  {6406144u, "b9134a04f572151821dab89451cd2ae9aa13e0bd3cbad55f9ac65b892a9e36a2"},
		  {6410240u, "b8a6957306e01d39cbde35281203a4cc24c4c28a1e17fdc1b373fdf9e6cf8bc8"},
		  {6414336u, "dc97e2b511efbcb1fee4431af0aacff35a303c834eb7496b7c13444ee4442fea"},
		  {6414336u, "d8bd0bd39ebdb626c6720a5ccf81fc46f0adddcd32ec064f3cf07875736abac9"},
		  {6426624u, "38076f5aa7f0d16955995848493a23f9a6c19fa7609eb24a61c5a2f93d5c25ab"},
		  {6426624u, "80422ed9d165334d177e4dcc7b62f832ca8342e1ef9d5f838b343f0095744b46"},
		  {6426624u, "7d68892b14a8b883258c11120c4711a7f11328295b8c1160bbf2fe5ce8b1f615"},
		  {6426624u, "30f9823aa00e81a1e279de1c4d19717c5a3c3abfd89679740821292f1e6a21a3"},
		  {6426624u, "60040ed24042afe57375be43dece4099f95fcce99149a47866acba04e1f9eb2d"}}},
		{"bootdisk", "/hackios/20H392/flashboot.apfs",
		 {{4194304u, "14485e7c5ce125ef516168445c2500ac04ca5e40a908c2aef516b2d92c24ba0c"},
		  {0u, 0}}},
	};
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		struct ext4_inode inode;
		const struct lancelot_ramdisk_variant *found = 0;
		if (!ext4_lookup(&fs, files[i].path, &inode)) {
			print("ubuntu: missing %s\n", files[i].path);
			return 0;
		}
		for (unsigned v = 0; v < sizeof(files[i].variants) / sizeof(files[i].variants[0]); v++) {
			if (files[i].variants[v].hash && files[i].variants[v].size == inode.size) {
				found = &files[i].variants[v];
				break;
			}
		}
		if (!found) {
			print("ubuntu: wrong-size %s\n", files[i].path);
			return 0;
		}
		const uint32_t expected_size = found->size;
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
		/* The first size match above bounds the read, not the image identity.
		 * Hash once, then try every same-size pin in this file's whitelist. */
		found = 0;
		for (unsigned v = 0; v < sizeof(files[i].variants) / sizeof(files[i].variants[0]); v++) {
			const struct lancelot_ramdisk_variant *candidate = &files[i].variants[v];
			if (!candidate->hash || candidate->size != expected_size) continue;
			unsigned j;
			for (j = 0; j < 32; j++) {
				if (digits[hash[j] >> 4] != candidate->hash[j * 2] ||
				    digits[hash[j] & 15] != candidate->hash[j * 2 + 1]) break;
			}
			if (j == 32) { found = candidate; break; }
		}
		if (!found) {
			print("ubuntu: SHA256 mismatch %s\n", files[i].path);
			return 0;
		}
		end = data + expected_size;
		print("ubuntu: %s %d bytes SHA256 verified (%s)\n", files[i].path, (uint64_t)expected_size, found->hash);
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
