/* Private descriptor export gate for XNU 8796.121.2. Not a hardware API. */
#ifndef LANCELOT_MALI_EXPORT_H
#define LANCELOT_MALI_EXPORT_H
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include "lancelot_mali_client_logic.h"

class LancelotMaliExport : public IOSubMemoryDescriptor {
    OSDeclareDefaultStructors(LancelotMaliExport);
    IOLock *fGate;
    task_t fOwner;
    struct lmal_client_export fExport;
    static int force_unmap(void *object) {
        return ((IOMemoryMap *)object)->unmap() == kIOReturnSuccess ? LMAL_OK : LMAL_EIO;
    }
    static void drop_map(void *object) { ((IOMemoryMap *)object)->release(); }
public:
    static LancelotMaliExport *withBacking(IOMemoryDescriptor *parent, task_t owner) {
        LancelotMaliExport *e = OSTypeAlloc(LancelotMaliExport);
        if (!e) return NULL;
        e->fGate = IOLockAlloc();
        e->fOwner = TASK_NULL;
        bzero(&e->fExport, sizeof(e->fExport));
        if (!e->fGate || !e->initSubRange(parent, 0, parent->getLength(), kIODirectionInOut)) {
            e->release();
            return NULL;
        }
        task_reference(owner);
        e->fOwner = owner;
        return e;
    }
    /* Gate lock -> XNU recursive memory lock. Never gate lock -> client lock. */
    virtual IOMemoryMap *makeMapping(IOMemoryDescriptor *owner, task_t task,
        IOVirtualAddress address, IOOptionBits options, IOByteCount offset,
        IOByteCount length) APPLE_KEXT_OVERRIDE {
        IOLockLock(fGate);
        bool reference = (options & kIOMapReference) != 0;
        if (fExport.closed || fExport.poisoned || task != fOwner || current_task() != fOwner ||
            (!reference && fExport.count == LMAL_CLIENT_EXPORT_MAPS)) {
            /* createMappingInTask allocated this candidate; makeMapping owns it. */
            ((IOMemoryMap *)address)->release();
            IOLockUnlock(fGate);
            return NULL;
        }
        /* Unique creation is essential: a shared submap's unmap does not revoke
         * its supermap. Reference-only transport unmap MUST search existing maps. */
        options = reference ? (options & ~kIOMapUnique) : (options | kIOMapUnique);
        IOMemoryMap *map = IOSubMemoryDescriptor::makeMapping(owner, task, address,
            options, offset, length);
        if (map) {
            int inserted = lmal_client_export_track(&fExport, map);
            if (inserted > 0) map->retain();
            else if (inserted < 0) {
                /* Capacity is checked before creation. This is defensive only. */
                if (map->unmap() != kIOReturnSuccess) {
                    fExport.poisoned = fExport.closed = 1;
                    map->retain(); /* uncertain VM teardown: never free backing */
                }
                map->release();
                map = NULL;
            }
        }
        IOLockUnlock(fGate);
        return map;
    }
    uint32_t mapCount() {
        IOLockLock(fGate);
        uint32_t count = fExport.count;
        IOLockUnlock(fGate);
        return count;
    }
    int revoke(bool permanent) {
        IOLockLock(fGate);
        int result = lmal_client_export_revoke(&fExport, permanent, force_unmap, drop_map);
        IOLockUnlock(fGate);
        return result;
    }
    virtual void free() APPLE_KEXT_OVERRIDE {
        /* The client permanently revokes before releasing its owning reference.
         * Failed revocation keeps that reference quarantined, never reaches free. */
        if (fOwner != TASK_NULL) task_deallocate(fOwner);
        if (fGate) IOLockFree(fGate);
        IOSubMemoryDescriptor::free();
    }
};
OSDefineMetaClassAndStructors(LancelotMaliExport, IOSubMemoryDescriptor);
#endif
