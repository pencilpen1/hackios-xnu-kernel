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
static uint32_t return_test_mpidr;
static int64_t return_test_affinity[8]; /* Tests explicitly supply OFF facts. */
static uint32_t return_mpidr(void) {return return_test_mpidr;}
static int64_t return_affinity(uint32_t id) {return return_test_affinity[id>>8];}
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
static uint32_t return_mpidr(void) {
    uint64_t value;__asm__ volatile("mrs %0,mpidr_el1":"=r"(value));return value&0xffffffu;
}
static int64_t return_affinity(uint32_t id) {
    register uint64_t a0 __asm__("x0")=0xc4000004ULL;
    register uint64_t a1 __asm__("x1")=id,a2 __asm__("x2")=0;
    __asm__ volatile("smc #0":"+r"(a0),"+r"(a1),"+r"(a2)::
        "x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    return (int64_t)a0;
}
#endif

static struct lancelot_pwrap_handoff handoff;
static uintptr_t owner;
static bool registered, poisoned;
static bool peer_owned;
static bool return_frozen,return_claimed,return_active;
static uint32_t return_boot_mpidr;
static uint64_t return_owner;

#ifndef LANCELOT_PWRAP_HOST_TEST
/* Board evidence for the WACS2 window. The AP owns the WACS2 command window;
 * peer processors use separate PWRAP windows (WACS_P2P / WACS_MD32) and the
 * hardware arbiter serialises bus access, so there is no same-WACS2 peer to
 * coordinate with. Sources:
 *  - vendor pwrap_hal.c: pwrap_init() ends in _pwrap_enable(), which sets
 *    HPRIO_ARB_EN=0x3fa65 with WACS0/WACS2/WACS_P2P/WACS_MD32 enabled, and
 *    pwrap_wacs2_write() drives WACS2 directly unless SSPM IPI forwarding is
 *    compiled in.
 *  - shipping Shockwave 4.19 kernel (CONFIG_MTK_TINYSYS_SSPM_SUPPORT=y,
 *    CONFIG_MTK_PMIC_WRAP=y): binds the mainline mtk-pmic-wrap driver, passes
 *    the WACS2 PMIC write test, and enables VGPU/VSRAM through the regulator
 *    framework while SSPM v1 runs; no PMIC IPI forwarding appears in dmesg.
 *  - XNU read-only sampling at the same boot: WACS2 INIT=1, FSM/REQ idle,
 *    SYNC_BUSY only from another channel.
 * This is evidence for the AP WACS2 window, not proof that every other
 * processor is stopped; the arbiter remains enabled. */
static const char lancelot_pwrap_evidence[] =
    "AP owns the WACS2 window; vendor pwrap_init enables WACS0/WACS2/P2P/MD32 "
    "with HPRIO_ARB_EN=0x3fa65 and the shipping 4.19 kernel drives WACS2 "
    "directly (write test pass, VGPU/VSRAM via regulators) while SSPM v1 "
    "runs; SSPM uses the P2P/MD32 windows; XNU read-only sampling sees WACS2 "
    "INIT=1 and FSM/REQ idle";
#endif

int
lancelot_pwrap_ensure_board_handoff(void)
{
#ifdef LANCELOT_PWRAP_HOST_TEST
    /* Host lock tests install explicit fixtures; no hardware identity is inferred. */
    return -95;
#else
    extern bool lancelot_touch_platform(void);
    if (!lancelot_touch_platform() || !irq_enabled()) return -19;
    if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return -16;
    lock();
    int error = return_frozen ? -19 : poisoned ? -5 : 0;
    if (!error && !registered) {
        handoff = (struct lancelot_pwrap_handoff){
            .mode = LANCELOT_PWRAP_AP_EXCLUSIVE, .evidence = lancelot_pwrap_evidence,
        };
        registered = true;
    }
    unlock();
    return error;
#endif
}

int
lancelot_pwrap_register_handoff(const struct lancelot_pwrap_handoff *h)
{
    if (__atomic_load_n(&return_frozen,__ATOMIC_ACQUIRE)) return -19;
    if (!irq_enabled() || !h || !h->evidence || !h->evidence[0]) return -22;
	if (h->mode != LANCELOT_PWRAP_AP_EXCLUSIVE && h->mode != LANCELOT_PWRAP_COORDINATED) return -22;
	if (h->mode == LANCELOT_PWRAP_COORDINATED && (!h->peer_acquire || !h->peer_release)) return -22;
	if (h->mode == LANCELOT_PWRAP_AP_EXCLUSIVE && (h->peer_acquire || h->peer_release)) return -22;
	if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return -16;
	lock();
    int e = return_frozen ? -19 : registered ? -16 : 0;
	if (!e) { handoff = *h; registered = true; }
	unlock();
	return e;
}

bool
lancelot_pwrap_available(void)
{
    if (__atomic_load_n(&return_frozen,__ATOMIC_ACQUIRE) || !irq_enabled() ||
        __atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return false;
	lock(); bool ready = registered && !poisoned; unlock(); return ready;
}

int
lancelot_pwrap_acquire(__unused void *unused, uint64_t timeout_ns)
{
    if (__atomic_load_n(&return_frozen,__ATOMIC_ACQUIRE)) return -19;
    if (!irq_enabled() || !timeout_ns || timeout_ns > 1000000000ull) return -22;
	if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == caller()) return -16;
	uint64_t start = now_ns();
	unsigned poll = 0;
	while (!try_lock()) {
		if (now_ns() - start >= timeout_ns || ++poll >= 100000) return -110;
		pause_us();
	}
    /* A client may have passed the fast check just before orderly freeze.
     * Do not publish even a transient normal owner after freeze: a CPU could
     * be stopped before clearing that owner, despite no hardware transaction. */
    if(return_frozen) {unlock();return -19;}
	int e = return_frozen ? -19 : !registered ? -95 : poisoned ? -5 : 0;
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

int lancelot_pwrap_freeze_for_return(uint64_t timeout_ns) {
    if(__atomic_load_n(&return_frozen,__ATOMIC_ACQUIRE))return -16;
    int error=lancelot_pwrap_acquire(NULL,timeout_ns);if(error)return error;
    if(handoff.mode!=LANCELOT_PWRAP_AP_EXCLUSIVE||peer_owned)error=-95;
    else __atomic_store_n(&return_frozen,true,__ATOMIC_RELEASE);
    lancelot_pwrap_release(NULL);return error;
}
int lancelot_pwrap_return_begin(uint32_t boot_mpidr) {
    if(!__atomic_load_n(&return_frozen,__ATOMIC_ACQUIRE)||!registered||poisoned)return -5;
    if(boot_mpidr>0x700u||(boot_mpidr&255u)||return_mpidr()!=boot_mpidr)return -22;
    for(uint32_t id=0;id<=0x700u;id+=0x100u)
        if(id!=boot_mpidr&&return_affinity(id)!=1)return -16; /* negative PSCI is NOT OFF */
    if(__atomic_load_n(&owner,__ATOMIC_ACQUIRE)||__atomic_load_n(&return_owner,__ATOMIC_ACQUIRE))return -16;
    bool inactive=false;
    if(!__atomic_compare_exchange_n(&return_claimed,&inactive,true,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -16;
    return_boot_mpidr=boot_mpidr;
    __atomic_store_n(&return_active,true,__ATOMIC_RELEASE);return 0;
}
int lancelot_pwrap_return_acquire(uint64_t timeout_ns) {
    if(!timeout_ns||timeout_ns>1000000000ull)return -22;
    if(!__atomic_load_n(&return_active,__ATOMIC_ACQUIRE)||!return_frozen||poisoned||
        return_mpidr()!=return_boot_mpidr)return -5;
    uint64_t empty=0,me=(uint64_t)return_boot_mpidr+1;
    return __atomic_compare_exchange_n(&return_owner,&empty,me,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)?0:-16;
}
void lancelot_pwrap_return_release(void) {
    uint64_t me=(uint64_t)return_mpidr()+1;
    (void)__atomic_compare_exchange_n(&return_owner,&me,0,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
int lancelot_pwrap_return_poison(void) {
    if(__atomic_load_n(&return_owner,__ATOMIC_ACQUIRE)!=(uint64_t)return_mpidr()+1)return -16;
    poisoned=true;return 0;
}
