/* Host API doubles for executing production Export.h, NOT an XNU VM emulator. */
#ifndef LMAL_HOST_IOKIT_H
#define LMAL_HOST_IOKIT_H
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>
using IOByteCount=uint64_t;
using IOVirtualAddress=uintptr_t;
using IOOptionBits=uint32_t;
using IODirection=uint32_t;
using IOReturn=int;
using task_t=void*;
constexpr task_t TASK_NULL=nullptr;
constexpr int kIOReturnSuccess=0, kIOReturnError=-1;
constexpr IOOptionBits kIODirectionInOut=3, kIOMapReference=0x02000000, kIOMapUnique=0x04000000;
inline thread_local task_t host_current_task=(void*)1;
inline unsigned host_task_refs;
inline task_t current_task() { return host_current_task; }
inline void task_reference(task_t) { ++host_task_refs; }
inline void task_deallocate(task_t) { --host_task_refs; }
using IOLock=std::mutex;
inline IOLock *IOLockAlloc() { return new IOLock; }
inline void IOLockFree(IOLock *l) { delete l; }
inline void IOLockLock(IOLock *l) { l->lock(); }
inline void IOLockUnlock(IOLock *l) { l->unlock(); }
#define OSDeclareDefaultStructors(T)
#define OSDefineMetaClassAndStructors(T,P)
#define OSTypeAlloc(T) new T
#define APPLE_KEXT_OVERRIDE override
#include <strings.h>
class IOMemoryDescriptor;
class IOMemoryMap {
    unsigned refs=1;
public:
    uint64_t address=0x1000;
    IOOptionBits options=0;
    bool fail=false;
    unsigned unmaps=0;
    IOMemoryDescriptor *parent=nullptr;
    inline static unsigned live=0;
    IOMemoryMap() { ++live; }
    virtual ~IOMemoryMap();
    void retain() { ++refs; }
    void release() { assert(refs); if(!--refs) delete this; }
    unsigned references() const { return refs; }
    virtual IOReturn unmap() { ++unmaps; address=0; return fail?kIOReturnError:kIOReturnSuccess; }
    uint64_t getAddress() const { return address; }
};
class IOMemoryDescriptor {
    unsigned refs=1;
public:
    std::vector<IOMemoryMap*> maps;
    IOOptionBits last_options=0;
    unsigned creates=0, references=0;
    uint64_t length=4096;
    virtual ~IOMemoryDescriptor() { assert(maps.empty()); }
    virtual void free() {}
    void retain() { ++refs; }
    void release() { assert(refs); if(!--refs) { free(); delete this; } }
    unsigned retainCount() const { return refs; }
    uint64_t getLength() const { return length; }
    virtual IOMemoryMap *makeMapping(IOMemoryDescriptor*,task_t,IOVirtualAddress addr,
        IOOptionBits opts,IOByteCount,IOByteCount) {
        auto *candidate=(IOMemoryMap*)addr;
        last_options=opts;
        if(opts&kIOMapReference) {
            ++references;
            auto it=std::find_if(maps.begin(),maps.end(),[&](auto *m){return m->address==candidate->address;});
            candidate->release();
            if(it==maps.end()) return nullptr;
            (*it)->retain(); return *it;
        }
        ++creates;
        assert(opts&kIOMapUnique); /* production must forbid super-map sharing */
        candidate->options=opts;
        candidate->parent=this;
        maps.push_back(candidate);
        return candidate;
    }
};
inline IOMemoryMap::~IOMemoryMap() {
    if(parent) {
        auto &v=parent->maps;
        v.erase(std::find(v.begin(),v.end(),this));
    }
    --live;
}
class IOSubMemoryDescriptor:public IOMemoryDescriptor {
protected:
    IOMemoryDescriptor *_parent=nullptr;
public:
    bool initSubRange(IOMemoryDescriptor *p,IOByteCount,IOByteCount bytes,IODirection) {
        _parent=p; length=bytes; p->retain(); return true;
    }
    void free() override { if(_parent) _parent->release(); }
    IOMemoryMap *makeMapping(IOMemoryDescriptor *owner,task_t task,IOVirtualAddress addr,
        IOOptionBits opts,IOByteCount off,IOByteCount len) override {
        return _parent->makeMapping(owner,task,addr,opts,off,len);
    }
};
#endif
