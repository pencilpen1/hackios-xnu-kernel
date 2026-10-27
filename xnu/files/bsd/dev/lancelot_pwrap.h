/* Shared WACS2 arbitration. This layer never reads/writes PWRAP registers. */
#ifndef LANCELOT_PWRAP_H
#define LANCELOT_PWRAP_H
#include <stdint.h>
#include <stdbool.h>

#define LANCELOT_PWRAP_AP_EXCLUSIVE 1u
#define LANCELOT_PWRAP_COORDINATED 2u
struct lancelot_pwrap_handoff {
	/* Board-internal evidence, NOT a bootarg/idle-FSM claim. AP_EXCLUSIVE
	 * requires proof that no firmware/other processor uses WACS2 after handoff.
	 * COORDINATED must use the actual same-channel peer exclusion protocol.
	 * Other WACS channels do not need this software lock. */
	uint32_t mode;
	const char *evidence;
	void *cookie;
	int (*peer_acquire)(void *, uint64_t timeout_ns);
	void (*peer_release)(void *);
};
/* Register once, before GPU/PMIC init. No board proof is installed yet. */
int lancelot_pwrap_register_handoff(const struct lancelot_pwrap_handoff *);
/* All current/future XNU WACS2 callers use this single lock, never private locks.
 * Thread context, IRQs enabled. Nonrecursive. Lock order: driver -> WACS2 -> peer;
 * peer callbacks must not reenter any driver or this layer. */
int lancelot_pwrap_acquire(void *unused, uint64_t timeout_ns);
void lancelot_pwrap_release(void *unused);
/* A possibly outstanding OWN command poisons the shared channel, not just GPU.
 * Caller must own the lock. Release drops the software lock, but a poisoned
 * coordinated channel RETAINS peer exclusion until reboot/new proven handoff:
 * do not hand an orphan response back to firmware by calling peer_release.
 * No foreign response is consumed/cleared as a recovery shortcut. */
int lancelot_pwrap_poison(void);
bool lancelot_pwrap_available(void);
#endif
