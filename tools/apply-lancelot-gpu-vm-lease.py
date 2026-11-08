#!/usr/bin/env python3
"""Pinned XNU8796 owned-binding relationship gate. No strategy changes.

All inputs are checked before any write. Reapplication accepts only the exact
generated output. Unknown/local VM edits fail closed, rather than being merged.
The lifetime policy is permanent KERN_PROTECTION_FAILURE, NOT lease-busy: no
new wait under a map/named-entry/UPL lock and no delayed NULL/Boolean ambiguity.

The only cache-related edit is an opt-in that REMOVES a cache-attribute write:
a physical IOMD explicitly marked by the CPU-lease bridge (and nothing else)
gets a VM_WIMG_USE_DEFAULT device pager on the non-contiguous path, so XNU's
per-page populate (private page insert + shadowed UPL alias free) never
rewrites the managed PA's WIMG; mappings inherit it via pmap_cache_attributes.
Every other IOMD, and the contiguous path, is byte-for-byte stock behavior.
"""
from pathlib import Path
import argparse
import hashlib

BASE = 'c54f35ca767986246321eb901baf8f5ff7923f6a'
OLD = {
    'osfmk/vm/vm_object.h': '8af68dc319485921dfa796b426420de959ea6c2dc8fe816d5aee0a8facfe4963',
    'osfmk/vm/vm_object.c': '7fad002b17eb966e476d5a9361b2cbbcf4ef310299da83970092b732874a77a1',
    'osfmk/vm/vm_map.h': '35b7ef151a7059e31af8cc3109b54fc498c836742d9afdb120149aad33a67235',
    'osfmk/vm/vm_map.c': '468c36e13fe76397a80854162e15ec5d7694a1112c421b1a70abc93f40daf680',
    'osfmk/vm/vm_pageout.c': 'e0aaab8c9f9c2f474bb6886ff5065d5ab4ea1feab9d36b2c168a1be85e0fe83c',
    'osfmk/vm/memory_object.c': '811e9c6132e9867dc3094e986e7385a8da1ae2c48d9650f04de9152557bab8b3',
    'iokit/IOKit/IOMemoryDescriptor.h': '56988da525cc7fff243a910f98752d2b76949c5f2d07a3bde2f8f407f6c79cf2',
    'iokit/Kernel/IOMemoryDescriptor.cpp': '9963ef1a82cfe5e1bfca797c17c686c54eeea6e631ed349babfc80ac8c824379',
    'iokit/Kernel/IOKitKernelInternal.h': '46a61b1e39437ee9334000e891a7618ce69de71315c480a11b777b02d02d8abe',
}


def replace(s, old, new, count=1):
    if s.count(old) != count:
        raise ValueError('Pinned anchor count mismatch: ' + repr(old[:120]))
    return s.replace(old, new)


def function(s, name, edit):
    """Limit edits to a release function (column-zero closing brace).

    Counting braces before preprocessing is incorrect for this release's
    #if/#else alternative blocks. All selected functions use indented inner
    closing braces; pinned full-blob hashes validate that lexical convention.
    """
    import re
    m = re.search(r'^' + re.escape(name) + r'\(.*?^\{', s, re.M | re.S)
    if not m:
        raise ValueError('Missing function ' + name)
    start = m.start()
    end = re.search(r'^\}', s[m.end():], re.M)
    if not end:
        raise ValueError('Missing function end ' + name)
    pos = m.end() + end.end()
    return s[:start] + edit(s[start:pos]) + s[pos:]


def transform(name, s):
    if name == 'iokit/IOKit/IOMemoryDescriptor.h':
        return replace(s, 'class IOGeneralMemoryDescriptor : public IOMemoryDescriptor\n{',
            'class IOGeneralMemoryDescriptor : public IOMemoryDescriptor\n{\n#ifdef XNU_KERNEL_PRIVATE\n\tfriend class LancelotMaliOwnedPrepare;\n#endif')
    if name == 'iokit/Kernel/IOMemoryDescriptor.cpp':
        # Uses this TU's actual private ioGMDData/ioPLBlock definitions. The
        # included bridge authenticates the retained initial UPL/prepare count;
        # it is not a private-layout copy or generic foreign-UPL exemption.
        def managed_diagnostics(f):
            # Keep the real managed-page classification, population, fault and
            # error paths. Kernel cache views can contain thousands of managed
            # spans: one stack per span only floods KLOG and repeats the same
            # caller. Preserve the first stack AND the full count per call.
            # User mappings retain their original per-span diagnostics.
            f = replace(f, '\ttype = _flags & kIOMemoryTypeMask;', '''#if DEBUG || DEVELOPMENT
\tuint64_t managedWarningSegments = 0, managedWarningBytes = 0;
#endif
\ttype = _flags & kIOMemoryTypeMask;''')
            f = replace(f, '''\t\t\tOSReportWithBacktrace("IOMemoryDescriptor physical with managed page 0x%qx:0x%qx",
\t\t\t    physAddr, (uint64_t)segLen);''', '''\t\t\t++managedWarningSegments;
\t\t\tmanagedWarningBytes += segLen;
\t\t\tif (addressMap != kernel_map || managedWarningSegments == 1) {
\t\t\t\tOSReportWithBacktrace("IOMemoryDescriptor physical with managed page 0x%qx:0x%qx",
\t\t\t\t    physAddr, (uint64_t)segLen);
\t\t\t}''')
            return replace(f, '\n\treturn err;\n', '''
#if DEBUG || DEVELOPMENT
\tif (addressMap == kernel_map && managedWarningSegments > 1) {
\t\tIOLog("IOMemoryDescriptor managed mapping: segments=%llu bytes=%llu first_backtrace_retained=1 error=%x\\n",
\t\t    managedWarningSegments, managedWarningBytes, err);
\t}
#endif
\treturn err;
''')
        s = function(s, 'IOMemoryDescriptor::populateDevicePager', managed_diagnostics)
        # Non-contiguous physical pager for an explicitly marked CPU-lease cache
        # view only: no pager WIMG, so vm_page_insert/UPL alias free never call
        # pmap_set_cache_attributes on the BO's managed PAs. The contiguous
        # (single-range) path is untouched: pmap_map_block uses wimg_bits.
        s = function(s, 'IOGeneralMemoryDescriptor::memoryReferenceCreate', lambda f: replace(f,
            '''\t\tif (reserved->dp.pagerContig) {
\t\t\tpagerFlags |= DEVICE_PAGER_CONTIGUOUS;
\t\t}
''', '''\t\tif (reserved->dp.pagerContig) {
\t\t\tpagerFlags |= DEVICE_PAGER_CONTIGUOUS;
\t\t} else if (reserved->dp.lancelotInheritWimg) {
\t\t\t/* Lancelot CPU-lease cache view: inherit the already-proven
\t\t\t * uniform PA attribute; never set or reset it per page. */
\t\t\tpagerFlags = VM_WIMG_USE_DEFAULT;
\t\t}
'''))
        return s + '\n#include "lancelot_mali_owned_prepare.inc"\n'
    if name == 'iokit/Kernel/IOKitKernelInternal.h':
        # One bit from the existing 31 unused bits: layout and size unchanged.
        # IOMallocType zero-fills reserved, so every other IOMD keeps 0.
        return replace(s, '\tunsigned int             unused:31;\n',
                       '\tunsigned int             lancelotInheritWimg:1;\n\tunsigned int             unused:30;\n')
    if name.endswith('vm_object.h'):
        s = replace(s, 'struct vm_page;\n', '''struct vm_page;
struct vm_owned_lease;
/* Kernel-only binding; never inherited by a copy or transposed. */
extern boolean_t vm_owned_lease_iopl_allowed_locked(vm_object_t,
    upl_control_flags_t, vm_object_offset_t, upl_size_t);
extern boolean_t vm_owned_lease_iopl_claim_locked(vm_object_t,
    upl_control_flags_t, vm_object_offset_t, upl_size_t);
''')
        s = replace(s, '\tstruct vm_object        *shadow;        /* My shadow */',
                    '\tstruct vm_owned_lease   *owned_lease;   /* Object lock protected */\n\tunsigned int            owned_lease_building; /* unregistered UPL publication */\n\tstruct vm_object        *shadow;        /* My shadow */')
        s = replace(s, '__private_extern__ boolean_t    vm_object_shadow(',
                    '__private_extern__ kern_return_t vm_object_shadow(')
        s = replace(s, '\tboolean_t               src_object_shared);',
                    '\tboolean_t               src_object_shared,\n\tkern_return_t           *lease_status);')
        return s
    if name.endswith('vm_object.c'):
        s = replace(s, '\t.shadow = VM_OBJECT_NULL,', '\t.owned_lease = NULL,\n\t.owned_lease_building = 0,\n\t.shadow = VM_OBJECT_NULL,')
        def quick(f):
            return replace(f, '\tcopy_strategy = object->copy_strategy;', '''\tif (object->owned_lease != NULL) {
\t\tvm_object_unlock(object);
\t\treturn FALSE; /* slow/strategic paths return a real error */
\t}
\tcopy_strategy = object->copy_strategy;''')
        s = function(s, 'vm_object_copy_quickly', quick)
        def slow(f):
            return replace(f, '\tif (size == 0) {', '''\tif (src_object->owned_lease != NULL) {
\t\t*_result_object = VM_OBJECT_NULL;
\t\tvm_object_unlock(src_object);
\t\treturn KERN_PROTECTION_FAILURE; /* no pixel-copy fallback */
\t}
\tif (size == 0) {''')
        s = function(s, 'vm_object_copy_slowly', slow)
        def delayed(f):
            f = replace(f, '\tboolean_t               src_object_shared)',
                        '\tboolean_t               src_object_shared,\n\tkern_return_t           *lease_status)')
            f = replace(f, '\tcopy_size = vm_object_round_page(copy_size);',
                        '\t*lease_status = KERN_SUCCESS;\n\tcopy_size = vm_object_round_page(copy_size);')
            f = replace(f, 'Retry:\n', '''Retry:
\t/* Rechecked after shared upgrade, trylock collision and allocation.
\t * No sibling is locked on entry except our private new_copy. */
\tif (src_object->owned_lease != NULL) {
\t\t*lease_status = KERN_PROTECTION_FAILURE;
\t\tvm_object_unlock(src_object);
\t\tif (new_copy != VM_OBJECT_NULL) {
\t\t\tvm_object_unlock(new_copy);
\t\t\tvm_object_deallocate(new_copy);
\t\t}
\t\treturn VM_OBJECT_NULL;
\t}
''')
            # paging_wait can release the lock; do not publish using stale state.
            f = replace(f, '\t\tvm_object_paging_wait(src_object, THREAD_UNINT);',
                        '\t\tvm_object_paging_wait(src_object, THREAD_UNINT);\n\t\tgoto Retry;')
            return f
        s = function(s, 'vm_object_copy_delayed', delayed)
        def strategic(f):
            # Strategy is reread under exclusive lock, including after pager sleep.
            a = f.index('\tcopy_strategy = src_object->copy_strategy;')
            b = f.index('\n\t/*', a)
            f = f[:a] + '\tvm_object_lock(src_object);\n' + f[b:]
            f = replace(f, '\tswitch (copy_strategy) {', '''\tcopy_strategy = src_object->copy_strategy;
\tif (src_object->owned_lease != NULL) {
\t\t*dst_object = VM_OBJECT_NULL;
\t\t*dst_offset = 0;
\t\t*dst_needs_copy = FALSE;
\t\tvm_object_unlock(src_object);
\t\treturn KERN_PROTECTION_FAILURE;
\t}
\tswitch (copy_strategy) {''')
            f = replace(f, '\tcase MEMORY_OBJECT_COPY_DELAY:\n', '\tcase MEMORY_OBJECT_COPY_DELAY: {\n\t\tkern_return_t lease_status;\n')
            f = replace(f, '\t\t    src_offset, size, object_lock_shared);', '''\t\t    src_offset, size, object_lock_shared, &lease_status);
\t\tif (lease_status != KERN_SUCCESS) {
\t\t\t*dst_offset = 0;
\t\t\t*dst_needs_copy = FALSE;
\t\t\treturn lease_status;
\t\t}''')
            f = replace(f, '\t\tOS_FALLTHROUGH; /* fall thru when delayed copy not allowed */', '''\t\tif (src_object->owned_lease != NULL) {
\t\t\tvm_object_unlock(src_object);
\t\t\treturn KERN_PROTECTION_FAILURE;
\t\t}
\t\tOS_FALLTHROUGH; /* original wired refusal, not lease denial */
\t}''')
            return f
        s = function(s, 'vm_object_copy_strategically', strategic)
        s = replace(s, '__private_extern__ boolean_t\nvm_object_shadow(',
                    '__private_extern__ kern_return_t\nvm_object_shadow(')
        def shadow(f):
            f = replace(f, '\t\treturn FALSE;', '\t\treturn KERN_SUCCESS;', 2)
            f = replace(f, '\tassert(source->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC);', '''\tvm_object_lock(source);
\tif (source->owned_lease != NULL) {
\t\tvm_object_unlock(source);
\t\treturn KERN_PROTECTION_FAILURE;
\t}
\tassert(source->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC);
\tvm_object_unlock(source);''')
            f = replace(f, '\t\t\tvm_object_lock(source);', '''\t\t\tvm_object_lock(source);
\t\t\tif (source->owned_lease != NULL) {
\t\t\t\tvm_object_unlock(source);
\t\t\t\treturn KERN_PROTECTION_FAILURE;
\t\t\t}''')
            f = replace(f, '\t*offset = vm_object_trunc_page(*offset);',
                        '\tvm_object_offset_t source_offset = vm_object_trunc_page(*offset);')
            f = replace(f, '\tresult->shadow = source;', '''\tvm_object_lock(source);
\tif (source->owned_lease != NULL) {
\t\tvm_object_unlock(source);
\t\tvm_object_deallocate(result);
\t\treturn KERN_PROTECTION_FAILURE;
\t}
\tresult->shadow = source;''')
            f = replace(f, '\tresult->vo_shadow_offset = *offset;',
                        '\tresult->vo_shadow_offset = source_offset;')
            f = replace(f, '\treturn TRUE;', '\tvm_object_unlock(source);\n\treturn KERN_SUCCESS;')
            return f
        s = function(s, 'vm_object_shadow', shadow)
        def collapse(f):
            f = replace(f, '\t\tbacking_object = object->shadow;', '''\t\tif (object->owned_lease != NULL) {
\t\t\tif (object != original_object) vm_object_unlock(object);
\t\t\treturn;
\t\t}
\t\tbacking_object = object->shadow;''')
            f = replace(f, '\t\tif (object->paging_in_progress != 0 ||', '''\t\tboolean_t next_bound = FALSE;
\t\tif (backing_object->shadow != VM_OBJECT_NULL) {
\t\t\tvm_object_lock_shared(backing_object->shadow);
\t\t\tnext_bound = backing_object->shadow->owned_lease != NULL;
\t\t\tvm_object_unlock(backing_object->shadow);
\t\t}
\t\tif (backing_object->owned_lease != NULL || next_bound) {
\t\t\tvm_object_unlock(backing_object);
\t\t\tif (object != original_object) vm_object_unlock(object);
\t\t\treturn;
\t\t}
\t\tif (object->paging_in_progress != 0 ||''')
            return f
        s = function(s, 'vm_object_collapse', collapse)
        s = function(s, 'vm_object_coalesce', lambda f: replace(f,
            '\tvm_object_lock(prev_object);', '''\tvm_object_lock(prev_object);
\tif (prev_object->owned_lease != NULL) {
\t\tvm_object_unlock(prev_object);
\t\treturn FALSE;
\t}'''))
        def transpose(f):
            f = replace(f, '\ttmp_object = vm_object_allocate(transpose_size);', '''\t/* Refuse either registered root before any paging wait under UPL locks. */
\tvm_object_lock(object1);
\tboolean_t bound1 = object1->owned_lease != NULL;
\tvm_object_unlock(object1);
\tvm_object_lock(object2);
\tboolean_t bound2 = object2->owned_lease != NULL;
\tvm_object_unlock(object2);
\tif (bound1 || bound2) {
\t\ttmp_object = VM_OBJECT_NULL;
\t\tretval = KERN_PROTECTION_FAILURE;
\t\tgoto done;
\t}
\ttmp_object = vm_object_allocate(transpose_size);''')
            f = replace(f, '\tobject1_locked = TRUE;', '''\tobject1_locked = TRUE;
\tif (object1->owned_lease != NULL) {
\t\tretval = KERN_PROTECTION_FAILURE;
\t\tgoto done;
\t}''')
            f = replace(f, '\tobject2_locked = TRUE;', '''\tobject2_locked = TRUE;
\tif (object2->owned_lease != NULL) {
\t\tretval = KERN_PROTECTION_FAILURE;
\t\tgoto done;
\t}''')
            f = replace(f, '\tvm_object_paging_only_wait(object2, THREAD_UNINT);', '''\tvm_object_paging_only_wait(object2, THREAD_UNINT);
\t/* Both locks held; paging waits may have dropped/reacquired them. */
\tif (object1->owned_lease != NULL || object2->owned_lease != NULL) {
\t\tretval = KERN_PROTECTION_FAILURE;
\t\tgoto done;
\t}''')
            return f
        s = function(s, 'vm_object_transpose', transpose)
        # Held root ref makes registered reap unreachable; never discard it.
        s = function(s, 'vm_object_reap', lambda f: replace(f,
            '\tvm_object_lock_assert_exclusive(object);', '''\tvm_object_lock_assert_exclusive(object);
\tif (object->owned_lease != NULL || object->owned_lease_building) {
\t\tpanic("vm_object_reap: live owned binding");
\t}'''))
        s = function(s, 'vm_object_terminate', lambda f: replace(f,
            '\t\tvm_object_lock(shadow_object);', '''\t\tvm_object_lock(shadow_object);
\t\tif (shadow_object->owned_lease != NULL) {
\t\t\tpanic("vm_object_terminate: unexpected inbound owned binding");
\t\t}'''))
        return s
    if name.endswith('memory_object.c'):
        return function(s, 'vm_object_update', lambda f: replace(f,
            '\tupdate_cow = ((flags & MEMORY_OBJECT_DATA_FLUSH)', '''\tvm_object_lock_assert_exclusive(object);
\tif (object->owned_lease != NULL && (flags & MEMORY_OBJECT_DATA_PURGE))
\t\treturn KERN_PROTECTION_FAILURE; /* no PA removal or hidden detach */
\tupdate_cow = ((flags & MEMORY_OBJECT_DATA_FLUSH)'''))
    if name.endswith('vm_map.h'):
        s = replace(s, 'static inline void\nVME_OBJECT_SHADOW(',
                    'static inline kern_return_t\nVME_OBJECT_SHADOW(')
        s = replace(s, '\tvm_object_shadow(&object, &offset, length, always);', '''\tkern_return_t kr = vm_object_shadow(&object, &offset, length, always);
\tif (kr != KERN_SUCCESS) return kr; /* entry/needs_copy untouched */''')
        return replace(s, '\t\tVME_OFFSET_SET(entry, offset);\n\t}\n}',
                       '\t\tVME_OFFSET_SET(entry, offset);\n\t}\n\treturn KERN_SUCCESS;\n}')
    if name.endswith('vm_map.c'):
        s = replace(s, 'static void             vm_map_fork_share(',
                    'static kern_return_t    vm_map_fork_share(')
        s = replace(s, '\t\t\t\t\t\tVME_OBJECT_SHADOW(copy_entry, copy_size, TRUE);', '''\t\t\t\t\t\tkr = VME_OBJECT_SHADOW(copy_entry, copy_size, TRUE);
\t\t\t\t\t\tif (kr != KERN_SUCCESS) break;''')
        s = replace(s, '\t\t\tVME_OBJECT_SHADOW(entry, size,\n\t\t\t    vm_map_always_shadow(map));', '''\t\t\trc = VME_OBJECT_SHADOW(entry, size,
\t\t\t    vm_map_always_shadow(map));
\t\t\tif (rc != KERN_SUCCESS) goto done;''')
        s = replace(s, '\t\t\tVME_OBJECT_SHADOW(entry, size, vm_map_always_shadow(map));', '''\t\t\trc = VME_OBJECT_SHADOW(entry, size, vm_map_always_shadow(map));
\t\t\tif (rc != KERN_SUCCESS) goto done;''')
        s = replace(s, '''\t\t\tVME_OBJECT_SHADOW(entry,
\t\t\t    (vm_map_size_t)(entry->vme_end
\t\t\t    - entry->vme_start),
\t\t\t    vm_map_always_shadow(dst_map));''', '''\t\t\tkr = VME_OBJECT_SHADOW(entry,
\t\t\t    (vm_map_size_t)(entry->vme_end
\t\t\t    - entry->vme_start),
\t\t\t    vm_map_always_shadow(dst_map));
\t\t\tif (kr != KERN_SUCCESS) { vm_map_unlock(dst_map); return kr; }''')
        s = replace(s, '''\t\t\t\tVME_OBJECT_SHADOW(entry,
\t\t\t\t    (entry->vme_end -
\t\t\t\t    entry->vme_start),
\t\t\t\t    vm_map_always_shadow(dst_map));''', '''\t\t\t\tr = VME_OBJECT_SHADOW(entry,
\t\t\t\t    (entry->vme_end -
\t\t\t\t    entry->vme_start),
\t\t\t\t    vm_map_always_shadow(dst_map));
\t\t\t\tif (r != KERN_SUCCESS) { vm_map_unlock(dst_map); return r; }''')
        s = replace(s, 'static void\nvm_map_fork_share(', 'static kern_return_t\nvm_map_fork_share(')
        s = replace(s, '''\t\tVME_OBJECT_SHADOW(old_entry,
\t\t    (vm_map_size_t) (old_entry->vme_end -
\t\t    old_entry->vme_start),
\t\t    vm_map_always_shadow(old_map));''', '''\t\tkern_return_t kr = VME_OBJECT_SHADOW(old_entry,
\t\t    (vm_map_size_t) (old_entry->vme_end -
\t\t    old_entry->vme_start),
\t\t    vm_map_always_shadow(old_map));
\t\tif (kr != KERN_SUCCESS) return kr;''')
        s = function(s, 'vm_map_fork_share', lambda f: f[:-1] + '\treturn KERN_SUCCESS;\n}')
        s = replace(s, '\t\t\tvm_map_fork_share(old_map, old_entry, new_map);', '''\t\t\tif (vm_map_fork_share(old_map, old_entry, new_map) != KERN_SUCCESS)
\t\t\t\tgoto owned_fork_failure;''')
        # Do not let the original fork error/skip loop claim a complete child.
        s = function(s, 'vm_map_fork', lambda f: replace(replace(f,
            '\t\t\tif ((old_entry->wired_count != 0) ||', '''\t\t\tvm_object_t fork_object = VME_OBJECT(old_entry);
\t\t\tif (fork_object != VM_OBJECT_NULL) {
\t\t\t\tvm_object_lock(fork_object);
\t\t\t\tboolean_t bound = fork_object->owned_lease != NULL;
\t\t\t\tvm_object_unlock(fork_object);
\t\t\t\tif (bound) goto owned_fork_failure;
\t\t\t}
\t\t\tif ((old_entry->wired_count != 0) ||'''),
            '\treturn new_map;', '''\treturn new_map;

owned_fork_failure:
#if PMAP_FORK_NEST
\tnew_entry = vm_map_last_entry(new_map);
\tif (new_entry == vm_map_to_entry(new_map)) {
\t\tvm_map_fork_unnest(new_pmap, pre_nested_start, pre_nested_end,
\t\t    vm_map_min(new_map), vm_map_max(new_map));
\t} else if (new_entry->vme_end < vm_map_max(new_map)) {
\t\tvm_map_fork_unnest(new_pmap, pre_nested_start, pre_nested_end,
\t\t    new_entry->vme_end, vm_map_max(new_map));
\t}
#endif
\tnew_map->size = new_size;
\tif (options & VM_MAP_FORK_CORPSE_FOOTPRINT)
\t\tvm_map_corpse_footprint_collect_done(new_map);
\tvm_map_unlock(new_map);
\tvm_map_unlock(old_map);
\tvm_map_deallocate(new_map);
\tvm_map_deallocate(old_map);
\treturn VM_MAP_NULL;'''))
        # Preserve lookup's existing read-lock-on-error contract, including real_map.
        s = replace(s, '''\t\t\tif (VME_OBJECT(entry)->shadowed == FALSE) {
\t\t\t\tvm_object_lock(VME_OBJECT(entry));
\t\t\t\tVME_OBJECT(entry)->shadowed = TRUE;
\t\t\t\tvm_object_unlock(VME_OBJECT(entry));
\t\t\t}''', '''\t\t\tvm_object_lock(VME_OBJECT(entry));
\t\t\tboolean_t bound = VME_OBJECT(entry)->owned_lease != NULL;
\t\t\tif (!bound) VME_OBJECT(entry)->shadowed = TRUE;
\t\t\tvm_object_unlock(VME_OBJECT(entry));
\t\t\tif (bound) {
\t\t\t\tvm_map_lock_write_to_read(map);
\t\t\t\tgoto protection_failure;
\t\t\t}
''')
        s = replace(s, '''\t\t\tVME_OBJECT_SHADOW(entry,
\t\t\t    (vm_map_size_t) (entry->vme_end -
\t\t\t    entry->vme_start),
\t\t\t    vm_map_always_shadow(map));''', '''\t\t\tif (VME_OBJECT_SHADOW(entry,
\t\t\t    (vm_map_size_t) (entry->vme_end -
\t\t\t    entry->vme_start),
\t\t\t    vm_map_always_shadow(map)) != KERN_SUCCESS) {
\t\t\t\tvm_map_lock_write_to_read(map);
\t\t\t\tgoto protection_failure;
\t\t\t}''')
        s = replace(s, '''\t\t\t\tVME_OBJECT_SHADOW(src_entry, entry_size,
\t\t\t\t    vm_map_always_shadow(map));''', '''\t\t\t\tresult = VME_OBJECT_SHADOW(src_entry, entry_size,
\t\t\t\t    vm_map_always_shadow(map));
\t\t\t\tif (result != KERN_SUCCESS) break;''')
        s = replace(s, '\t\t\tnew_object = vm_object_copy_delayed(', '\t\t\tkern_return_t lease_status;\n\t\t\tnew_object = vm_object_copy_delayed(')
        s = replace(s, '''\t\t\t\tsrc_size,
\t\t\t\tTRUE);
\t\t\tif (new_object == VM_OBJECT_NULL) {''', '''\t\t\t\tsrc_size,
\t\t\t\tTRUE, &lease_status);
\t\t\tif (new_object == VM_OBJECT_NULL && lease_status != KERN_SUCCESS) {
\t\t\t\t/* Join existing error cleanup, never CopySlowly on denial. */
\t\t\t\tresult = lease_status;
\t\t\t\tVME_OBJECT_SET(new_entry, VM_OBJECT_NULL, false, 0);
\t\t\t\tgoto owned_copy_result;
\t\t\t}
\t\t\tif (new_object == VM_OBJECT_NULL) {''')
        s = replace(s, '\t\tif (result == KERN_SUCCESS &&\n\t\t    ((preserve_purgeable &&',
                    'owned_copy_result:\n\t\tif (result == KERN_SUCCESS &&\n\t\t    ((preserve_purgeable &&')
        # Three named/direct helper RESTART->quick callers formerly assert success.
        s = replace(s, '\t\t\t\t\t\t\tassert(success);\n\t\t\t\t\t\t\tresult = KERN_SUCCESS;',
                    '\t\t\t\t\t\t\tresult = success ? KERN_SUCCESS : KERN_PROTECTION_FAILURE;')
        s = replace(s, '\t\t\tassert(success);\n\t\t\tresult = KERN_SUCCESS;',
                    '\t\t\tresult = success ? KERN_SUCCESS : KERN_PROTECTION_FAILURE;', 2)
        return s
    if name.endswith('vm_pageout.c'):
        # Keep ordinary upl_create unchanged. Only the registered blocking-UPL
        # path uses a checked constructor, before any activity/page publication.
        ctor = []
        function(s, 'upl_create', lambda f: (ctor.append(f) or f))
        checked = replace(ctor[0], 'upl_create(', 'upl_create_owned_checked(')
        checked = replace(checked,
            '\t    (type & UPL_CREATE_INTERNAL) ? pages : 0, Z_WAITOK | Z_ZERO);',
            '\t    (type & UPL_CREATE_INTERNAL) ? pages : 0, Z_WAITOK | Z_ZERO);\n\tif (upl == UPL_NULL) return UPL_NULL;')
        checked = replace(checked, '\t\t\tupl->lite_list = bitmap_alloc(pages);', '''\t\t\tupl->lite_list = bitmap_alloc(pages);
\t\t\tif (upl->lite_list == NULL) {
\t\t\t\tkfree_type(struct upl, struct upl_page_info,
\t\t\t\t    (type & UPL_CREATE_INTERNAL) ? pages : 0, upl);
\t\t\t\treturn UPL_NULL;
\t\t\t}''')
        checked = replace(checked, '\t\tupl->flags |= UPL_EXPEDITE_SUPPORTED;', '''\t\tif (upl->upl_reprio_info == NULL) {
\t\t\tupl_lock_destroy(upl);
\t\t\tif ((type & UPL_CREATE_LITE) && pages) bitmap_free(upl->lite_list, pages);
\t\t\tkfree_type(struct upl, struct upl_page_info,
\t\t\t    (type & UPL_CREATE_INTERNAL) ? pages : 0, upl);
\t\t\treturn UPL_NULL;
\t\t}
\t\tupl->flags |= UPL_EXPEDITE_SUPPORTED;''')
        s = replace(s, 'static upl_t\nupl_create(',
            '/* No object/UPL identity or page has been published on failure. */\nstatic upl_t\n' + checked + '\n\nstatic upl_t\nupl_create(')
        s = function(s, 'vm_pageout_object_terminate', lambda f: replace(f,
            '\tvm_object_lock(shadow_object);', '''\tvm_object_lock(shadow_object);
\tif (shadow_object->owned_lease != NULL) {
\t\tpanic("vm_pageout_object_terminate: unexpected inbound owned binding");
\t}'''))
        # Permanent binding rejection before wrapper/PA alias allocation.
        s = function(s, 'vm_object_upl_request', lambda f: replace(f,
            '\tdwp_start = dwp = NULL;\n\n\tif (cntrl_flags & ~UPL_VALID_FLAGS)', '''\tvm_object_lock(object);
\tboolean_t bound = object->owned_lease != NULL;
\tvm_object_unlock(object);
\tif (bound) return KERN_PROTECTION_FAILURE;
\tdwp_start = dwp = NULL;

\tif (cntrl_flags & ~UPL_VALID_FLAGS)'''))
        def upl_reservation(f):
            f = replace(f, '\tVM_DEBUG_CONSTANT_EVENT(vm_object_upl_request, VM_UPL_REQUEST, DBG_FUNC_START,', '''\t/* A registration-only reservation; original VM activity timing is unchanged. */
\tvm_object_lock(object);
\tif (object->owned_lease != NULL) {
\t\tvm_object_unlock(object);
\t\treturn KERN_PROTECTION_FAILURE;
\t}
\tif (object->owned_lease_building == UINT32_MAX) {
\t\tvm_object_unlock(object);
\t\treturn KERN_RESOURCE_SHORTAGE;
\t}
\t++object->owned_lease_building;
\tvm_object_unlock(object);
\tVM_DEBUG_CONSTANT_EVENT(vm_object_upl_request, VM_UPL_REQUEST, DBG_FUNC_START,''')
            f = replace(f, '\tvm_object_lock(object);\n\tvm_object_activity_begin(object);\n\n\tgrab_options',
                        '\tvm_object_lock(object);\n\tassert(object->owned_lease_building);\n\t--object->owned_lease_building;\n\tvm_object_activity_begin(object);\n\n\tgrab_options')
            return f
        s = function(s, 'vm_object_upl_request', upl_reservation)
        s = function(s, 'vm_object_iopl_request', lambda f: replace(f,
            '\tdwp_start = dwp = NULL;\n\n\tvm_object_offset_t original_offset', '''\tvm_object_lock(object);
\tboolean_t allowed = vm_owned_lease_iopl_allowed_locked(object,
\t    cntrl_flags, offset, size);
\tvm_object_unlock(object);
\tif (!allowed) return KERN_PROTECTION_FAILURE;
\tdwp_start = dwp = NULL;

\tvm_object_offset_t original_offset'''))
        def iopl_reservation(f):
            f = replace(f, '\tboolean_t               caller_lookup;',
                        '\tboolean_t               caller_lookup;\n\tboolean_t               owned_building = FALSE;\n\tboolean_t               owned_checked = FALSE;')
            f = replace(f, '\tVM_DEBUG_CONSTANT_EVENT(vm_object_iopl_request, VM_IOPL_REQUEST, DBG_FUNC_START,', '''\tvm_object_lock(object);
\tif (!vm_owned_lease_iopl_claim_locked(object, cntrl_flags,
\t    original_offset, original_size)) {
\t\tvm_object_unlock(object);
\t\treturn KERN_PROTECTION_FAILURE;
\t}
\tif (!object->owned_lease && object != kernel_object && object != compressor_object) {
\t\tif (object->owned_lease_building == UINT32_MAX) {
\t\t\tvm_object_unlock(object);
\t\t\treturn KERN_RESOURCE_SHORTAGE;
\t\t}
\t\t++object->owned_lease_building;
\t\towned_building = TRUE;
\t}
\towned_checked = object->owned_lease != NULL;
\tvm_object_unlock(object);
\tVM_DEBUG_CONSTANT_EVENT(vm_object_iopl_request, VM_IOPL_REQUEST, DBG_FUNC_START,''')
            f = replace(f, '\t\tvm_object_lock(object);\n\t\tvm_object_activity_begin(object);\n\t}',
                         '\t\tvm_object_lock(object);\n\t\tif (owned_building) {\n\t\t\tassert(object->owned_lease_building);\n\t\t\t--object->owned_lease_building;\n\t\t}\n\t\tvm_object_activity_begin(object);\n\t}')
            f = replace(f, '''\tif (cntrl_flags & UPL_SET_INTERNAL) {
\t\tupl = upl_create(UPL_CREATE_INTERNAL | UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize);
\t\tuser_page_list = size ? upl->page_list : NULL;
\t} else {
\t\tupl = upl_create(UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize);
\t}''', '''\tif (cntrl_flags & UPL_SET_INTERNAL) {
\t\tupl = owned_checked ?
\t\t    upl_create_owned_checked(UPL_CREATE_INTERNAL | UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize) :
\t\t    upl_create(UPL_CREATE_INTERNAL | UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize);
\t} else {
\t\tupl = owned_checked ?
\t\t    upl_create_owned_checked(UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize) :
\t\t    upl_create(UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize);
\t}
\tif (owned_checked && upl == UPL_NULL) {
\t\t/* No activity, page lock/wire, mapping or UPL identity was published.
\t\t * Retain the sole claim until the actual owner rolls PREPARING back;
\t\t * do not admit a second/foreign attempt or manufacture a completed job. */
\t\t*upl_ptr = UPL_NULL;
\t\tif (dwp_start != NULL && dwp_finish_ctx) vm_page_delayed_work_finish_ctx(dwp_start);
\t\treturn KERN_RESOURCE_SHORTAGE;
\t}
\tif (cntrl_flags & UPL_SET_INTERNAL) user_page_list = size ? upl->page_list : NULL;''')
            return f
        s = function(s, 'vm_object_iopl_request', iopl_reservation)
        # All vector sub-UPLs checked before submap creation/first publication.
        helper = '''static boolean_t
vm_owned_lease_upl_mapping_allowed(upl_t upl)
{
\tvm_object_t map_object = upl->map_object;
\tvm_object_t source = VM_OBJECT_NULL;
\tboolean_t bound;
\t/* The existing mapper rejects already mapped UPLs without consulting their
\t * object. Preserve that result/lock contract, including vector sub-UPLs. */
\tif (upl->flags & UPL_PAGE_LIST_MAPPED) return TRUE;
\tif (map_object == VM_OBJECT_NULL) return FALSE;
\tvm_object_lock(map_object);
\tbound = map_object->owned_lease != NULL;
\tif (!bound && (upl->flags & UPL_SHADOWED)) {
\t\tsource = map_object->shadow;
\t\tif (source == VM_OBJECT_NULL) {
\t\t\tvm_object_unlock(map_object);
\t\t\treturn FALSE; /* no provable wrapper source; never create a PA alias */
\t\t}
\t\tvm_object_reference(source);
\t}
\tvm_object_unlock(map_object);
\tif (source != VM_OBJECT_NULL) {
\t\tvm_object_lock(source);
\t\tbound = source->owned_lease != NULL;
\t\tvm_object_unlock(source);
\t\tvm_object_deallocate(source);
\t}
\treturn !bound;
}

'''
        s = replace(s, 'kern_return_t\nvm_map_enter_upl_range(',
                    helper + 'kern_return_t\nvm_map_enter_upl_range(')
        s = function(s, 'vm_map_enter_upl_range', lambda f: replace(replace(f,
            '\t\t\tvalid_upls++;', '''\t\t\tif (!vm_owned_lease_upl_mapping_allowed(upl)) {
\t\t\t\tupl_unlock(vector_upl);
\t\t\t\treturn KERN_PROTECTION_FAILURE;
\t\t\t}
\t\t\tvalid_upls++;'''),
            '\t\tupl_lock(upl);\n\t}', '''\t\tupl_lock(upl);
\t\tif (!vm_owned_lease_upl_mapping_allowed(upl)) {
\t\t\tupl_unlock(upl);
\t\t\treturn KERN_PROTECTION_FAILURE;
\t\t}
\t}'''))
        return s
    raise ValueError(name)


def apply(tree, originals=None):
    pending = {}
    for name, old_sha in OLD.items():
        if originals is None:
            import subprocess
            old = subprocess.check_output(['git', 'show', BASE + ':' + name], cwd=tree)
        else:
            old = (originals / name).read_bytes()
        if hashlib.sha256(old).hexdigest() != old_sha:
            raise ValueError('Wrong fixed Apple blob: ' + name)
        new = transform(name, old.decode()).encode()
        actual = (tree / name).read_bytes()
        if actual not in (old, new):
            raise ValueError('Unknown VM input (not overwritten): ' + name)
        pending[name] = new
    file_list = tree / 'osfmk/conf/files'
    if file_list.exists():
        raw = file_list.read_bytes()
        line = b'osfmk/vm/vm_owned_lease.c\tstandard\n'
        if b'vm_owned_lease.c' in raw and raw.count(line) != 1:
            raise ValueError('Unknown owned lease build registration')
        if line not in raw:
            pending['osfmk/conf/files'] = raw + (b'' if raw.endswith(b'\n') else b'\n') + line
    for name, data in pending.items():
        if (tree / name).read_bytes() != data:
            (tree / name).write_bytes(data)
        print(name, hashlib.sha256(data).hexdigest())


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('tree', type=Path)
    p.add_argument('--fixed-source', type=Path)
    a = p.parse_args()
    # Fresh apply uses fixed Git blobs. Offline tests may use audited snapshots.
    apply(a.tree, a.fixed_source)
