/* Bounded UEFI PWRAP read-only observation, before the same-core XNU jump.
 * No PMIC write API, power/voltage/DMA changes or ownership-provider creation.
 * Mu's driver owns its own firmware protocol clients, not all secure peers. */
#ifndef XNULOADER_LANCELOT_PMIC_H
#define XNULOADER_LANCELOT_PMIC_H
#include "efi.h"

static const uint16_t lpm_read_addresses[9] = {
	0x000a, 0x1508, 0x1520, 0x1526, 0x151e, 0x1bc8, 0x1be4, 0x1bf4, 0x1be2
};
struct lpm_snapshot { uint16_t raw[9]; };

/* Absent/off=0, exactly one =1 token=1, any malformed/duplicate mode=-1. */
static int
lpm_requested_named(const char *cmdline, const char *key)
{
	size_t key_len=0;
	if (!key) return -1;
	while (key[key_len]) key_len++;
	if (!key_len) return -1;
	int seen = 0, enabled = 0;
	if (!cmdline) return -1;
	while (*cmdline) {
		while (*cmdline == ' ') cmdline++;
		const char *start = cmdline;
		while (*cmdline && *cmdline != ' ') cmdline++;
		size_t len = (size_t)(cmdline - start), i;
		if (len < key_len) continue;
		for (i = 0; i < key_len && start[i] == key[i]; i++) {}
		if (i != key_len) continue;
		if (seen++ || len != key_len+1 || (start[i] != '0' && start[i] != '1')) return -1;
		enabled = start[i] == '1';
	}
	return enabled;
}
static int lpm_requested(const char *cmdline) {
	return lpm_requested_named(cmdline,"lancelot_pmic_probe=");
}

static efi_status
lpm_capture(const efi_mtk_pwrap_checked *io, struct lpm_snapshot *out)
{
	struct lpm_snapshot s = {{0}};
	uint16_t final_identity;
	if (!io || !io->read || !out) return EFI_UNSUPPORTED;
	for (unsigned i = 0; i < 9; i++) {
		efi_status status = io->read(lpm_read_addresses[i], &s.raw[i]);
		if (status) return EFI_ERROR(status) ? status : (EFI_ERROR_BIT | 7);
		if (!i && s.raw[0] != 0x5810 && s.raw[0] != 0x5820 && s.raw[0] != 0x5830)
			return EFI_UNSUPPORTED; /* HWCID is not SWCID. Do not read rails first. */
	}
	efi_status status = io->read(0x000a, &final_identity);
	if (status) return EFI_ERROR(status) ? status : (EFI_ERROR_BIT | 7);
	if (final_identity != s.raw[0]) return EFI_UNSUPPORTED;
	*out = s; /* No partial snapshot is published on a failed transaction. */
	return 0;
}
#endif
