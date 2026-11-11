#include "rwm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
struct IOSurface { unsigned char data[4096];size_t width,height,pitch,bytes;bool bad_lock;unsigned references; };
struct rwa_texture {};
static struct rwm_air air;
static unsigned reads,writes;
static unsigned plane_count;
extern "C" const struct rwm_air *rwm_air(void){return &air;}
extern "C" void rwm_print(const char*,...){}
extern "C" void rwm_worker_drain(const char*){}
extern "C" const void *CFRetain(const void*s){auto*p=(IOSurface*)s;p->references++;return s;}
extern "C" void CFRelease(const void*s){auto*p=(IOSurface*)s;assert(p->references);p->references--;}
extern "C" int IOSurfaceLock(IOSurfaceRef s,uint32_t,uint32_t*){return s->bad_lock?-1:0;}
extern "C" int IOSurfaceUnlock(IOSurfaceRef,uint32_t,uint32_t*){return 0;}
extern "C" size_t IOSurfaceGetPlaneCount(IOSurfaceRef){return plane_count;}
extern "C" uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef){return 0x42475241;}
extern "C" uint32_t IOSurfaceGetID(IOSurfaceRef){return 17;}
extern "C" const char *getprogname(){return "metal-test";}
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
static rwa_texture *create(rwa_device*,const rwa_texture_shape*,char*,size_t){return new rwa_texture;}
static rwa_texture *import_surface(void *context,rwa_device*,IOSurfaceRef,unsigned,const rwa_texture_shape*,
    rwm_import *road,char*,size_t){if(!context)return nullptr;road->surface=17;return new rwa_texture;}
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
    air.texture_create=create;
    auto *device=reinterpret_cast<rwa_device*>(1);rwm_import road;
    for(unsigned usage: {0u,1u,2u,3u,4u,5u,6u,7u}) {
        shape.usage=usage;bool writable=usage==0||(usage&6);unsigned oldreads=reads,oldwrites=writes;
        auto *imported=rwm_surface_import(device,&s,0,&shape,import_surface,nullptr,&road,why,sizeof(why));assert(imported);
        assert(road.kind==(writable?RWM_IMPORT_COPY_BACK:RWM_IMPORT_COPY_IN));
        assert(!rwm_texture_sync_surface(imported,why,sizeof(why))&&reads==oldreads+(writable?1:0));
        assert(!rwm_texture_copy_in(imported,&done,why,sizeof(why)));
        assert(writes==oldwrites+((usage==0||(usage&1))?1:0));
        rwm_texture_free(imported);assert(!s.references);
    }
    shape.usage=3;
    t=rwm_surface_import(device,&s,0,&shape,import_surface,(void*)1,&road,why,sizeof(why));
    assert(t&&road.kind==RWM_IMPORT_IN_PLACE&&rwm_texture_in_place(t)==17&&!rwm_texture_has_surface(t));
    rwm_texture_free(t);
    plane_count=2;shape.pixel_format=500;
    assert(!rwm_surface_import(device,&s,0,&shape,import_surface,nullptr,&road,why,sizeof(why)));
    assert(!s.references);
    puts("PASS production import: ShaderWrite and unknown usage retain output ownership; read/write retains both directions; in-place import preferred; writable YCbCr rejected");
    puts("PASS actual IOSurface copies: full frame only, short geometry/allocation/failed lock/missing reader are errors; source and retain lifetime verified");
}
