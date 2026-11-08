/* Included once by the dedicated OSFMK C proof unit (MACH_KERNEL_PRIVATE).
 * This does not change vm_fault/pmap/pager behavior or cache attributes. */
#ifndef LMAL_CPU_LEASE_VM_HOST_TEST
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_pageout.h>
#include <vm/vm_fault.h>
#include <vm/vm_protos.h>
#include <vm/vm_owned_lease.h>
#include <kern/misc_protos.h>
#endif
#include "lancelot_mali_cpu_lease_vm.h"
#include "lancelot_mali_uapi.h"

/* Bounded failure-only metadata (no addresses/pointers): first 48, then 1/256. */
static uint64_t lmal_cpu_vm_reports;
static int lmal_cpu_vm_report_sample(void)
{
    uint64_t n = __atomic_add_fetch(&lmal_cpu_vm_reports, 1, __ATOMIC_RELAXED);
    return n <= 48 || (n % 256) == 0;
}
/* Same predicate as before, evaluated under the same lock; returns the first
 * failing term (0 = valid) so the capture diagnostic can name it. */
static int lmal_cpu_vm_verify_locked_reason(vm_object_t object, uint64_t length, int blocked, int no_copy)
{
    if (!object->internal) return 1;
    if (object->private) return 2;
    if (object->phys_contiguous) return 3;
    if (!object->alive) return 4;
    if (object->terminating) return 5;
    if (object->purgable != VM_PURGABLE_DENY) return 6;
    if (object->shadow != VM_OBJECT_NULL) return 7;
    if (no_copy && object->copy != VM_OBJECT_NULL) return 8;
    if (object->vo_size != length) return 9;
    if ((object->blocked_access != 0) != (blocked != 0)) return 10;
    return 0;
}

int lmal_cpu_vm_verify(void *opaque, uint64_t length, int blocked, int no_copy)
{
    vm_object_t object = (vm_object_t)opaque;
    int reason;
    if (!object || object == kernel_object || object == compressor_object)
        return LMAL_ENOTSUP;
    vm_object_lock(object);
    reason = lmal_cpu_vm_verify_locked_reason(object, length, blocked, no_copy);
    vm_object_unlock(object);
    return reason ? LMAL_ENOTSUP : LMAL_OK;
}
int lmal_cpu_vm_capture(void *opaque, uint64_t address, uint64_t length, void **out)
{
    vm_map_t map = (vm_map_t)opaque;
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    vm_object_t object = VM_OBJECT_NULL;
    int why = 0, reason = 0;
    uint64_t span = 0, size = 0;
    if (!out) return LMAL_EINVAL;
    *out = NULL;
    if (!map || !address || !length || (address & (PAGE_SIZE - 1)) ||
        (length & (PAGE_SIZE - 1))) return LMAL_EINVAL;
    vm_map_lock_read(map);
    if (!vm_map_lookup_entry(map, address, &entry)) why = 1;
    else if (entry->is_sub_map) why = 2;
    else if (entry->needs_copy) why = 3;
    else if (!(address >= entry->vme_start && address < entry->vme_end)) why = 4;
    else if (!(length <= entry->vme_end - address)) why = 5;
    else if (VME_OFFSET(entry) + address - entry->vme_start != 0) why = 6;
    if (why >= 4) span = entry->vme_end - entry->vme_start;
    if (!why) {
        object = VME_OBJECT(entry);
        if (object) vm_object_reference(object);
    }
    vm_map_unlock_read(map);
    if (!why) {
        if (!object) why = 7;
        else if (object == kernel_object || object == compressor_object) why = 8;
        else {
            vm_object_lock(object);
            reason = lmal_cpu_vm_verify_locked_reason(object, length, 0, 1);
            size = object->vo_size;
            vm_object_unlock(object);
            if (reason) why = 9;
        }
    }
    if (why) {
        if (object) vm_object_deallocate(object);
        if (lmal_cpu_vm_report_sample())
            printf("lancelot-lease-vm: site=capture why=%d reason=%d length=%llu span=%llu vo_size=%llu\n",
                why, reason, (unsigned long long)length, (unsigned long long)span, (unsigned long long)size);
        return LMAL_ENOTSUP;
    }
    *out = object;
    return LMAL_OK;
}
int lmal_cpu_vm_verify_upl(void *opaque, void *object, uint64_t length)
{
    upl_t upl = (upl_t)opaque;
    if (!upl || !(upl->flags & UPL_ACCESS_BLOCKED) || (upl->flags & UPL_SHADOWED) ||
        upl->map_object != (vm_object_t)object || upl->u_size != length || upl->u_offset)
        return LMAL_ENOTSUP;
    /* A newly-created COW sibling during the lease is not an ordinary shared
     * alias. GPU writes cannot honor VM's CPU copy-on-write obligations. */
    return lmal_cpu_vm_verify(object, length, 1, 1);
}
int lmal_cpu_vm_prefault(void *opaque, uint64_t address)
{
    vm_map_t map = (vm_map_t)opaque;
    if (map != kernel_map || !address || (address & (PAGE_SIZE - 1))) return LMAL_ENOTSUP;
    return vm_fault(map, address, VM_PROT_READ | VM_PROT_WRITE, FALSE,
        VM_KERN_MEMORY_IOKIT, THREAD_UNINT, NULL, 0) == KERN_SUCCESS ? LMAL_OK : LMAL_EIO;
}
int lmal_cpu_vm_cache_view(void *opaque, uint64_t address, uint64_t length, void *blocked)
{
    vm_map_t map = (vm_map_t)opaque;
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    vm_object_t object = VM_OBJECT_NULL;
    int valid;
    if (map != kernel_map || !address || !length) return LMAL_ENOTSUP;
    vm_map_lock_read(map);
    valid = vm_map_lookup_entry(map, address, &entry) && !entry->is_sub_map &&
        !entry->needs_copy && address >= entry->vme_start && address < entry->vme_end &&
        length <= entry->vme_end - address && VME_OFFSET(entry) + address - entry->vme_start == 0;
    if (valid) {
        object = VME_OBJECT(entry);
        if (object) vm_object_reference(object);
    }
    vm_map_unlock_read(map);
    if (!object || object == (vm_object_t)blocked) {
        if (object) vm_object_deallocate(object);
        return LMAL_ENOTSUP;
    }
    vm_object_lock(object);
    valid = object->private && !object->blocked_access && object->shadow == VM_OBJECT_NULL;
    vm_object_unlock(object);
    vm_object_deallocate(object);
    return valid ? LMAL_OK : LMAL_ENOTSUP;
}
void lmal_cpu_vm_release(void *opaque)
{
    if (opaque) vm_object_deallocate((vm_object_t)opaque);
}
int lmal_cpu_vm_register(void *object, void *owner, uint64_t length,
    int fresh_unexported, void **binding)
{
    struct vm_owned_lease *lease = NULL;
    if (!binding) return LMAL_EINVAL;
    *binding = NULL;
    kern_return_t kr = vm_owned_lease_register((vm_object_t)object, owner,
        length, fresh_unexported != 0, &lease);
    if (kr != KERN_SUCCESS) return kr == KERN_RESOURCE_SHORTAGE ? LMAL_ENOMEM : LMAL_ENOTSUP;
    *binding = lease;
    return LMAL_OK;
}
int lmal_cpu_vm_binding_verify(void *object, void *binding, void *owner, int blocked)
{
    struct vm_owned_lease *lease = (struct vm_owned_lease *)binding;
    return lease && lease->object == (vm_object_t)object &&
        vm_owned_lease_verify(lease, owner, blocked != 0) ? LMAL_OK : LMAL_ENOTSUP;
}
int lmal_cpu_vm_phase(void *binding, void *owner, int phase)
{
    _Static_assert(LMAL_VM_BOUND_IDLE == (int)VM_OWNED_BOUND_IDLE, "phase ABI");
    _Static_assert(LMAL_VM_QUARANTINED == (int)VM_OWNED_QUARANTINED, "phase ABI");
    return vm_owned_lease_transition((struct vm_owned_lease *)binding, owner,
        (enum vm_owned_lease_phase)phase) == KERN_SUCCESS ? LMAL_OK : LMAL_ENOTSUP;
}
int lmal_cpu_vm_unregister(void *binding, void *owner)
{
    return vm_owned_lease_unregister((struct vm_owned_lease *)binding, owner) ==
        KERN_SUCCESS ? LMAL_OK : LMAL_ENOTSUP;
}
int lmal_cpu_vm_verify_native_page(void *opaque, uint64_t offset, uint64_t pa)
{
    vm_object_t object = (vm_object_t)opaque;
    vm_page_t page;
    int valid;
    if (!object || object == kernel_object || object == compressor_object ||
        (offset & PAGE_MASK) || !pa || (pa & PAGE_MASK)) return LMAL_ENOTSUP;
    vm_object_lock(object);
    page = vm_page_lookup(object, offset);
    valid = object->owned_lease && offset < object->vo_size && page &&
        !page->vmp_private && !page->vmp_fictitious && !page->vmp_absent &&
        !VMP_ERROR_GET(page) && VM_PAGE_WIRED(page) &&
        (uint64_t)VM_PAGE_GET_PHYS_PAGE(page) * PAGE_SIZE == pa;
    vm_object_unlock(object);
    return valid ? LMAL_OK : LMAL_ENOTSUP;
}
int lmal_cpu_vm_register_prepared(void *object, void *owner, uint64_t length,
    void *upl, const void *page_info, uint32_t count, uint64_t preparation_id, void **binding)
{
    struct vm_owned_lease *lease = NULL;
    if (!binding) return LMAL_EINVAL;
    *binding = NULL;
    kern_return_t kr = vm_owned_lease_register_prepared((vm_object_t)object, owner,
        length, (upl_t)upl, (const upl_page_info_t *)page_info, count, preparation_id, &lease);
    if (kr != KERN_SUCCESS) {
        if (lmal_cpu_vm_report_sample())
            printf("lancelot-lease-vm: site=register_prepared kr=%d length=%llu count=%u\n",
                (int)kr, (unsigned long long)length, count);
        return kr == KERN_RESOURCE_SHORTAGE ? LMAL_ENOMEM : LMAL_ENOTSUP;
    }
    *binding = lease;
    return LMAL_OK;
}
int lmal_cpu_vm_verify_kernel_allocation(void *opaque, uint64_t address,
    uint64_t length, void *expected)
{
    vm_map_t map = (vm_map_t)opaque;
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    int why = 0;
    unsigned wired = 0;
    uint64_t span = 0, start_offset = 0;
    if (map != kernel_map || !expected || !address || !length ||
        (address & PAGE_MASK) || (length & PAGE_MASK)) return LMAL_ENOTSUP;
    vm_map_lock_read(map);
    if (!vm_map_lookup_entry(map, address, &entry)) why = 1;
    else if (entry->is_sub_map) why = 2;
    else if (entry->needs_copy) why = 3;
    else if (entry->vme_kernel_object) why = 4;
    else if (entry->wired_count != 1) why = 5;
    else if (entry->vme_start != address) why = 6;
    else if (entry->vme_end - address != length) why = 7;
    else if (VME_OFFSET(entry) != 0) why = 8;
    else if (VME_OBJECT(entry) != (vm_object_t)expected) why = 9;
    if (why >= 2) {
        wired = entry->wired_count;
        span = entry->vme_end - entry->vme_start;
        start_offset = address - entry->vme_start;
    }
    vm_map_unlock_read(map);
    if (why && lmal_cpu_vm_report_sample())
        printf("lancelot-lease-vm: site=kernel_allocation why=%d wired=%u length=%llu span=%llu start_offset=%llu\n",
            why, wired, (unsigned long long)length, (unsigned long long)span,
            (unsigned long long)start_offset);
    return why ? LMAL_ENOTSUP : LMAL_OK;
}
