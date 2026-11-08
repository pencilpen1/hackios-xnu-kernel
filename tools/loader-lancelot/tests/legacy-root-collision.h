/* Historical size-first bootstrap negative control. */
	struct lancelot_ramdisk_variant {
		uint32_t size;
		const char *hash;
	};
	static const struct {
		const char *entry, *path;
		struct lancelot_ramdisk_variant variants[4];
	} files[] = {
		{"ramdisk", "/hackios/20H392/touch-hardware-20261024/root.apfs",
		 {{4194304u, "81a86d265596387f227c7b8a3e021f991293c2900b69a89a38b0f29be6436763"},
		  {6373376u, "537d98abb1a73234e62068f1e18dcc8ceffd3e5f0e55cb439423b4a1b9bc8536"},
		  {6377472u, "25049136ec4c6727ad7c85335cb1409b32d4f3893b6d5afc26cb090a8806e121"},
		  {6377472u, "0fcc4ff64fb44cc1caff0c26fd073c12ddb25c27ef6284719cd32abac8dbd901"}}},
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
		for (unsigned j = 0; j < 32; j++) {
			if (digits[hash[j] >> 4] != found->hash[j * 2] || digits[hash[j] & 15] != found->hash[j * 2 + 1]) {
				print("ubuntu: SHA256 mismatch %s\n", files[i].path);
				return 0;
			}
		}
		end = data + expected_size;
		print("ubuntu: %s %d bytes SHA256 verified (%s)\n", files[i].path, (uint64_t)expected_size, found->hash);
	}
	memset(end, 0, sizeof(struct payload_entry));
