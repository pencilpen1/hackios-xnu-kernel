#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#define LMAL_OK 0
#define LMAL_ETIMEDOUT (-110)
#define LMAL_MAX_WAIT_NS 5000000000ULL
#define kIOMapAnywhere 1
#define MINISURF_TABLE_SIZE 16384
#define kIOReturnSuccess 0
#define kIOReturnNotFound 1
#define kIOReturnBadArgument 2
#define kIOReturnNoMemory 3
using task_t = void *;
using IOReturn = int;

struct IOLock { std::mutex mutex; std::thread::id owner; };
static void IOLockLock(IOLock *p) { p->mutex.lock(); p->owner = std::this_thread::get_id(); }
static void IOLockUnlock(IOLock *p) { assert(p->owner == std::this_thread::get_id()); p->owner = {}; p->mutex.unlock(); }
static bool tryLock(IOLock *p) {
    if (!p->mutex.try_lock()) return false;
    p->owner = std::this_thread::get_id(); return true;
}
// Only the owning thread reads owner, or a successfully acquired mutex does.
static IOLock surfaceLock;
static IOLock *gSurfaceLock = &surfaceLock;
static unsigned allocationFailures;
template<class T> static T *allocate(size_t count) {
    if (allocationFailures) { --allocationFailures; return nullptr; }
    return static_cast<T *>(calloc(count, sizeof(T)));
}
#define IONew(type, count) allocate<type>(count)
#define IODelete(ptr, type, count) free(ptr)
#define OSSafeReleaseNULL(ptr) do { if (ptr) { (ptr)->release(); (ptr) = nullptr; } } while (0)
#define IOLog(...) ((void)0)

struct Lease {
    IOLock gate;
    IOLock *fGate = &gate;
    unsigned fGpuCount = 0, writers = 0;
    int fError = 0;
    std::function<bool()> wait;
    uint64_t deadline(uint64_t ns) { return ns; }
    bool sleepUntil(void *, uint64_t) {
        IOLockUnlock(fGate);
        assert(wait);
        bool progress = wait();
        IOLockLock(fGate);
        return progress;
    }
    void endUserCpu(task_t, bool writer) { assert(writer && writers); --writers; }
#include "actual-mapping-gate.inc"
};
struct IOMemoryMap {
    static std::atomic<unsigned> live;
    IOMemoryMap() { ++live; }
    void release() { assert(live); --live; delete this; }
};
std::atomic<unsigned> IOMemoryMap::live{0};
struct Memory {
    Lease *lease = nullptr;
    bool fail = false;
    unsigned calls = 0;
    std::function<void()> beforeMap;
    IOMemoryMap *createMappingInTask(task_t, unsigned, unsigned) {
        ++calls;
        if (lease) {
            assert(lease->gate.owner == std::this_thread::get_id());
            assert(!lease->fGpuCount);
        }
        if (beforeMap) { auto action = std::move(beforeMap); beforeMap = {}; action(); }
        return fail ? nullptr : new IOMemoryMap;
    }
};
struct MiniIOSurface {
    uint32_t sid, holders = 1, useCount[2]{};
    struct { uint32_t allocSize = 4096; } info;
    struct Shared { uint64_t useCount = 0; } state;
    Lease *maliCpuLease = nullptr;
    Memory storage, pixels;
    Memory *memory = &storage, *client = nullptr;
    task_t clientTask = nullptr;
    bool freed = false;
    explicit MiniIOSurface(uint32_t number) : sid(number) {}
    void release() { assert(!holders && !freed); freed = true; }
    Shared *shared() { return &state; }
    void setLease(Lease *value) { maliCpuLease = value; storage.lease = value; }
};
static MiniIOSurface *gSurfaceTable[MINISURF_TABLE_SIZE];
static uint32_t gSurfaceCount;
static uint64_t gSurfaceBytes;
#include "actual-surface-holds.inc"
static void registerSurface(MiniIOSurface &s) {
    assert(!gSurfaceTable[s.sid]);
    gSurfaceTable[s.sid] = &s; ++gSurfaceCount; gSurfaceBytes += s.info.allocSize;
}
struct minisurf_client_ref {
    MiniIOSurface *surface;
    IOMemoryMap *map, *pixelMap;
    uint32_t use[2], maliReads, maliWrites;
    uint64_t maliGeneration;
    bool maliPending, detached;
};
struct IOExternalMethodArguments {
    unsigned scalarInputCount = 1;
    uint64_t scalarInput[1]{};
    uint64_t generation = 0;
};
struct IOSurfaceRootUserClient {
    task_t fTask = this;
    minisurf_client_ref *fRefs = nullptr;
    uint32_t fRefCount = 0, fRefCapacity = 0;
    uint64_t fNextRefGeneration = 0;
    bool fClosing = false;
    std::atomic<unsigned> refs{1};
    void retain() { assert(refs); ++refs; }
    void release() { assert(refs); --refs; }
    minisurf_client_ref *findRef(uint32_t, bool);
#ifdef BEFORE_MAPPING
    minisurf_client_ref *addRef(MiniIOSurface *);
#else
    minisurf_client_ref *addRef(MiniIOSurface *, bool *known = nullptr);
#endif
    void dropRef(minisurf_client_ref *);
    IOReturn methodLookup(IOExternalMethodArguments *);
    IOReturn returnSurface(minisurf_client_ref *ref, bool, IOExternalMethodArguments *args) {
        args->generation = ref->maliGeneration; return kIOReturnSuccess;
    }
    void closeLocked() {
        fClosing = true;
        while (fRefCount) dropRef(&fRefs[fRefCount - 1]);
    }
    ~IOSurfaceRootUserClient() {
        IOLockLock(gSurfaceLock); closeLocked(); IOLockUnlock(gSurfaceLock);
        free(fRefs);
    }
};
#include "actual-surface-ref.inc"
static IOReturn lookup(IOSurfaceRootUserClient &c, MiniIOSurface &s, uint64_t *gen = nullptr) {
    IOExternalMethodArguments args;
    args.scalarInput[0] = s.sid;
    IOReturn r = c.methodLookup(&args);
    if (gen) *gen = args.generation;
    return r;
}
static void cleanupSurface(MiniIOSurface &s) {
    IOLockLock(gSurfaceLock); surfaceUnhold(&s); IOLockUnlock(gSurfaceLock);
    assert(s.freed);
}
// Reproduce the global-lock edge of the two-root cycle with actual addRef,
// lockMapping and dropRef. GPU completion is controlled here, not emulated DMA.
static void mappingWriterCycle() {
    MiniIOSurface a(1), b(2); Lease first, second;
    a.setLease(&first); b.setLease(&second);
    registerSurface(a); registerSurface(b);
    {
        IOSurfaceRootUserClient mapper, writer;
        assert(lookup(writer, b) == 0);
        IOLockLock(gSurfaceLock);
        writer.fRefs[0].maliWrites = second.writers = 1;
        IOLockUnlock(gSurfaceLock);
        first.fGpuCount = 1;
        std::mutex control;
        std::condition_variable changed;
        bool waiting = false, resolved = false, unlocked = false;
        first.wait = [&] {
            std::unique_lock<std::mutex> lock(control);
            waiting = true; changed.notify_all();
            changed.wait(lock, [&] { return resolved; });
            return unlocked;
        };
        IOReturn result = -1;
        std::thread mapping([&] { result = lookup(mapper, a); });
        { std::unique_lock<std::mutex> lock(control); changed.wait(lock, [&] { return waiting; }); }
        unlocked = tryLock(gSurfaceLock);
        if (unlocked) {
            writer.dropRef(&writer.fRefs[0]);
            assert(!second.writers);
            IOLockUnlock(gSurfaceLock);
            // The second root is now available; queued work can retire first.
            IOLockLock(first.fGate); first.fGpuCount = 0; IOLockUnlock(first.fGate);
        }
        { std::lock_guard<std::mutex> lock(control); resolved = true; changed.notify_all(); }
        mapping.join();
#ifdef BEFORE_MAPPING
        assert(!unlocked && result == kIOReturnNoMemory && second.writers == 1);
        assert(a.storage.calls == 0);
        puts("PASS G2: mapping holds table lock, prevents CPU unlock and times out");
#else
        assert(unlocked && result == 0 && !second.writers);
        assert(a.storage.calls == 1 && mapper.fRefCount == 1);
        puts("PASS G3: CPU unlock progresses during GPU mapping wait");
#endif
    }
    cleanupSurface(a); cleanupSurface(b);
}

#ifndef BEFORE_MAPPING
static void lifecycleCases() {
    // Close and release the caller's ownership during the wait. The actual
    // methodLookup retain must keep the client alive until it reacquires lock.
    {
        MiniIOSurface s(3); Lease lease; s.setLease(&lease); registerSurface(s);
        IOSurfaceRootUserClient c;
        lease.fGpuCount = 1;
        lease.wait = [&] {
            IOLockLock(gSurfaceLock);
            assert(c.refs == 2 && s.holders == 2);
            c.closeLocked(); c.release(); surfaceUnhold(&s);
            assert(c.refs == 1 && !s.freed);
            IOLockUnlock(gSurfaceLock);
            IOLockLock(lease.fGate); lease.fGpuCount = 0; IOLockUnlock(lease.fGate);
            return true;
        };
        assert(lookup(c, s) == kIOReturnNoMemory);
        assert(c.refs == 0 && c.fRefCount == 0 && s.freed && !IOMemoryMap::live);
    }
    // While one mapping is in flight, publish enough unrelated refs to move
    // fRefs twice, then publish the same surface; keep its CPU counts/generation.
    {
        MiniIOSurface s(4); registerSurface(s);
        std::vector<MiniIOSurface *> peers;
        uint64_t generation = 0;
        {
            IOSurfaceRootUserClient c;
            s.storage.beforeMap = [&] {
                for (unsigned i = 0; i < 40; ++i) {
                    auto *p = new MiniIOSurface(100 + i); peers.push_back(p); registerSurface(*p);
                    assert(lookup(c, *p) == 0);
                }
                assert(lookup(c, s, &generation) == 0);
                IOLockLock(gSurfaceLock); c.findRef(s.sid, false)->use[0] = s.useCount[0] = 7; IOLockUnlock(gSurfaceLock);
            };
            uint64_t actual = 0;
            assert(lookup(c, s, &actual) == 0 && actual == generation);
            assert(c.fRefCount == 41 && c.fNextRefGeneration == 41);
            assert(c.findRef(s.sid, false)->use[0] == 7 && s.holders == 2);
            assert(IOMemoryMap::live == 41);
        }
        cleanupSurface(s);
        for (auto *p : peers) { cleanupSurface(*p); delete p; }
    }
    // A detached predecessor must remain independently releasable. Reuse the
    // current generation published while this addRef prepared a second map.
    {
        MiniIOSurface s(5); registerSurface(s);
        {
            IOSurfaceRootUserClient c;
            s.storage.beforeMap = [&] {
                assert(lookup(c, s) == 0);
                IOLockLock(gSurfaceLock); c.findRef(s.sid, false)->detached = true; IOLockUnlock(gSurfaceLock);
                assert(lookup(c, s) == 0);
            };
            uint64_t actual = 0;
            assert(lookup(c, s, &actual) == 0 && actual == 2);
            assert(c.fRefCount == 2 && c.findRef(s.sid, true)->maliGeneration == 1);
            assert(IOMemoryMap::live == 2 && s.holders == 3);
        }
        cleanupSurface(s);
    }
    // Publication generation exhaustion after dropping the lock, allocation
    // failure, both map failure positions, and lease timeout/error all unwind.
    for (unsigned mode = 0; mode < 6; ++mode) {
        MiniIOSurface s(6); Lease lease; registerSurface(s);
        {
            IOSurfaceRootUserClient c;
            if (mode == 0) s.storage.beforeMap = [&] {
                IOLockLock(gSurfaceLock); c.fNextRefGeneration = UINT64_MAX; IOLockUnlock(gSurfaceLock);
            };
            if (mode == 1) allocationFailures = 1;
            if (mode == 2) s.storage.fail = true;
            if (mode == 3) { s.client = &s.pixels; s.pixels.fail = true; }
            if (mode == 4) { s.setLease(&lease); lease.fGpuCount = 1; lease.wait = [] { return false; }; }
            if (mode == 5) { s.setLease(&lease); lease.fError = -5; }
            assert(lookup(c, s) == kIOReturnNoMemory);
            assert(!c.fRefCount && s.holders == 1 && !IOMemoryMap::live && !allocationFailures);
        }
        cleanupSurface(s);
    }
    // Successful borrowed-pixel mapping publishes both maps, and a concurrent
    // duplicate reports known=true for the Mach-port returnSurface contract.
    {
        MiniIOSurface s(7); s.client = &s.pixels; registerSurface(s);
        {
            IOSurfaceRootUserClient c;
            s.storage.beforeMap = [&] { assert(lookup(c, s) == 0); };
            bool known = false;
            c.retain(); IOLockLock(gSurfaceLock);
            auto *ref = c.addRef(&s, &known);
            assert(ref && known && ref->map && ref->pixelMap && c.fRefCount == 1);
            IOLockUnlock(gSurfaceLock); c.release();
            assert(IOMemoryMap::live == 2 && s.holders == 2);
        }
        cleanupSurface(s);
    }
    puts("PASS G3: close, array growth, duplicate/detach generations, failure cleanup and borrowed maps");
}
#endif
int main() {
    mappingWriterCycle();
#ifndef BEFORE_MAPPING
    lifecycleCases();
#endif
    assert(!IOMemoryMap::live && !gSurfaceCount && !gSurfaceBytes);
}
