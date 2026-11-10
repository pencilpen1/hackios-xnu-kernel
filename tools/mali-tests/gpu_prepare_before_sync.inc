/* W7 production entry, retained to reproduce first-clean publication order. */
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
        int turn = awaitGpuTurn(own, !requireBlock, end, waitCpu);
        if (turn) { IOLockUnlock(fGate); return turn; }
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
