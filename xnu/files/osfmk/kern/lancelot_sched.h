/* MT6768 idle CPU preference. Capacity classes are observed from MIDR on
 * each running CPU; no fabricated SMT topology or permanent thread binding. */
#ifndef LANCELOT_SCHED_H
#define LANCELOT_SCHED_H
#include <stdint.h>
#include <stdbool.h>
static inline unsigned
lsc_capacity_class(uint64_t midr, uint64_t mpidr)
{
    if (((midr >> 24) & 255) != 0x41 || (mpidr & 0xffffff) > 0x700 || (mpidr & 255)) return 0;
    unsigned part = (unsigned)(midr >> 4) & 4095;
    if (part == 0xd0a && (mpidr & 0xffffff) >= 0x600) return 2; /* Cortex-A75 */
    if (part == 0xd05 && (mpidr & 0xffffff) < 0x600) return 1;  /* Cortex-A55 */
    return 0;
}
static inline uint64_t
lsc_preferred_idle(uint64_t idle, uint64_t fast, uint64_t slow,
    bool foreground, bool eligible)
{
    if (!eligible || !fast || !slow || (fast & slow)) return 0;
    return idle & (foreground ? fast : slow);
}
#if defined(GENERIC_PSCI_SMP)
extern uint64_t lancelot_sched_capacity_mask(bool fast);
#endif
#endif
