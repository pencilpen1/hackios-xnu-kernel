/* Owned IOBuffer CPU alias lease, XNU8796.121.2. No pixel copies or cache retag.
 * A full-root UPL_BLOCK_ACCESS gates the VM object, not a list of CPU pointers.
 * Cache maintenance uses a separate same-PA kernel pager, refaulted before DMA.
 * Borrowed/device/global-kernel/shadow/mixed-cache backing is unsupported. */
#ifndef LANCELOT_MALI_CPU_LEASE_H
#define LANCELOT_MALI_CPU_LEASE_H
#ifndef LMAL_CPU_LEASE_HOST_TEST
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOKitServer.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <kern/task.h>
#include <vm/memory_types.h>
#endif
#include "lancelot_mali_uapi.h"
#include "lancelot_mali_cpu_lease_vm.h"

/* Metadata-only reason a fresh-root lease was not created (diagnostic; the
 * NULL result and every check are unchanged). step 0 means a lease exists. */
struct lmal_cpu_lease_why {
    uint32_t step, detail; uint64_t value;
    /* First sub-stage where any page's cache bits differ from fCacheBits:
     * 1 after native pages, 2 after physical IOMD, 3 after cache-view map,
     * 4 after cache_view check, 5 page right after its own prefault, 6 final. */
    uint32_t stage, first, bits, mismatch, final_first, final_bits, final_mismatch, pages;
};

class LancelotMaliCpuLease : public OSObject {
    OSDeclareDefaultStructors(LancelotMaliCpuLease);
    enum { SLOT_COUNT = 64 }; /* up to the donor FIFO's queued reservations */
    IOLock *fGate;
    IOBufferMemoryDescriptor *fRoot;
    IOMemoryDescriptor *fPhysical;
    IOMemoryMap *fHolder, *fCache;
    void *fObject; /* retained C VM proof object, not a C++ struct overlay */
    void *fBinding; /* retained VM-owned binding, installed only on fresh roots */
    IOAddressRange *fPages;
    uint32_t fPageCount, fCacheBits, fGpuCount, fReaders, fWriters, fCpuWaiters;
    uint32_t fIdleRootRetains;
    uint64_t fLength, fNextCookie, fLastPair, fLastWriter;
    struct { uint64_t cookie, pair; bool writer, published; } fSlots[SLOT_COUNT];
    struct { task_t task; uint32_t reads, writes; } fUsers[SLOT_COUNT];
    uint32_t fUserReads, fUserWrites;
    int fError, fWriteError; /* unsafe DMA vs latest accepted writer completion */
    bool fFrozen, fQuarantined, fGpuRetain;

    static uint64_t nowNs() {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        return ns;
    }
    static uint64_t deadline(uint64_t remaining) {
        uint64_t now = nowNs();
        if (remaining > LMAL_MAX_WAIT_NS) remaining = LMAL_MAX_WAIT_NS;
        return remaining > UINT64_MAX - now ? UINT64_MAX : now + remaining;
    }
    bool sleepUntil(void *event, uint64_t end) {
        uint64_t now = nowNs(), absolute = 0;
        if (!now || now >= end) return false;
        nanoseconds_to_absolutetime(end, &absolute);
        (void)IOLockSleepDeadline(fGate, event, absolute, THREAD_UNINT);
        uint64_t after = nowNs();
        return after >= now && after < end;
    }
    bool objectValid(bool blocked, bool noCopy = false) const {
        return lmal_cpu_vm_verify(fObject, fLength, blocked, noCopy) == LMAL_OK;
    }
    bool captureObject() {
        mach_vm_address_t address = fHolder->getAddress();
        if (!fHolder->fAddressMap || !address || fHolder->fSuperMap ||
            (fHolder->getMapOptions() & kIOMapStatic) ||
            fHolder->getMemoryDescriptor() != fRoot || fHolder->fOffset ||
            fHolder->getLength() != fLength) return false;
        return lmal_cpu_vm_capture(fHolder->fAddressMap, address, fLength, &fObject) == LMAL_OK;
    }
    /* Pure re-read of captureObject's holder prechecks, for the diagnostic only. */
    uint32_t holderMask() const {
        return (!fHolder->fAddressMap ? 1u : 0u) | (!fHolder->getAddress() ? 2u : 0u) |
            (fHolder->fSuperMap ? 4u : 0u) | ((fHolder->getMapOptions() & kIOMapStatic) ? 8u : 0u) |
            (fHolder->getMemoryDescriptor() != fRoot ? 16u : 0u) | (fHolder->fOffset ? 32u : 0u) |
            (fHolder->getLength() != fLength ? 64u : 0u);
    }
    bool cacheValid() const {
        for (uint32_t i = 0; i < fPageCount; ++i)
            if ((IODefaultCacheBits(fPages[i].address) & VM_WIMG_MASK) != fCacheBits)
                return false;
        return true;
    }
    /* Diagnostic-only pure reads of the per-PA cache bits. */
    uint32_t cacheMismatch(uint32_t *first, uint32_t *bits) const {
        uint32_t n = 0;
        for (uint32_t i = 0; i < fPageCount; ++i) {
            uint32_t b = IODefaultCacheBits(fPages[i].address) & VM_WIMG_MASK;
            if (b != fCacheBits) { if (!n) { *first = i; *bits = b; } ++n; }
        }
        return n;
    }
    void noteCache(lmal_cpu_lease_why *why, uint32_t stage) const {
        if (!why || why->stage) return;
        uint32_t first = 0, bits = 0, n = cacheMismatch(&first, &bits);
        if (n) { why->stage = stage; why->first = first; why->bits = bits; why->mismatch = n; }
    }
    bool prefaultCache(uint32_t *detail = NULL, lmal_cpu_lease_why *why = NULL) {
        if (lmal_cpu_vm_cache_view(fCache->fAddressMap, fCache->getAddress(),
            fLength, fObject) != LMAL_OK) { if (detail) *detail = 1; return false; }
        noteCache(why, 4);
        for (uint32_t i = 0; i < fPageCount; ++i) {
            mach_vm_address_t address = fCache->getAddress() + (uint64_t)i * PAGE_SIZE;
            if (lmal_cpu_vm_prefault(fCache->fAddressMap, address) != LMAL_OK) {
                if (detail) *detail = 2 | (i << 4);
                return false;
            }
            if (why && !why->stage) {
                uint32_t b = IODefaultCacheBits(fPages[i].address) & VM_WIMG_MASK;
                if (b != fCacheBits) { why->stage = 5; why->first = i; why->bits = b; why->mismatch = 1; }
            }
            IOByteCount length = 0;
            if (fCache->getPhysicalSegment((uint64_t)i * PAGE_SIZE, &length) !=
                fPages[i].address || length < PAGE_SIZE) {
                if (detail) *detail = 3 | (i << 4);
                return false;
            }
        }
        bool valid = cacheValid();
        if (!valid) {
            if (detail) *detail = 4;
            if (why) why->final_mismatch = cacheMismatch(&why->final_first, &why->final_bits);
            noteCache(why, 6);
        }
        return valid;
    }
    bool blockedProof() const {
        return lmal_cpu_vm_verify_upl(fHolder->fRedirUPL, fObject, fLength) == LMAL_OK &&
            lmal_cpu_vm_binding_verify(fObject, fBinding, (void *)this, true) == LMAL_OK &&
            nativePagesValid();
    }
    bool nativePagesValid() const {
        for (uint32_t i = 0; i < fPageCount; ++i)
            if (lmal_cpu_vm_verify_native_page(fObject, (uint64_t)i * PAGE_SIZE,
                fPages[i].address) != LMAL_OK) return false;
        return true;
    }
    bool reopen() {
        if (lmal_cpu_vm_binding_verify(fObject, fBinding, this, fFrozen) != LMAL_OK)
            return false;
        IOReturn ret = fHolder->redirect(fRoot, kIOMapDefaultCache, (mach_vm_size_t)0);
        if (ret != kIOReturnSuccess || fHolder->fRedirUPL || !objectValid(false))
            return false;
        fFrozen = false;
        return cacheValid() && lmal_cpu_vm_phase(fBinding, this, LMAL_VM_BOUND_IDLE) == LMAL_OK;
    }
    void poison(int error) {
        if (!fError) fError = error ? error : LMAL_EIO;
        fQuarantined = true;
        if (fBinding) (void)lmal_cpu_vm_phase(fBinding, this, LMAL_VM_QUARANTINED);
        /* UPL/root/cache/self retains remain. Never wake raw stores into
         * possibly-owned backing or let map destruction auto-commit the UPL. */
        IOLockWakeup(fGate, &fGpuCount, false);
        IOLockWakeup(fGate, &fReaders, false);
    }
    bool cpuConflict(task_t own, bool legacy) const {
        uint32_t reads = fUserReads, writes = fUserWrites;
        if (legacy)
            for (uint32_t i = 0; i < SLOT_COUNT; ++i) if (fUsers[i].task == own) {
                reads -= fUsers[i].reads; writes -= fUsers[i].writes;
            }
        /* A legacy producer must let a pending CPU observer reach a quiet
         * root. Already-queued GPU cookies still complete. An existing user
         * writer may need GPU work to finish its own lock, so do not block
         * that dependency before it releases the logical write ownership. */
        return fReaders || fWriters || reads || writes ||
            (legacy && fCpuWaiters && !fWriters && !fUserWrites);
    }
    static LancelotMaliCpuLease *rootFailure(LancelotMaliCpuLease *s, lmal_cpu_lease_why *why,
        uint32_t step, uint32_t detail, uint64_t value) {
        if (why) { why->step = step; why->detail = detail; why->value = value; }
        if (s) s->release();
        return NULL;
    }
public:
    static LancelotMaliCpuLease *withRoot(IOBufferMemoryDescriptor *root, bool freshUnexported = false,
        uint32_t expectedPrepareCount = 0, lmal_cpu_lease_why *why = NULL) {
        if (why) *why = lmal_cpu_lease_why();
        if (!freshUnexported || (expectedPrepareCount != 1 && expectedPrepareCount != 2) ||
            !root || (PAGE_SIZE != 4096 && PAGE_SIZE != 16384) ||
            !root->getLength() || root->getLength() != root->getCapacity() ||
            (root->getLength() & (PAGE_SIZE - 1)) ||
            root->getLength() > LMAL_MAX_OWNED_SURFACE_BYTES + PAGE_SIZE) {
            uint32_t mask = (!freshUnexported ? 1u : 0u) |
                (expectedPrepareCount != 1 && expectedPrepareCount != 2 ? 2u : 0u) | (!root ? 4u : 0u);
            if (root) mask |= (!root->getLength() ? 8u : 0u) |
                (root->getLength() != root->getCapacity() ? 16u : 0u) |
                ((root->getLength() & (PAGE_SIZE - 1)) ? 32u : 0u) |
                (root->getLength() > LMAL_MAX_OWNED_SURFACE_BYTES + PAGE_SIZE ? 64u : 0u);
            return rootFailure(NULL, why, 1, mask, root ? (uint64_t)root->getLength() : 0);
        }
        LancelotMaliCpuLease *s = OSTypeAlloc(LancelotMaliCpuLease);
        if (!s || !s->init()) return rootFailure(s, why, 2, 0, 0);
        s->fGate = IOLockAlloc();
        s->fLength = root->getLength();
        s->fRoot = root; root->retain();
        s->fPageCount = (uint32_t)(s->fLength / PAGE_SIZE);
        if (why) why->pages = s->fPageCount;
        s->fPages = IONew(IOAddressRange, s->fPageCount);
        if (!s->fGate || !s->fPages) return rootFailure(s, why, 3, 0, s->fLength);
        s->fHolder = root->createMappingInTask(kernel_task, 0,
            kIOMapAnywhere | kIOMapUnique, 0, s->fLength);
        if (!s->fHolder) return rootFailure(s, why, 4, 0, s->fLength);
        if (!s->captureObject()) return rootFailure(s, why, 5, s->holderMask(), s->fLength);
        int registered = lmal_iomd_register_initial(root, s->fObject, s, s->fLength,
            expectedPrepareCount, &s->fBinding);
        if (registered != LMAL_OK) return rootFailure(s, why, 6, (uint32_t)registered, s->fLength);
        for (uint32_t i = 0; i < s->fPageCount; ++i) {
            IOByteCount length = 0;
            uint64_t pa = root->getPhysicalSegment((uint64_t)i * PAGE_SIZE, &length);
            if (!pa || (pa & (PAGE_SIZE - 1)) || length < PAGE_SIZE)
                return rootFailure(s, why, 7, (i << 4) | (!pa ? 1u : 0u) |
                    ((pa & (PAGE_SIZE - 1)) ? 2u : 0u) | (length < PAGE_SIZE ? 4u : 0u), (uint64_t)length);
            s->fPages[i].address = pa; s->fPages[i].length = PAGE_SIZE;
            uint32_t bits = IODefaultCacheBits(pa) & VM_WIMG_MASK;
            if ((!i && bits != VM_WIMG_COPYBACK && bits != VM_WIMG_WCOMB) ||
                (i && bits != s->fCacheBits))
                return rootFailure(s, why, 8, (i << 8) | (bits & 0xffu), s->fCacheBits);
            s->fCacheBits = bits;
        }
        if (!s->nativePagesValid()) {
            uint32_t bad = 0;
            while (bad < s->fPageCount && lmal_cpu_vm_verify_native_page(s->fObject,
                (uint64_t)bad * PAGE_SIZE, s->fPages[bad].address) == LMAL_OK) ++bad;
            return rootFailure(s, why, 9, bad, s->fPageCount);
        }
        s->noteCache(why, 1);
        /* Explicitly INHERIT the already-proved uniform mode. No pmap/cache
         * setter is used, and existing PA attributes must remain identical:
         * the cache-view pager is opted into VM_WIMG_USE_DEFAULT, otherwise
         * XNU's per-page populate resets every multi-page BO PA to DEFAULT. */
        IOOptionBits cache = s->fCacheBits == VM_WIMG_WCOMB ?
            kIOWriteCombineCache : kIOCopybackCache;
        s->fPhysical = IOMemoryDescriptor::withAddressRanges(s->fPages, s->fPageCount,
            kIODirectionInOut | kIOMemoryMapperNone |
            (cache << kIOMemoryBufferCacheShift), NULL);
        if (!s->fPhysical) return rootFailure(s, why, 10, 0, s->fCacheBits);
        if (lmal_iomd_inherit_cache_view(s->fPhysical, s->fLength, s->fPageCount) != LMAL_OK)
            return rootFailure(s, why, 10, 1, s->fCacheBits);
        s->noteCache(why, 2);
        s->fCache = s->fPhysical->createMappingInTask(kernel_task, 0,
            kIOMapAnywhere | kIOMapDefaultCache, 0, s->fLength);
        if (!s->fCache) return rootFailure(s, why, 11, 1, s->fCacheBits);
        if (s->fCache->getLength() != s->fLength) return rootFailure(s, why, 11, 2, s->fCache->getLength());
        if (s->fCache->getMemoryDescriptor() == root) return rootFailure(s, why, 11, 3, s->fCacheBits);
        s->noteCache(why, 3);
        uint32_t prefault = 0;
        if (!s->prefaultCache(&prefault, why)) return rootFailure(s, why, 11, 8 | (prefault << 4), s->fCacheBits);
        s->fIdleRootRetains=root->getRetainCount();
        return s;
    }
    /* A kept IOSurface transfers this root together with its memory. Capture
     * the constructor's owned references before any export, rather than
     * guessing that the holder and memory-entry implementation add two. */
    bool canRecycle(IOBufferMemoryDescriptor *memory) {
        IOLockLock(fGate);
        bool safe=memory==fRoot && getRetainCount()==1 && fIdleRootRetains &&
            fRoot->getRetainCount()==fIdleRootRetains && !fGpuCount && !fCpuWaiters && !fReaders && !fWriters &&
            !fUserReads && !fUserWrites && !fFrozen && !fGpuRetain && !fError && !fWriteError && !fQuarantined &&
            objectValid(false,true) && cacheValid() &&
            lmal_cpu_vm_binding_verify(fObject,fBinding,this,false)==LMAL_OK;
        IOLockUnlock(fGate);return safe;
    }
    void *cacheAddress() const { return fCache ? (void *)fCache->getAddress() : NULL; }
    IOBufferMemoryDescriptor *root() const { return fRoot; }
    /* Mapping gate is acquired BEFORE entering IOMD/global memory lock. A
     * reference-only lookup can unmap/close during DMA without creating PTEs. */
    int lockMapping(bool reference) {
        IOLockLock(fGate);
        uint64_t end = deadline(LMAL_MAX_WAIT_NS);
        while (!reference && fGpuCount && !fError)
            if (!sleepUntil(&fGpuCount, end)) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
        if (!reference && fError) { IOLockUnlock(fGate); return fError; }
        return LMAL_OK; /* caller holds gate through makeMapping */
    }
    void unlockMapping() { IOLockUnlock(fGate); }
    int beginGpu(uint64_t remaining_ns, uint64_t *cookie, bool requireBlock = true, bool writer = true, bool waitCpu = true,
        bool *firstGpu = NULL) {
        if (firstGpu) *firstGpu = false;
        if (!cookie || !remaining_ns) return LMAL_EINVAL;
        *cookie = 0;
        IOLockLock(fGate);
        uint64_t end = deadline(remaining_ns);
        task_t own = current_task();
        bool ownsUserLock = false;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
            ownsUserLock |= fUsers[i].task == own && (fUsers[i].reads || fUsers[i].writes);
        if (requireBlock && ownsUserLock) { IOLockUnlock(fGate); return LMAL_ENOTSUP; }
        if (!waitCpu && cpuConflict(own, !requireBlock) && !fError) {
            IOLockUnlock(fGate); return LMAL_EBUSY;
        }
        while (cpuConflict(own, !requireBlock) && !fError)
            if (!sleepUntil(&fReaders, end)) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
        if (fError || fQuarantined) { int e = fError ? fError : LMAL_EIO; IOLockUnlock(fGate); return e; }
        uint32_t slot;
        for (slot = 0; slot < SLOT_COUNT && fSlots[slot].cookie; ++slot) {}
        if (slot == SLOT_COUNT || fNextCookie == UINT64_MAX) {
            IOLockUnlock(fGate); return LMAL_EBUSY;
        }
        if (fGpuCount && requireBlock && !fFrozen) {
            IOLockUnlock(fGate); return LMAL_ENOTSUP;
        }
        if (!fGpuCount) {
            uint64_t before = nowNs();
            if (!before || before >= end) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
            if (!cacheValid()) { IOLockUnlock(fGate); return LMAL_EIO; }
            bool blockable = objectValid(false, true) &&
                lmal_cpu_vm_binding_verify(fObject, fBinding, this, false) == LMAL_OK;
            if (!blockable) { IOLockUnlock(fGate); return LMAL_ENOTSUP; }
            retain(); fGpuRetain = true;
            if (lmal_cpu_vm_phase(fBinding, this, LMAL_VM_PREPARING) != LMAL_OK) {
                fGpuRetain = false;
                IOLockUnlock(fGate); release(); return LMAL_ENOTSUP;
            }
            if (blockable) {
                IOReturn ret = fHolder->redirect((IOMemoryDescriptor *)NULL,
                    kIOMapDefaultCache, (mach_vm_size_t)0);
                fFrozen = fHolder->fRedirUPL != NULL;
                bool proof = ret == kIOReturnSuccess && blockedProof();
                bool cacheReady = proof && prefaultCache();
                if (!proof || !cacheReady) {
                    bool safe = fFrozen ? reopen() : objectValid(false) &&
                        lmal_cpu_vm_phase(fBinding, this, LMAL_VM_BOUND_IDLE) == LMAL_OK;
                    if (!safe) { poison(LMAL_EIO); IOLockUnlock(fGate); return LMAL_EIO; }
                    fGpuRetain = false;
                    IOLockUnlock(fGate); release(); return proof ? LMAL_EIO : LMAL_ENOTSUP;
                }
            }
            /* CPU alias exclusion/refault is preparation, not a fresh GPU
             * budget. Exhaustion after preparing must unwind before the
             * cookie is published; no job/fence has been accepted yet. */
            uint64_t after = nowNs();
            if (after < before || after >= end) {
                int e = after < before ? LMAL_EIO : LMAL_ETIMEDOUT;
                if (fFrozen && !reopen()) { poison(e); IOLockUnlock(fGate); return LMAL_EIO; }
                fGpuRetain = false;
                IOLockUnlock(fGate); release(); return e;
            }
            if (lmal_cpu_vm_phase(fBinding, this, LMAL_VM_ACTIVE) != LMAL_OK) {
                poison(LMAL_EIO); IOLockUnlock(fGate); return LMAL_EIO;
            }
        }
        if (firstGpu) *firstGpu = fGpuCount == 0;
        fSlots[slot].cookie = ++fNextCookie;
        fSlots[slot].pair = 0;
        fSlots[slot].writer = writer;
        fSlots[slot].published = false;
        ++fGpuCount;
        *cookie = fSlots[slot].cookie;
        IOLockUnlock(fGate);
        return LMAL_OK;
    }
    int bindPair(uint64_t cookie, uint64_t pair) {
        IOLockLock(fGate);
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
            if (fSlots[i].cookie == cookie && cookie && pair && !fSlots[i].published) {
                fSlots[i].pair = pair; fLastPair = pair;
                fSlots[i].published = true;
                if (fSlots[i].writer) { fLastWriter = cookie; fWriteError = 0; }
                IOLockUnlock(fGate); return LMAL_OK;
            }
        IOLockUnlock(fGate); return LMAL_EINVAL;
    }
    int markLegacy(uint64_t cookie) {
        IOLockLock(fGate);
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
            if (cookie && fSlots[i].cookie == cookie && !fSlots[i].published) {
                fSlots[i].published = true;
                if (fSlots[i].writer) { fLastWriter = cookie; fWriteError = 0; }
                IOLockUnlock(fGate); return LMAL_OK;
            }
        IOLockUnlock(fGate); return LMAL_EINVAL;
    }
    int endGpu(uint64_t cookie, int result, bool dma_safe,
        void (*lastGpuSync)(void *, size_t) = NULL) {
        IOLockLock(fGate);
        uint32_t slot;
        for (slot = 0; slot < SLOT_COUNT && fSlots[slot].cookie != cookie; ++slot) {}
        if (!cookie || slot == SLOT_COUNT || !fGpuCount) {
            IOLockUnlock(fGate); return LMAL_EINVAL;
        }
        if (!dma_safe || fQuarantined) {
            poison(result); IOLockUnlock(fGate); return LMAL_EBUSY;
        }
        if (fFrozen && !blockedProof()) {
            /* Keep the frozen backing if a new COW/shadow relationship or
             * UPL ownership mutation invalidated the full-root proof. There
             * is no safe way to repair VM copy semantics by copying pixels. */
            poison(LMAL_EIO); IOLockUnlock(fGate); return LMAL_EBUSY;
        }
        if (fSlots[slot].published && fSlots[slot].writer && cookie == fLastWriter)
            fWriteError = result;
        if (fGpuCount == 1 && fFrozen) {
            /* The last reservation may be a cancelled preparation following
             * an already completed GPU writer. Invalidate at this atomic
             * ownership boundary, not at every queued job's completion. */
            if (lastGpuSync) lastGpuSync(cacheAddress(), fLength);
            if (lmal_cpu_vm_phase(fBinding, this, LMAL_VM_RETIRING) != LMAL_OK || !reopen()) {
                poison(result); IOLockUnlock(fGate); return LMAL_EIO;
            }
        }
        fSlots[slot].cookie = fSlots[slot].pair = 0;
        fSlots[slot].writer = fSlots[slot].published = false;
        --fGpuCount;
        bool drop = !fGpuCount && fGpuRetain;
        if (drop) fGpuRetain = false;
        IOLockWakeup(fGate, &fGpuCount, false);
        IOLockWakeup(fGate, &fReaders, false);
        IOLockUnlock(fGate);
        if (drop) release();
        return LMAL_OK;
    }
    int waitCpu(uint64_t remaining_ns) {
        IOLockLock(fGate);
        if(fCpuWaiters==UINT32_MAX){IOLockUnlock(fGate);return LMAL_EBUSY;}
        ++fCpuWaiters;
        uint64_t end = deadline(remaining_ns);
        int ret=LMAL_OK;
        while (fGpuCount && !fError)
            if (!sleepUntil(&fGpuCount, end)) {ret=LMAL_ETIMEDOUT;break;}
        if(!ret)ret=fError ? fError : fQuarantined ? LMAL_EIO : fWriteError;
        --fCpuWaiters;IOLockWakeup(fGate,&fReaders,false);
        IOLockUnlock(fGate); return ret;
    }
    int beginCpuRead(uint64_t remaining_ns) {
        IOLockLock(fGate);
        if(fCpuWaiters==UINT32_MAX){IOLockUnlock(fGate);return LMAL_EBUSY;}
        ++fCpuWaiters;
        uint64_t end = deadline(remaining_ns);
        int ret=LMAL_OK;
        while ((fGpuCount || fWriters || fUserWrites) && !fError)
            if (!sleepUntil(&fGpuCount, end)) {ret=LMAL_ETIMEDOUT;break;}
        if(!ret && (fError || fWriteError || fQuarantined || fReaders == UINT32_MAX))
            ret=fError ? fError : fWriteError ? fWriteError : LMAL_EBUSY;
        if(!ret){++fReaders;retain();}
        --fCpuWaiters;IOLockWakeup(fGate,&fReaders,false);
        IOLockUnlock(fGate); return ret;
    }
    void endCpuRead() {
        IOLockLock(fGate);
        bool drop = fReaders != 0;
        if (drop) --fReaders;
        IOLockWakeup(fGate, &fReaders, false);
        IOLockUnlock(fGate);
        if (drop) release();
    }
    int beginCpuWrite(uint64_t remaining_ns) {
        IOLockLock(fGate);
        uint64_t end = deadline(remaining_ns);
        while ((fGpuCount || fReaders || fWriters || fUserReads || fUserWrites) && !fError)
            if (!sleepUntil(&fReaders, end)) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
        if (fError || fQuarantined) {
            int e = fError ? fError : LMAL_EIO; IOLockUnlock(fGate); return e;
        }
        fWriters = 1; retain();
        IOLockUnlock(fGate); return LMAL_OK;
    }
    void endCpuWrite() {
        IOLockLock(fGate);
        bool drop = fWriters != 0;
        if (drop) { fWriters = 0; fWriteError = 0; }
        IOLockWakeup(fGate, &fReaders, false);
        IOLockWakeup(fGate, &fGpuCount, false);
        IOLockUnlock(fGate);
        if (drop) release();
    }
    int beginUserCpu(task_t task, bool writer, uint64_t remaining_ns) {
        if (!task || task != current_task()) return LMAL_EPERM;
        IOLockLock(fGate);
        uint64_t end = deadline(remaining_ns);
        for (;;) {
            uint32_t ownWrites = 0, ownReads = 0;
            for (uint32_t i = 0; i < SLOT_COUNT; ++i)
                if (fUsers[i].task == task) { ownWrites = fUsers[i].writes; ownReads = fUsers[i].reads; }
            /* Mixed modes cannot wait for a lock held by this same task. */
            if ((writer && ownReads) || (!writer && ownWrites)) {
                IOLockUnlock(fGate); return LMAL_EBUSY;
            }
            bool conflict = fGpuCount || fWriters || (fUserWrites > ownWrites) ||
                (writer && (fReaders || fUserReads));
            if (!conflict || fError) break;
            if (!sleepUntil(&fReaders, end)) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
        }
        if (fError || fQuarantined || (!writer && fWriteError)) {
            int e = fError ? fError : fWriteError ? fWriteError : LMAL_EIO;
            IOLockUnlock(fGate); return e;
        }
        uint32_t slot = SLOT_COUNT, empty = SLOT_COUNT;
        /* An earlier empty slot must not split one task into two ownership
         * records: endUserCpu would otherwise drop the wrong kind/retain. */
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            if (fUsers[i].task == task) { slot = i; break; }
            if (!fUsers[i].task && empty == SLOT_COUNT) empty = i;
        }
        if (slot == SLOT_COUNT) slot = empty;
        if (slot == SLOT_COUNT || (writer ? fUserWrites : fUserReads) == UINT32_MAX) {
            IOLockUnlock(fGate); return LMAL_EBUSY;
        }
        if (!fUsers[slot].task) { fUsers[slot].task = task; task_reference(task); }
        if (writer) { ++fUsers[slot].writes; ++fUserWrites; }
        else { ++fUsers[slot].reads; ++fUserReads; }
        retain(); IOLockUnlock(fGate); return LMAL_OK;
    }
    void endUserCpu(task_t task, bool writer) {
        IOLockLock(fGate);
        bool drop = false, dropTask = false;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) if (fUsers[i].task == task) {
            uint32_t &count = writer ? fUsers[i].writes : fUsers[i].reads;
            if (count) {
                --count; drop = true;
                if (writer) { --fUserWrites; fWriteError = 0; }
                else --fUserReads;
            }
            if (!fUsers[i].reads && !fUsers[i].writes) { fUsers[i].task = NULL; dropTask = true; }
            break;
        }
        IOLockWakeup(fGate, &fReaders, false);
        IOLockWakeup(fGate, &fGpuCount, false);
        IOLockUnlock(fGate);
        if (dropTask) task_deallocate(task);
        if (drop) release();
    }
    bool gpuProof(uint64_t cookie) {
        IOLockLock(fGate);
        bool found = false;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) found |= cookie && fSlots[i].cookie == cookie;
        bool proof = found && fFrozen && !fError && !fQuarantined && blockedProof() && cacheValid();
        IOLockUnlock(fGate); return proof;
    }
    virtual void free() APPLE_KEXT_OVERRIDE {
        if (fGpuCount || fCpuWaiters || fReaders || fWriters || fUserReads || fUserWrites || fGpuRetain || fFrozen ||
            (fHolder && fHolder->fRedirUPL)) {
            /* Retains normally make this unreachable. Fail closed on an
             * ownership bug rather than auto-commit a live UPL/descriptors. */
            return;
        }
        if (fBinding) {
            if (lmal_cpu_vm_unregister(fBinding, this) != LMAL_OK) return;
            fBinding = NULL;
        }
        if (fCache) fCache->release();
        if (fPhysical) fPhysical->release();
        if (fHolder) fHolder->release();
        if (fObject) lmal_cpu_vm_release(fObject);
        if (fPages) IODelete(fPages, IOAddressRange, fPageCount);
        if (fRoot) fRoot->release();
        if (fGate) IOLockFree(fGate);
        OSObject::free();
    }
};

/* Presenter/readback metadata users hold every root in stable order through
 * helper joins and the existing record path. No pixel operation is added. */
class LancelotMaliReadGroup {
    enum { MAX_ROOTS = 16 };
    LancelotMaliCpuLease *fRoots[MAX_ROOTS] = {};
    uint32_t fCount = 0, fHeld = 0;
public:
    bool add(LancelotMaliCpuLease *root) {
        if (!root) return true;
        if (fHeld) return false;
        uint32_t i = 0;
        while (i < fCount && (uintptr_t)fRoots[i] < (uintptr_t)root) ++i;
        if (i < fCount && fRoots[i] == root) return true;
        if (fCount == MAX_ROOTS) return false;
        for (uint32_t j = fCount; j > i; --j) fRoots[j] = fRoots[j - 1];
        fRoots[i] = root; root->retain(); ++fCount;
        return true;
    }
    int acquire(uint64_t remaining_ns) {
        if (fHeld) return LMAL_EBUSY;
        uint64_t began = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &began);
        for (; fHeld < fCount; ++fHeld) {
            uint64_t now = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &now);
            int result = now < began ? LMAL_EIO : now - began >= remaining_ns ?
                LMAL_ETIMEDOUT : fRoots[fHeld]->beginCpuRead(remaining_ns - (now - began));
            if (result) { releaseReaders(); return result; }
        }
        return LMAL_OK;
    }
    void releaseReaders() {
        while (fHeld) fRoots[--fHeld]->endCpuRead();
    }
    ~LancelotMaliReadGroup() {
        releaseReaders();
        while (fCount) fRoots[--fCount]->release();
    }
    LancelotMaliReadGroup() = default;
    LancelotMaliReadGroup(const LancelotMaliReadGroup &) = delete;
    LancelotMaliReadGroup &operator=(const LancelotMaliReadGroup &) = delete;
};
#endif
