/* Kernel-only C boundary for read-only VM/UPL proof and safe-view refault.
 * VM's private C structs are intentionally not redefined in C++ IOKit code. */
#ifndef LANCELOT_MALI_CPU_LEASE_VM_H
#define LANCELOT_MALI_CPU_LEASE_VM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int lmal_cpu_vm_capture(void *address_map, uint64_t address, uint64_t length, void **object);
int lmal_cpu_vm_verify(void *object, uint64_t length, int blocked, int no_copy);
int lmal_cpu_vm_verify_upl(void *upl, void *object, uint64_t length);
int lmal_cpu_vm_cache_view(void *address_map, uint64_t address, uint64_t length, void *blocked_object);
int lmal_cpu_vm_prefault(void *address_map, uint64_t address);
void lmal_cpu_vm_release(void *object);
/* Explicit owned allocator provenance, never inferred from alias/map counts. */
enum { LMAL_VM_BOUND_IDLE = 1, LMAL_VM_PREPARING, LMAL_VM_ACTIVE,
       LMAL_VM_RETIRING, LMAL_VM_QUARANTINED };
int lmal_cpu_vm_register(void *object, void *owner, uint64_t length,
    int fresh_unexported, void **binding);
int lmal_cpu_vm_binding_verify(void *object, void *binding, void *owner, int blocked);
int lmal_cpu_vm_phase(void *binding, void *owner, int phase);
int lmal_cpu_vm_unregister(void *binding, void *owner);
int lmal_cpu_vm_verify_native_page(void *object, uint64_t offset, uint64_t pa);
/* Implemented inside the actual IOMD TU; no private C++ layout overlay. */
int lmal_iomd_register_initial(void *root, void *object, void *owner, uint64_t length,
    uint32_t expected_prepare_count, void **binding);
/* Opt a fresh unmapped one-page-per-range physical cache view into a pager
 * that inherits (never sets/resets) the PA cache attribute. */
int lmal_iomd_inherit_cache_view(void *physical, uint64_t length, uint32_t pages);
int lmal_cpu_vm_register_prepared(void *object, void *owner, uint64_t length,
    void *upl, const void *page_info, uint32_t count, uint64_t preparation_id, void **binding);
int lmal_cpu_vm_verify_kernel_allocation(void *address_map, uint64_t address,
    uint64_t length, void *object);
/* Opaque IOKit export hooks; implemented by the actual CPU lease class. */
void lmal_cpu_lease_retain(void *lease);
void lmal_cpu_lease_release(void *lease);
int lmal_cpu_lease_map_lock(void *lease, int reference_only);
void lmal_cpu_lease_map_unlock(void *lease);
#ifdef __cplusplus
}
#endif
#endif
