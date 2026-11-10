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
/* Idempotent MT6768 board proof, shared by GPU and WLAN before first WACS2 use. */
int lancelot_pwrap_ensure_board_handoff(void);
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
/* Orderly freeze while threads/IRQs still work. Permanently denies normal
 * WACS2 clients after the current owner retires; no register writes here. */
int lancelot_pwrap_freeze_for_return(uint64_t timeout_ns);
/* Final singleton access avoids a mutex abandoned on a stopped CPU. begin
 * independently checks the real boot MPIDR and PSCI OFF for every other AP.
 * This is a WACS2 context transition, not PMIC-target/foreign-peer permission. */
int lancelot_pwrap_return_begin(uint32_t boot_mpidr);
int lancelot_pwrap_return_acquire(uint64_t timeout_ns);
void lancelot_pwrap_return_release(void);
int lancelot_pwrap_return_poison(void);
#endif
