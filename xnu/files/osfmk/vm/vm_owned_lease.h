/* Kernel-only owned allocation binding. Not a CPU PTE exclusion primitive. */
#ifndef _VM_VM_OWNED_LEASE_H_
#define _VM_VM_OWNED_LEASE_H_
#include <vm/vm_object.h>

enum vm_owned_lease_phase {
    VM_OWNED_BOUND_IDLE = 1,
    VM_OWNED_PREPARING,
    VM_OWNED_ACTIVE,
    VM_OWNED_RETIRING,
    VM_OWNED_QUARANTINED
};
struct vm_owned_lease {
    vm_object_t object; /* independent retained object reference */
    void *owner;       /* opaque identity; caller owns its OSObject lifetime */
    uint64_t length, generation, epoch;
    thread_t preparer; /* retained; sole authorized blocking-UPL creator */
    boolean_t upl_claimed; /* one request; never a substitute for real UPL proof */
    enum vm_owned_lease_phase phase;
    upl_t initial_upl; /* separately retained exact descriptor-owned IOWIRE */
    ppnum_t *initial_pa;
    uint64_t initial_preparation_id;
    uint32_t initial_page_count;
};
extern kern_return_t vm_owned_lease_register(vm_object_t, void *, uint64_t,
    boolean_t fresh_unexported, struct vm_owned_lease **);
extern kern_return_t vm_owned_lease_transition(struct vm_owned_lease *, void *,
    enum vm_owned_lease_phase);
extern kern_return_t vm_owned_lease_unregister(struct vm_owned_lease *, void *);
extern boolean_t vm_owned_lease_verify(struct vm_owned_lease *, void *, boolean_t);
extern kern_return_t vm_owned_lease_register_prepared(vm_object_t, void *, uint64_t,
    upl_t, const upl_page_info_t *, uint32_t, uint64_t, struct vm_owned_lease **);
#endif
