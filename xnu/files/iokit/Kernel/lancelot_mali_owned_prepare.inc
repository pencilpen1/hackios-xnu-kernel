/* Included by actual IOMemoryDescriptor.cpp: uses its real private data types. */
#include "lancelot_mali_cpu_lease_vm.h"
#include "lancelot_mali_uapi.h"

/* Bounded failure-only metadata for the fresh-root admission (no pointers). */
static uint64_t gLmalOwnedPrepareReports;
static void
lmal_owned_prepare_report(uint32_t why, uint32_t mask, int result, uint64_t length, uint32_t expected,
    uint32_t wire, uint32_t flags, uint32_t ranges, uint32_t pages, uint32_t iopls)
{
    uint64_t n = __atomic_add_fetch(&gLmalOwnedPrepareReports, 1, __ATOMIC_RELAXED);
    if (n > 48 && (n % 256) != 0) return;
    printf("lancelot-lease-iomd: why=%u mask=%#x result=%d length=%llu expected=%u wire=%u flags=%#x ranges=%u pages=%u iopls=%u n=%llu\n",
        why, mask, result, (unsigned long long)length, expected, wire, flags, ranges, pages, iopls,
        (unsigned long long)n);
}

class LancelotMaliOwnedPrepare {
public:
    static int attach(IOBufferMemoryDescriptor *root, void *object, void *owner,
        uint64_t length, uint32_t expected, void **binding)
    {
        if (!binding) return LMAL_EINVAL;
        *binding = NULL;
        if (!root || !object || !owner || (expected != 1 && expected != 2) ||
            root->getMetaClass() != IOBufferMemoryDescriptor::metaClass ||
            root->getLength() != length || root->getCapacity() != length ||
            !length || (length & PAGE_MASK)) {
            uint32_t mask = (!root ? 1u : 0u) | (!object ? 2u : 0u) | (!owner ? 4u : 0u) |
                (expected != 1 && expected != 2 ? 8u : 0u) | (!length ? 128u : 0u) |
                ((length & PAGE_MASK) ? 256u : 0u);
            if (root) mask |= (root->getMetaClass() != IOBufferMemoryDescriptor::metaClass ? 16u : 0u) |
                (root->getLength() != length ? 32u : 0u) | (root->getCapacity() != length ? 64u : 0u);
            lmal_owned_prepare_report(1, mask, LMAL_ENOTSUP, length, expected, 0, 0, 0, 0, 0);
            return LMAL_ENOTSUP;
        }
        IOGeneralMemoryDescriptor *md = root;
        int result = LMAL_ENOTSUP;
        uint32_t why = 0, mask = 0, wire = 0, flags = 0, ranges = 0, pages = 0, iopls = 0;
        /* Fresh allocator caller is still unexported and single-threaded.
         * Existing lock order: IOMD -> prepare -> UPL -> VM. */
        LOCK;
        if (md->_prepareLock) IOLockLock(md->_prepareLock);
        do {
            const IOOptionBits unsupported = kIOMemoryBufferPageable | kIOMemoryBufferPurgeable |
                kIOMemoryRemote | kIOMemoryMapCopyOnWrite | kIOMemoryClearEncrypt;
            wire = (uint32_t)md->_wireCount; flags = (uint32_t)md->_flags; ranges = (uint32_t)md->_rangesCount;
            if (!md->_initialized || md->_task != kernel_task || md->_rangesCount != 1 ||
                (md->_flags & kIOMemoryTypeMask) != kIOMemoryTypeVirtual ||
                !(md->_flags & kIOMemoryAutoPrepare) || (md->_flags & unsupported) ||
                md->_wireCount != expected || md->_dmaReferences || !md->_memoryEntries) {
                why = 2;
                mask = (!md->_initialized ? 1u : 0u) | (md->_task != kernel_task ? 2u : 0u) |
                    (md->_rangesCount != 1 ? 4u : 0u) |
                    ((md->_flags & kIOMemoryTypeMask) != kIOMemoryTypeVirtual ? 8u : 0u) |
                    (!(md->_flags & kIOMemoryAutoPrepare) ? 16u : 0u) | ((md->_flags & unsupported) ? 32u : 0u) |
                    (md->_wireCount != expected ? 64u : 0u) | (md->_dmaReferences ? 128u : 0u) |
                    (!md->_memoryEntries ? 256u : 0u);
                break;
            }
            ioGMDData *data = getDataP(md->_memoryEntries);
            if (data) pages = (uint32_t)data->fPageCnt;
            if (!data || data->fMapper || data->fMappedBaseValid || data->fMappedBase ||
                data->fCompletionError || data->fPageCnt != length / PAGE_SIZE ||
                md->_memoryEntries->getLength() != computeDataSize(data->fPageCnt, 1) ||
                getNumIOPL(md->_memoryEntries, data) != 1) {
                why = 3;
                if (!data) {
                    mask = 1;
                } else {
                    iopls = (uint32_t)getNumIOPL(md->_memoryEntries, data);
                    mask = (data->fMapper ? 2u : 0u) | (data->fMappedBaseValid ? 4u : 0u) |
                        (data->fMappedBase ? 8u : 0u) | (data->fCompletionError ? 16u : 0u) |
                        (data->fPageCnt != length / PAGE_SIZE ? 32u : 0u) |
                        (md->_memoryEntries->getLength() != computeDataSize(data->fPageCnt, 1) ? 64u : 0u) |
                        (iopls != 1 ? 128u : 0u);
                }
                break;
            }
            iopls = 1;
            ioPLBlock *block = getIOPLList(data);
            if (!block->fIOPL || block->fFlags || block->fIOMDOffset || block->fPageInfo ||
                block->fPageOffset || block->fMappedPage) {
                why = 4;
                mask = (!block->fIOPL ? 1u : 0u) | (block->fFlags ? 2u : 0u) | (block->fIOMDOffset ? 4u : 0u) |
                    (block->fPageInfo ? 8u : 0u) | (block->fPageOffset ? 16u : 0u) | (block->fMappedPage ? 32u : 0u);
                break;
            }
            uint64_t preparation = md->getPreparationID();
            if (preparation <= kIOPreparationIDAlwaysPrepared ||
                preparation != data->fPreparationID) {
                why = 5;
                mask = (preparation <= kIOPreparationIDAlwaysPrepared ? 1u : 0u) |
                    (preparation != data->fPreparationID ? 2u : 0u);
                break;
            }
            mach_vm_address_t address = md->_ranges.v64[0].address;
            if (md->_ranges.v64[0].length != length) { why = 6; mask = 1; break; }
            if (lmal_cpu_vm_verify_kernel_allocation(get_task_map(kernel_task), address,
                    length, object) != LMAL_OK) { why = 6; mask = 2; break; }
            /* Authenticate actual full IOPL/PA/bitmap and base1 + IOPL1 wire.
             * No complete/unwire/reprepare or activity mutation. */
            result = lmal_cpu_vm_register_prepared(object, owner, length, block->fIOPL,
                getPageList(data), data->fPageCnt, preparation, binding);
            if (result != LMAL_OK) why = 7;
        } while (false);
        if (md->_prepareLock) IOLockUnlock(md->_prepareLock);
        UNLOCK;
        if (why) lmal_owned_prepare_report(why, mask, result, length, expected, wire, flags, ranges, pages, iopls);
        return result;
    }
    /* Mark a fresh, unmapped, kernel-owned physical IOMD (the CPU-lease cache
     * view: one page per range over pages already proven uniform) so its
     * non-contiguous device pager inherits the PA attribute instead of
     * writing it. Must run before the first mapping creates _memRef. */
    static int inheritCacheView(IOMemoryDescriptor *opaque, uint64_t length, uint32_t pages)
    {
        IOGeneralMemoryDescriptor *md = OSDynamicCast(IOGeneralMemoryDescriptor, opaque);
        if (!md || !pages || !length || length != (uint64_t)pages * PAGE_SIZE) return LMAL_ENOTSUP;
        int result = LMAL_ENOTSUP;
        LOCK;
        do {
            if (!md->_initialized || md->_task || md->_memRef || md->_rangesCount != pages ||
                md->_pages != pages || md->getLength() != length ||
                (md->_flags & kIOMemoryTypeMask) != kIOMemoryTypePhysical64) break;
            uint32_t i = 0;
            while (i < pages && md->_ranges.v64[i].address && md->_ranges.v64[i].length == PAGE_SIZE &&
                !(md->_ranges.v64[i].address & PAGE_MASK)) ++i;
            if (i != pages) break;
            IOMemoryDescriptorReserved *dpReserved = md->getKernelReserved();
            if (!dpReserved || dpReserved->dp.devicePager || dpReserved->dp.lancelotInheritWimg) break;
            dpReserved->dp.lancelotInheritWimg = 1;
            result = LMAL_OK;
        } while (false);
        UNLOCK;
        return result;
    }
};

extern "C" int
lmal_iomd_inherit_cache_view(void *opaque, uint64_t length, uint32_t pages)
{
    return LancelotMaliOwnedPrepare::inheritCacheView((IOMemoryDescriptor *)opaque, length, pages);
}

extern "C" int
lmal_iomd_register_initial(void *opaque, void *object, void *owner, uint64_t length,
    uint32_t expected_prepare_count, void **binding)
{
    return LancelotMaliOwnedPrepare::attach(
        OSDynamicCast(IOBufferMemoryDescriptor, (IOMemoryDescriptor *)opaque),
        object, owner, length, expected_prepare_count, binding);
}
