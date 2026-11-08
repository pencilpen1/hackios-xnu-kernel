/* Lifetime COW/backing denial for narrowly-owned native allocations.
 * UPL_BLOCK_ACCESS + actual full-root proof still supply raw CPU exclusion.
 * No GPU/IOKit callback, UPL commit, pmap operation or sleep under this lock.
 */
#ifndef VM_OWNED_LEASE_HOST_TEST
#include <vm/vm_owned_lease.h>
#include <vm/vm_pageout.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <kern/bits.h>
#include <os/atomic_private.h>
#include <kern/misc_protos.h>
#endif

static uint64_t vm_owned_lease_generation;

/* Caller holds initial UPL lock before the object lock. No IOKit callback. */
static boolean_t
vm_owned_initial_valid_locked(struct vm_owned_lease *lease, vm_object_t object)
{
    upl_t upl = lease->initial_upl;
    if (!upl) return TRUE; /* original zero-activity registration */
    const uint32_t allowed = UPL_IO_WIRE | UPL_LITE | UPL_TRACKED_BY_OBJECT;
    if (!lease->initial_pa || !lease->initial_page_count ||
        upl->map_object != object || upl->u_offset || upl->u_size != lease->length ||
        upl->u_mapped_size || upl->kaddr || upl->ext_ref_count ||
        (upl->flags & (UPL_IO_WIRE | UPL_LITE)) != (UPL_IO_WIRE | UPL_LITE) ||
        (upl->flags & ~allowed) || !upl->lite_list) return FALSE;
    for (uint32_t i = 0; i < lease->initial_page_count; ++i) {
        vm_page_t p = vm_page_lookup(object, (vm_object_offset_t)i * PAGE_SIZE);
        if (!bitmap_test(upl->lite_list, i) || !p ||
            p->vmp_private || p->vmp_fictitious || p->vmp_absent || VMP_ERROR_GET(p) ||
            p->vmp_wire_count < 2 || VM_PAGE_GET_PHYS_PAGE(p) != lease->initial_pa[i])
            return FALSE;
    }
    return TRUE;
}

static boolean_t
vm_owned_lease_root_valid_locked(vm_object_t object, uint64_t length)
{
    vm_object_lock_assert_exclusive(object);
    return object != kernel_object && object != compressor_object &&
           object->internal && !object->private && !object->pageout &&
           !object->phys_contiguous && object->alive && !object->terminating &&
           object->purgable == VM_PURGABLE_DENY && object->vo_size == length &&
           !object->copy && !object->shadow && !object->shadowed;
}

kern_return_t
vm_owned_lease_register(vm_object_t object, void *owner, uint64_t length,
    boolean_t fresh_unexported, struct vm_owned_lease **out)
{
    struct vm_owned_lease *lease;
    if (!out) return KERN_INVALID_ARGUMENT;
    *out = NULL;
    /* fresh is an audited allocator contract, never a user/late map-count flag. */
    if (!object || !owner || !fresh_unexported || !length ||
        (length & PAGE_MASK)) return KERN_INVALID_ARGUMENT;
    lease = kalloc_type(struct vm_owned_lease, Z_WAITOK | Z_ZERO);
    if (!lease) return KERN_RESOURCE_SHORTAGE;
    uint64_t generation;
    for (;;) {
        generation = os_atomic_load(&vm_owned_lease_generation, relaxed);
        if (generation == UINT64_MAX) {
            kfree_type(struct vm_owned_lease, lease);
            return KERN_RESOURCE_SHORTAGE;
        }
        if (os_atomic_cmpxchg(&vm_owned_lease_generation, generation, generation + 1, relaxed)) {
            ++generation;
            break;
        }
    }
    vm_object_lock(object);
    if (!vm_owned_lease_root_valid_locked(object, length) || object->owned_lease ||
        object->blocked_access || object->owned_lease_building ||
        object->paging_in_progress || object->activity_in_progress) {
        vm_object_unlock(object);
        kfree_type(struct vm_owned_lease, lease);
        return KERN_PROTECTION_FAILURE;
    }
    lease->object = object;
    lease->owner = owner;
    lease->length = length;
    lease->generation = generation;
    lease->epoch = 1;
    lease->phase = VM_OWNED_BOUND_IDLE;
    vm_object_reference_locked(object);
    object->owned_lease = lease;
    vm_object_unlock(object);
    *out = lease;
    return KERN_SUCCESS;
}

/* Bounded failure-only metadata for the prepared-root registration; values are
 * copied under the existing locks and printed only after both are dropped. */
struct vm_owned_lease_reject_diag {
    uint32_t why, mask, root, page, page_wire, activity, expected_activity;
    uint32_t resident, wired, count, upl_ref, upl_ext, upl_flags;
};
static uint64_t vm_owned_lease_reject_reports;
static uint32_t
vm_owned_lease_root_detail_locked(vm_object_t object, uint64_t length)
{
    return (object == kernel_object ? 1u : 0u) | (object == compressor_object ? 2u : 0u) |
        (!object->internal ? 4u : 0u) | (object->private ? 8u : 0u) | (object->pageout ? 16u : 0u) |
        (object->phys_contiguous ? 32u : 0u) | (!object->alive ? 64u : 0u) |
        (object->terminating ? 128u : 0u) | (object->purgable != VM_PURGABLE_DENY ? 256u : 0u) |
        (object->vo_size != length ? 512u : 0u) | (object->copy ? 1024u : 0u) |
        (object->shadow ? 2048u : 0u) | (object->shadowed ? 4096u : 0u);
}
static void
vm_owned_lease_reject_report(const struct vm_owned_lease_reject_diag *d, uint64_t length)
{
    uint64_t n = __atomic_add_fetch(&vm_owned_lease_reject_reports, 1, __ATOMIC_RELAXED);
    if (!d->why || (n > 48 && (n % 256) != 0)) return;
    printf("lancelot-lease-owned-reject: why=%u mask=%#x root=%#x page=%u page_wire=%u activity=%u expected=%u resident=%u wired=%u count=%u upl_ref=%u upl_ext=%u upl_flags=%#x length=%llu n=%llu\n",
        d->why, d->mask, d->root, d->page, d->page_wire, d->activity, d->expected_activity,
        d->resident, d->wired, d->count, d->upl_ref, d->upl_ext, d->upl_flags,
        (unsigned long long)length, (unsigned long long)n);
}

kern_return_t
vm_owned_lease_register_prepared(vm_object_t object, void *owner, uint64_t length,
    upl_t initial, const upl_page_info_t *info, uint32_t count, uint64_t preparation_id,
    struct vm_owned_lease **out)
{
    struct vm_owned_lease_reject_diag diag = { .why = 0 };
    struct vm_owned_lease *lease;
    if (!out) return KERN_INVALID_ARGUMENT;
    *out = NULL;
    if (!object || !owner || !initial || !info || !length || (length & PAGE_MASK) ||
        count != length / PAGE_SIZE || preparation_id <= 2) return KERN_INVALID_ARGUMENT;
    lease = kalloc_type(struct vm_owned_lease, Z_WAITOK | Z_ZERO);
    if (!lease) return KERN_RESOURCE_SHORTAGE;
    lease->initial_pa = kalloc_data((vm_size_t)count * sizeof(ppnum_t), Z_WAITOK | Z_ZERO);
    if (!lease->initial_pa) { kfree_type(struct vm_owned_lease, lease); return KERN_RESOURCE_SHORTAGE; }
    lease->object = object; lease->owner = owner; lease->length = length;
    lease->initial_upl = initial; lease->initial_page_count = count;
    lease->initial_preparation_id = preparation_id;
    uint64_t generation;
    for (;;) {
        generation = os_atomic_load(&vm_owned_lease_generation, relaxed);
        if (generation == UINT64_MAX) goto shortage;
        if (os_atomic_cmpxchg(&vm_owned_lease_generation, generation, generation + 1, relaxed)) {
            ++generation; break;
        }
    }
    upl_lock(initial);
    vm_object_lock(object);
    uint32_t activities = 1 + ((initial->flags & UPL_TRACKED_BY_OBJECT) != 0);
    if (!vm_owned_lease_root_valid_locked(object, length) || object->owned_lease ||
        object->blocked_access || object->owned_lease_building || object->paging_in_progress ||
        object->activity_in_progress != activities || object->transposed ||
        object->resident_page_count != count || object->wired_page_count != count ||
        initial->ref_count != 1 || initial->ext_ref_count) {
        diag.why = 1;
        diag.root = vm_owned_lease_root_detail_locked(object, length);
        diag.mask = (diag.root ? 1u : 0u) | (object->owned_lease ? 2u : 0u) |
            (object->blocked_access ? 4u : 0u) | (object->owned_lease_building ? 8u : 0u) |
            (object->paging_in_progress ? 16u : 0u) | (object->activity_in_progress != activities ? 32u : 0u) |
            (object->transposed ? 64u : 0u) | (object->resident_page_count != count ? 128u : 0u) |
            (object->wired_page_count != count ? 256u : 0u) | (initial->ref_count != 1 ? 512u : 0u) |
            (initial->ext_ref_count ? 1024u : 0u);
        diag.activity = (uint32_t)object->activity_in_progress; diag.expected_activity = activities;
        diag.resident = (uint32_t)object->resident_page_count; diag.wired = (uint32_t)object->wired_page_count;
        diag.count = count; diag.upl_ref = (uint32_t)initial->ref_count;
        diag.upl_ext = (uint32_t)initial->ext_ref_count; diag.upl_flags = (uint32_t)initial->flags;
        goto rejected;
    }
    for (uint32_t i = 0; i < count; ++i) {
        vm_page_t p = vm_page_lookup(object, (vm_object_offset_t)i * PAGE_SIZE);
        if (!p || p->vmp_wire_count != 2 || p->vmp_busy || p->vmp_cleaning || p->vmp_laundry ||
            info[i].device || info[i].absent || info[i].phys_addr != VM_PAGE_GET_PHYS_PAGE(p)) {
            diag.why = 2; diag.page = i; diag.count = count;
            diag.mask = !p ? 1u : ((p->vmp_wire_count != 2 ? 2u : 0u) | (p->vmp_busy ? 4u : 0u) |
                (p->vmp_cleaning ? 8u : 0u) | (p->vmp_laundry ? 16u : 0u) | (info[i].device ? 32u : 0u) |
                (info[i].absent ? 64u : 0u) | (info[i].phys_addr != VM_PAGE_GET_PHYS_PAGE(p) ? 128u : 0u));
            if (p) diag.page_wire = (uint32_t)p->vmp_wire_count;
            goto rejected;
        }
        lease->initial_pa[i] = info[i].phys_addr;
    }
    if (!vm_owned_initial_valid_locked(lease, object)) {
        diag.why = 3; diag.count = count; diag.upl_flags = (uint32_t)initial->flags;
        diag.upl_ref = (uint32_t)initial->ref_count; diag.upl_ext = (uint32_t)initial->ext_ref_count;
        goto rejected;
    }
    /* Exact initial descriptor UPL owns one reference; proof owns the second.
     * Do not commit/unwire it or change the descriptor's normal prepare count. */
    ++initial->ref_count;
    lease->generation = generation; lease->epoch = 1; lease->phase = VM_OWNED_BOUND_IDLE;
    vm_object_reference_locked(object);
    object->owned_lease = lease;
    vm_object_unlock(object); upl_unlock(initial);
    *out = lease;
    return KERN_SUCCESS;
rejected:
    vm_object_unlock(object); upl_unlock(initial);
    vm_owned_lease_reject_report(&diag, length);
    kfree_data(lease->initial_pa, (vm_size_t)count * sizeof(ppnum_t));
    kfree_type(struct vm_owned_lease, lease);
    return KERN_PROTECTION_FAILURE;
shortage:
    kfree_data(lease->initial_pa, (vm_size_t)count * sizeof(ppnum_t));
    kfree_type(struct vm_owned_lease, lease);
    return KERN_RESOURCE_SHORTAGE;
}

kern_return_t
vm_owned_lease_transition(struct vm_owned_lease *lease, void *owner,
    enum vm_owned_lease_phase phase)
{
    vm_object_t object;
    thread_t acquired = THREAD_NULL, drop = THREAD_NULL;
    kern_return_t result = KERN_PROTECTION_FAILURE;
    if (!lease || owner != lease->owner) return KERN_INVALID_ARGUMENT;
    object = lease->object;
    if (phase == VM_OWNED_PREPARING) {
        acquired = current_thread();
        thread_reference(acquired);
    }
    if (lease->initial_upl) upl_lock(lease->initial_upl);
    vm_object_lock(object);
    if (object->owned_lease != lease ||
        lease->phase == VM_OWNED_QUARANTINED) goto done;
    if (phase == VM_OWNED_QUARANTINED) {
        /* Keep owner/object/thread lifetime and never restore raw write access. */
        lease->phase = phase;
        if (lease->epoch != UINT64_MAX) ++lease->epoch;
        result = KERN_SUCCESS;
        goto done;
    }
    if (lease->epoch == UINT64_MAX ||
        !vm_owned_lease_root_valid_locked(object, lease->length) ||
        !vm_owned_initial_valid_locked(lease, object)) goto done;
    switch (phase) {
    case VM_OWNED_PREPARING:
        if (lease->phase != VM_OWNED_BOUND_IDLE || object->blocked_access) break;
        lease->preparer = acquired;
        acquired = THREAD_NULL;
        result = KERN_SUCCESS;
        break;
    case VM_OWNED_ACTIVE:
        if (lease->phase == VM_OWNED_PREPARING &&
            lease->preparer == current_thread() && lease->upl_claimed && object->blocked_access)
            result = KERN_SUCCESS;
        break;
    case VM_OWNED_RETIRING:
        if (lease->phase == VM_OWNED_ACTIVE && object->blocked_access)
            result = KERN_SUCCESS;
        break;
    case VM_OWNED_BOUND_IDLE:
        /* PREPARING rollback has never submitted DMA. ACTIVE cannot skip RETIRING. */
        if ((lease->phase == VM_OWNED_PREPARING || lease->phase == VM_OWNED_RETIRING) &&
            !object->blocked_access) {
            drop = lease->preparer;
            lease->preparer = THREAD_NULL;
            lease->upl_claimed = FALSE;
            result = KERN_SUCCESS;
        }
        break;
    default:
        break;
    }
    if (result == KERN_SUCCESS) {
        lease->phase = phase;
        ++lease->epoch;
    }
done:
    vm_object_unlock(object);
    if (lease->initial_upl) upl_unlock(lease->initial_upl);
    if (acquired) thread_deallocate(acquired);
    if (drop) thread_deallocate(drop);
    return result;
}

boolean_t
vm_owned_lease_iopl_allowed_locked(vm_object_t object, upl_control_flags_t flags,
    vm_object_offset_t offset, upl_size_t size)
{
    struct vm_owned_lease *lease = object->owned_lease;
    vm_object_lock_assert_exclusive(object);
    if (!lease) return TRUE; /* byte-for-byte original unregistered algorithm */
    const upl_control_flags_t required = UPL_SET_LITE | UPL_SET_IO_WIRE | UPL_BLOCK_ACCESS;
    const upl_control_flags_t allowed = required | UPL_SET_INTERNAL | UPL_COPYOUT_FROM;
    return lease->phase == VM_OWNED_PREPARING &&
           !lease->upl_claimed && lease->preparer == current_thread() && offset == 0 &&
           size == lease->length && !object->blocked_access &&
           (flags & required) == required && !(flags & ~allowed);
}

boolean_t
vm_owned_lease_iopl_claim_locked(vm_object_t object, upl_control_flags_t flags,
    vm_object_offset_t offset, upl_size_t size)
{
    if (!vm_owned_lease_iopl_allowed_locked(object, flags, offset, size)) return FALSE;
    if (object->owned_lease) object->owned_lease->upl_claimed = TRUE;
    return TRUE;
}

boolean_t
vm_owned_lease_verify(struct vm_owned_lease *lease, void *owner, boolean_t blocked)
{
    boolean_t valid;
    if (!lease || lease->owner != owner) return FALSE;
    vm_object_t object = lease->object;
    if (lease->initial_upl) upl_lock(lease->initial_upl);
    vm_object_lock(object);
    valid = object->owned_lease == lease && lease->generation && lease->epoch &&
        lease->phase != VM_OWNED_QUARANTINED &&
        vm_owned_lease_root_valid_locked(object, lease->length) &&
        vm_owned_initial_valid_locked(lease, object) &&
        (object->blocked_access != 0) == (blocked != 0);
    vm_object_unlock(object);
    if (lease->initial_upl) upl_unlock(lease->initial_upl);
    return valid;
}

kern_return_t
vm_owned_lease_unregister(struct vm_owned_lease *lease, void *owner)
{
    if (!lease || lease->owner != owner) return KERN_INVALID_ARGUMENT;
    vm_object_t object = lease->object;
    upl_t initial = lease->initial_upl;
    if (initial) upl_lock(initial);
    vm_object_lock(object);
    if (object->owned_lease != lease || lease->phase != VM_OWNED_BOUND_IDLE ||
        object->blocked_access || lease->preparer) {
        vm_object_unlock(object);
        if (initial) upl_unlock(initial);
        return KERN_PROTECTION_FAILURE;
    }
    object->owned_lease = NULL; /* only known-safe owner destruction */
    vm_object_unlock(object);
    if (initial) {
        upl_unlock(initial);
        upl_deallocate(initial);
        kfree_data(lease->initial_pa, (vm_size_t)lease->initial_page_count * sizeof(ppnum_t));
    }
    kfree_type(struct vm_owned_lease, lease);
    vm_object_deallocate(object);
    return KERN_SUCCESS;
}
