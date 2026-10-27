/* Executes the actual production Export.h against narrow host API doubles.
 * VM/RPC machinery is NOT emulated: kernel runtime validation remains pending. */
#include <cstdio>
#include "../../xnu/files/iokit/Kernel/LancelotMaliExport.h"
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while(0)
static IOMemoryMap *map(LancelotMaliExport *e, IOOptionBits options=0,
    task_t task=(void*)1, uint64_t address=0x1000) {
    auto *candidate=new IOMemoryMap;
    candidate->address=address;
    return e->makeMapping(e,task,(IOVirtualAddress)candidate,options,0,0);
}
int main() {
    auto *parent=new IOMemoryDescriptor;
    auto *e=LancelotMaliExport::withBacking(parent,(void*)1);
    CHECK(e && parent->retainCount()==2 && host_task_refs==1);
    auto *m=map(e);
    CHECK(m && e->mapCount()==1 && m->references()==2);
    CHECK(parent->last_options&kIOMapUnique); /* no supermap can be shared */
    m->retain(); m->release(); /* IOKit transport owns one reference */
    auto *lookup=map(e,kIOMapReference);
    CHECK(lookup==m && e->mapCount()==1 && parent->creates==1 && parent->references==1);
    CHECK(!(parent->last_options&kIOMapUnique)); /* unmap must FIND, not create */
    m->release(); lookup->release(); /* transport Unmap drops only transport refs */
    CHECK(e->mapCount()==1 && m->references()==1 && m->unmaps==0);
    CHECK(e->revoke(false)==LMAL_OK && e->mapCount()==0 && IOMemoryMap::live==0);
    CHECK(e->revoke(false)==LMAL_OK); /* no fake decrement/negative refs */
    m=map(e);
    CHECK(m && e->mapCount()==1);
    m->release();
    CHECK(e->revoke(true)==LMAL_OK && IOMemoryMap::live==0);
    /* Descriptor escaped before Close cannot create ANY mapping afterward. */
    e->retain(); e->release();
    CHECK(map(e)==nullptr && map(e,kIOMapReference)==nullptr && IOMemoryMap::live==0);
    e->release(); CHECK(parent->retainCount()==1 && host_task_refs==0); parent->release();

    parent=new IOMemoryDescriptor;
    e=LancelotMaliExport::withBacking(parent,(void*)1);
    CHECK(map(e,0,(void*)2)==nullptr && IOMemoryMap::live==0); /* cross-task target */
    host_current_task=(void*)2;
    CHECK(map(e)==nullptr && IOMemoryMap::live==0); /* transferred connection caller */
    host_current_task=(void*)1;
    IOMemoryMap *transport[LMAL_CLIENT_EXPORT_MAPS];
    for(unsigned i=0;i<LMAL_CLIENT_EXPORT_MAPS;++i) {
        transport[i]=map(e,0,(void*)1,0x1000+i*4096);
        CHECK(transport[i]!=nullptr);
    }
    CHECK(e->mapCount()==LMAL_CLIENT_EXPORT_MAPS);
    CHECK(map(e,0,(void*)1,0x900000)==nullptr && IOMemoryMap::live==LMAL_CLIENT_EXPORT_MAPS);
    lookup=map(e,kIOMapReference,(void*)1,0x1000);
    CHECK(lookup==transport[0] && e->mapCount()==LMAL_CLIENT_EXPORT_MAPS);
    lookup->release();
    CHECK(e->revoke(true)==LMAL_OK && e->mapCount()==0);
    for(unsigned i=0;i<LMAL_CLIENT_EXPORT_MAPS;++i) {
        CHECK(transport[i]->getAddress()==0 && transport[i]->unmaps==1);
        transport[i]->release();
    }
    CHECK(IOMemoryMap::live==0);
    e->release(); parent->release(); CHECK(host_task_refs==0);
    for(uint64_t kernel_page : {UINT64_C(4096),UINT64_C(16384)}) {
        parent=new IOMemoryDescriptor;
        parent->length=lmal_client_round_backing(4097,kernel_page,LMAL_MAX_BO_BYTES);
        e=LancelotMaliExport::withBacking(parent,(void*)1);
        CHECK(e && e->getLength()==parent->getLength());
        CHECK(e->getLength()%kernel_page==0);
        CHECK(lmal_client_gpu_page_count(e->getLength(),32768)==e->getLength()/4096);
        CHECK(e->revoke(true)==LMAL_OK);
        e->release(); parent->release(); CHECK(host_task_refs==0);
    }
    std::printf("PASS production Export.h: %u checks (unique/reference, exact retains, transport-Unmap/ACK, close escape, task restriction, 16-map bound, kernel4K/16K export lengths)\n",checks);
}
