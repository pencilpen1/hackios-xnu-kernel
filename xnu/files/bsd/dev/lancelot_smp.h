/* MT6768 CPU handoff: metadata only, no firmware callbacks or mailbox.
 * Physical reg values from Android FDT, not cpu@ unit names. */
#ifndef LANCELOT_SMP_H
#define LANCELOT_SMP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define LANCELOT_SMP_MAGIC 0x31504d534b544d4cULL
struct lancelot_smp_offer {
	uint64_t magic;
	uint32_t version, size, boot_mpidr, count;
	uint32_t mpidr[8];
	uint32_t off_mask, method; /* physical core bits; 0 = PSCI SMC */
};
_Static_assert(sizeof(struct lancelot_smp_offer) == 64, "MTK SMP handoff ABI");

static inline bool
lancelot_smp_valid(const struct lancelot_smp_offer *p)
{
	if (!p || p->magic != LANCELOT_SMP_MAGIC || p->version != 1 || p->size != sizeof(*p) ||
	    p->count != 8 || p->method || p->boot_mpidr > 0x700 || (p->boot_mpidr & 0xff) ||
	    p->mpidr[0] != p->boot_mpidr || p->off_mask != (0xffu ^ (1u << (p->boot_mpidr >> 8)))) return false;
	unsigned seen = 0;
	for (unsigned i = 0; i < 8; i++) {
		unsigned id = p->mpidr[i];
		if (id > 0x700 || (id & 255) || (seen & (1u << (id >> 8)))) return false;
		seen |= 1u << (id >> 8);
	}
	return seen == 255;
}

static inline int
lancelot_cpu_request(const char *line)
{
	/* Default same-CPU boot, or an explicit complete eight-core request.
	 * Refuse ambiguity: XNU otherwise honors only the first cpus= token. */
	int count = 1; bool supplied = false;
	if (!line) return -1;
	for (const char *p = line; *p;) {
		while (*p == ' ') p++;
		const char *end = p; while (*end && *end != ' ') end++;
		size_t n = (size_t)(end - p);
		if (n >= 5 && p[0]=='c' && p[1]=='p' && p[2]=='u' && p[3]=='s' && p[4]=='=') {
			if (supplied || n != 6 || (p[5] != '1' && p[5] != '8')) return -1;
			supplied = true; count = p[5] - '0';
		}
		if ((n >= 8 && p[0]=='b' && p[1]=='o' && p[2]=='o' && p[3]=='t' && p[4]=='c' && p[5]=='p' && p[6]=='u' && p[7]=='=') ||
		    (n >= 8 && p[0]=='c' && p[1]=='p' && p[2]=='u' && p[3]=='m' && p[4]=='a' && p[5]=='s' && p[6]=='k' && p[7]=='=')) return -1;
		p = end;
	}
	return count;
}

static inline bool
lancelot_smp_plan(uint32_t boot, int64_t (*affinity)(uint32_t), struct lancelot_smp_offer *p)
{
	if (!p) return false;
	*p = (struct lancelot_smp_offer){0};
	if (!affinity || boot > 0x700 || (boot & 255)) return false;
	struct lancelot_smp_offer plan = {.magic=LANCELOT_SMP_MAGIC, .version=1, .size=sizeof(plan),
	    .boot_mpidr=boot, .count=8, .mpidr={boot}};
	/* Keep the firmware's boot core; prefer the two A75s among secondaries.
	 * CPU_ON is performed later by XNU, never by this read-only preflight. */
	const uint32_t order[8] = {0x700, 0x600, 0, 0x100, 0x200, 0x300, 0x400, 0x500};
	unsigned at = 1;
	for (unsigned i = 0; i < 8; i++) {
		uint32_t id = order[i];
		if (id == boot) continue;
		if (affinity(id) != 1) return false; /* UNKNOWN/ON/PENDING cannot be taken */
		plan.mpidr[at++] = id; plan.off_mask |= 1u << (id >> 8);
	}
	if (!lancelot_smp_valid(&plan)) return false;
	*p = plan;
	return true;
}

#ifdef KERNEL
#include <pexpert/device_tree.h>
static inline bool
lancelot_smp_boot_allowed(uint32_t cpus)
{
	if (cpus == 1) return true;
	if (cpus != 8) return false;
	DTEntry chosen; const void *value; unsigned size;
	struct lancelot_smp_offer offer;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "lancelot-smp", &value, &size) != kSuccess || size != sizeof(offer)) return false;
	__builtin_memcpy(&offer, value, sizeof(offer));
	if (!lancelot_smp_valid(&offer)) return false;
	if (SecureDTGetProperty(chosen, "psci-method", &value, &size) != kSuccess || size != 4 || __builtin_memcmp(value, "smc", 4)) return false;
	for (unsigned i = 0; i < 8; i++) {
		char path[] = "/cpus/cpu0"; DTEntry cpu; uint32_t reg;
		path[9] = (char)('0' + i);
		if (SecureDTLookupEntry(NULL, path, &cpu) != kSuccess ||
		    SecureDTGetProperty(cpu, "reg", &value, &size) != kSuccess || size != sizeof(reg)) return false;
		__builtin_memcpy(&reg, value, sizeof(reg));
		if (reg != offer.mpidr[i]) return false;
	}
	return true;
}
#endif
#endif
