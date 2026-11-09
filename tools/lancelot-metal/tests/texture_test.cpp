#include "rwm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct IOSurface { unsigned char data[4096];size_t width,height,pitch,bytes;bool bad_lock;unsigned references; };
struct rwa_texture {};
static struct rwm_air air;
static unsigned reads,writes;
extern "C" const struct rwm_air *rwm_air(void){return &air;}
extern "C" void rwm_print(const char*,...){}
extern "C" void rwm_worker_drain(const char*){}
extern "C" const void *CFRetain(const void*s){auto*p=(IOSurface*)s;p->references++;return s;}
extern "C" void CFRelease(const void*s){auto*p=(IOSurface*)s;assert(p->references);p->references--;}
extern "C" int IOSurfaceLock(IOSurfaceRef s,uint32_t,uint32_t*){return s->bad_lock?-1:0;}
extern "C" int IOSurfaceUnlock(IOSurfaceRef,uint32_t,uint32_t*){return 0;}
extern "C" size_t IOSurfaceGetPlaneCount(IOSurfaceRef){return 0;}
extern "C" void *IOSurfaceGetBaseAddress(IOSurfaceRef s){return s->data;}
extern "C" size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s){return s->pitch;}
extern "C" size_t IOSurfaceGetWidth(IOSurfaceRef s){return s->width;}
extern "C" size_t IOSurfaceGetHeight(IOSurfaceRef s){return s->height;}
extern "C" size_t IOSurfaceGetBytesPerElement(IOSurfaceRef){return 4;}
extern "C" size_t IOSurfaceGetAllocSize(IOSurfaceRef s){return s->bytes;}
extern "C" void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,size_t){return s->data;}
extern "C" size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s,size_t){return s->pitch;}
extern "C" size_t IOSurfaceGetWidthOfPlane(IOSurfaceRef s,size_t){return s->width;}
extern "C" size_t IOSurfaceGetHeightOfPlane(IOSurfaceRef s,size_t){return s->height;}
extern "C" size_t IOSurfaceGetBytesPerElementOfPlane(IOSurfaceRef,size_t){return 4;}
extern "C" uint32_t IOSurfaceGetSeed(IOSurfaceRef){return 7;}
static void destroy(rwa_texture*t){delete t;}
static int read(rwa_texture*,unsigned,unsigned,const unsigned[3],const unsigned size[3],void*out,size_t row,char*,size_t){
    reads++;memset(out,0x66,row*size[1]);return 0;
}
static int write(rwa_texture*,unsigned,unsigned,const unsigned[3],const unsigned size[3],const void*,size_t row,char*,size_t){
    writes++;assert(size[0]==16&&size[1]==16&&row==64);return 0;
}
int main(void){
    air.texture_destroy=destroy;air.texture_read=read;air.texture_write=write;
    rwa_texture_shape shape={80,2,16,16,1,1,1,1,5};char why[256]={};size_t done=0;
    auto*t=rwm_texture_new(new rwa_texture,&shape);assert(t);
    assert(!rwm_texture_sync_surface(t,why,sizeof(why))&&!reads);
    IOSurface s{};s.width=s.height=16;s.pitch=64;s.bytes=1024;
    rwm_texture_copy_back_to(t,&s);assert(s.references==1);
    assert(!rwm_texture_sync_surface(t,why,sizeof(why))&&reads==1);
    for(unsigned i=0;i<1024;i++)assert(s.data[i]==0x66);
    memset(s.data,0,4096);s.height=8;
    assert(rwm_texture_sync_surface(t,why,sizeof(why))==-1);
    for(unsigned i=0;i<4096;i++)assert(!s.data[i]);
    s.height=16;s.bytes=1023;assert(rwm_texture_sync_surface(t,why,sizeof(why))==-1);
    s.bytes=1024;s.bad_lock=true;assert(rwm_texture_sync_surface(t,why,sizeof(why))==-1);
    s.bad_lock=false;air.texture_read=nullptr;assert(rwm_texture_sync_surface(t,why,sizeof(why))==-1);
    air.texture_read=read;rwm_texture_copy_in_from(t,&s,false);
    s.bad_lock=true;assert(rwm_texture_copy_in(t,&done,why,sizeof(why))==-1&&!writes);
    s.bad_lock=false;assert(!rwm_texture_copy_in(t,&done,why,sizeof(why))&&writes==1&&done==1024);
    rwm_texture_free(t);assert(!s.references);
    puts("PASS actual IOSurface copies: full frame only, short geometry/allocation/failed lock/missing reader are errors; source and retain lifetime verified");
}
