#include "lancelot_pwrap.h"
#ifdef LANCELOT_PWRAP_HOST_TEST
#define __unused __attribute__((unused))
#include <pthread.h>
#include <time.h>
#include <string.h>
static pthread_mutex_t wacs2_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t caller(void) { return (uintptr_t)pthread_self(); }
static bool irq_enabled(void) { return true; }
static void lock(void) { pthread_mutex_lock(&wacs2_lock); }
static bool try_lock(void) { return pthread_mutex_trylock(&wacs2_lock) == 0; }
static void unlock(void) { pthread_mutex_unlock(&wacs2_lock); }
static uint64_t now_ns(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void pause_us(void) { struct timespec t = {0, 1000}; nanosleep(&t, NULL); }
#else
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
static LCK_GRP_DECLARE(wacs2_group, "lancelot-wacs2");
static LCK_MTX_DECLARE(wacs2_lock, &wacs2_group);
static uintptr_t caller(void) { return (uintptr_t)current_thread(); }
static bool irq_enabled(void) { return ml_get_interrupts_enabled(); }
static void lock(void) { lck_mtx_lock(&wacs2_lock); }
static bool try_lock(void) { return lck_mtx_try_lock(&wacs2_lock); }
static void unlock(void) { lck_mtx_unlock(&wacs2_lock); }
static uint64_t now_ns(void) {
	uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;
}
static void pause_us(void) { delay_for_interval(1, NSEC_PER_USEC); }
#endif

static struct lancelot_pwrap_handoff handoff;
static uintptr_t owner;
static bool registered, poisoned;
static bool peer_owned;

int
lancelot_pwrap_register_handoff(const struct lancelot_pwrap_handoff *h)
{
	if (!irq_enabled() || !h || !h->evidence || !h->evidence[0]) return -22;
	if (h->mode != LANCELOT_PWRAP_AP_EXCLUSIVE && h->mode != LANCELOT_PWRAP_COORDINATED) return -22;
	if (h->mode == LANCELOT_PWRAP_COORDINATED && (!h->peer_acquire || !h->peer_release)) return -22;
	if (h->mode == LANCELOT_PWRAP_AP_EXCLUSIVE && (h->peer_acquire || h->peer_release)) return -22;
	if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return -16;
	lock();
	int e = registered ? -16 : 0;
	if (!e) { handoff = *h; registered = true; }
	unlock();
	return e;
}

bool
lancelot_pwrap_available(void)
{
	if (!irq_enabled() || __atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return false;
	lock(); bool ready = registered && !poisoned; unlock(); return ready;
}

int
lancelot_pwrap_acquire(__unused void *unused, uint64_t timeout_ns)
{
	if (!irq_enabled() || !timeout_ns || timeout_ns > 1000000000ull) return -22;
	if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return -16;
	uint64_t start = now_ns();
	unsigned poll = 0;
	while (!try_lock()) {
		if (now_ns() - start >= timeout_ns || ++poll >= 100000) return -110;
		pause_us();
	}
	int e = !registered ? -95 : poisoned ? -5 : 0;
	/* Mark the software owner before peer callbacks, so accidental peer
	 * reentry is rejected rather than trying to reacquire our nonrecursive lock. */
	__atomic_store_n(&owner, caller(), __ATOMIC_RELEASE);
	uint64_t elapsed = now_ns() - start;
	if (!e && elapsed >= timeout_ns) e = -110;
	bool acquired_peer = false;
	if (!poisoned) peer_owned = false; /* retain a poisoned peer token across denied attempts */
	if (!e && handoff.mode == LANCELOT_PWRAP_COORDINATED) {
		e = handoff.peer_acquire(handoff.cookie, timeout_ns - elapsed);
		if (!e) {
			peer_owned = true;
			acquired_peer = true;
			if (now_ns() - start >= timeout_ns) e = -110;
		}
	}
	if (e) {
		if (acquired_peer) { handoff.peer_release(handoff.cookie); peer_owned = false; }
		__atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
		unlock(); return e;
	}
	return 0;
}

int
lancelot_pwrap_poison(void)
{
	if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) != caller()) return -16;
	poisoned = true; return 0;
}

void
lancelot_pwrap_release(__unused void *unused)
{
	if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) != caller()) return;
	/* Unknown OWN response must not be handed to a same-channel firmware peer.
	 * Keep its exclusion token after poisoning; no automatic recovery in boot. */
	if (peer_owned && !poisoned) { handoff.peer_release(handoff.cookie); peer_owned = false; }
	__atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
	unlock();
}
