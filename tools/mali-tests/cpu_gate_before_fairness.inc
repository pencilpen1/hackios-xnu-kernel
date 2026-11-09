bool cpuConflict(task_t own, bool legacy) const {
        uint32_t reads = fUserReads, writes = fUserWrites;
        if (legacy)
            for (uint32_t i = 0; i < SLOT_COUNT; ++i) if (fUsers[i].task == own) {
                reads -= fUsers[i].reads; writes -= fUsers[i].writes;
            }
        return fReaders || fWriters || reads || writes;
    }
int waitCpu(uint64_t remaining_ns) {
        IOLockLock(fGate);
        uint64_t end = deadline(remaining_ns);
        while (fGpuCount && !fError)
            if (!sleepUntil(&fGpuCount, end)) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
        int ret = fError ? fError : fQuarantined ? LMAL_EIO : fWriteError;
        IOLockUnlock(fGate); return ret;
    }
int beginCpuRead(uint64_t remaining_ns) {
        IOLockLock(fGate);
        uint64_t end = deadline(remaining_ns);
        while ((fGpuCount || fWriters || fUserWrites) && !fError)
            if (!sleepUntil(&fGpuCount, end)) { IOLockUnlock(fGate); return LMAL_ETIMEDOUT; }
        if (fError || fWriteError || fQuarantined || fReaders == UINT32_MAX) {
            int e = fError ? fError : fWriteError ? fWriteError : LMAL_EBUSY; IOLockUnlock(fGate); return e;
        }
        ++fReaders; retain();
        IOLockUnlock(fGate); return LMAL_OK;
    }
void endCpuRead() {
        IOLockLock(fGate);
        bool drop = fReaders != 0;
        if (drop) --fReaders;
        IOLockWakeup(fGate, &fReaders, false);
        IOLockUnlock(fGate);
        if (drop) release();
    }
