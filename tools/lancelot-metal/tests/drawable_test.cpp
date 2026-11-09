#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "drawable.h"
#include "lancelot_mali_uapi.h"
#include "lancelot_mali_queue_uapi.h"
#include "air_metal.h"
static bool no_service,short_reply,oversized_reply,call_failure;
static unsigned opens,describes,takes,closes,imports;
extern "C" void *IOServiceMatching(const char*name){assert(!strcmp(name,LMAL_SERVICE_NAME));return (void*)1;}
extern "C" io_service_t IOServiceGetMatchingService(unsigned,void*){return no_service?0:7;}
extern "C" kern_return_t IOServiceOpen(io_service_t s,unsigned task,unsigned type,io_connect_t*out){assert(s==7&&task==99&&type==LMAL_OPEN_TYPE);opens++;*out=8;return 0;}
extern "C" kern_return_t IOServiceClose(io_connect_t c){assert(c==8);return 0;}
extern "C" kern_return_t IOObjectRelease(unsigned){return 0;}
extern "C" unsigned mach_task_self(void){return 99;}
extern "C" kern_return_t IOConnectCallStructMethod(io_connect_t c,unsigned selector,const void*in,size_t n,void*out,size_t*size){
    assert(c==8);
    if(call_failure)return kIOReturnUnsupported;
    if(selector==LMQ_SURFACE){
        assert(n==sizeof(lmq_surface)&&out&&size&&*size==sizeof(lmq_surface));
        const lmq_surface*q=(const lmq_surface*)in;assert(q->surface==5);
        lmq_surface reply={5,0,4096,0,64,16,16,0x42475241,0};
        if(q->handle==LMQ_DESCRIBE)describes++;else{takes++;reply.handle=3;}
        memcpy(out,&reply,n);*size=short_reply?n-1:oversized_reply?n+1:n;return 0;
    }
    assert(selector==LMQ_CLOSE&&n==sizeof(lmq_close)&&out&&size&&*size==n);
    assert(((const lmq_close*)in)->handle==3);closes++;memcpy(out,in,n);return 0;
}
static uint32_t get_id(void*surface){assert(surface==(void*)5);return 5;}
static rwa_texture *import_texture(rwa_device*device,const rwd_texture_shape*shape,int id,unsigned offset,unsigned pitch,char*,size_t){
    assert(device==(rwa_device*)1&&id==5&&shape->pixel_format==80&&offset==0&&pitch==64);
    imports++;return (rwa_texture*)9;
}
extern "C" void *lancelot_model_dlopen(const char*path,int){assert(!strcmp(path,"/redwood/lib/libredwoodair.dylib"));return (void*)3;}
extern "C" void *lancelot_model_dlsym(void*,const char*name){
    if(!strcmp(name,"IOSurfaceGetID"))return (void*)get_id;
    assert(!strcmp(name,"rwa_texture_from_surface"));return (void*)import_texture;
}
int main(void){
    char why[512]={};io_connect_t c=0;
    assert(rwd_gpu_open(nullptr,why,sizeof(why))==-1&&!opens);
    no_service=true;assert(rwd_gpu_open(&c,why,sizeof(why))==-1&&!c);no_service=false;
    assert(!rwd_gpu_open(&c,why,sizeof(why))&&c==8&&opens==1);
    rwd_surface s{};assert(!rwd_surface_describe(c,5,&s,why,sizeof(why))&&!takes&&describes==1);
    assert(!rwd_surface_fits_adreno(&s,4,why,sizeof(why)));
    s.size=4096;s.offset=UINT32_MAX;s.rowBytes=0xffffffc0;s.height=UINT32_MAX;
    assert(rwd_surface_fits_adreno(&s,4,why,sizeof(why))==-1);
    short_reply=true;assert(rwd_surface_describe(c,5,&s,why,sizeof(why))==-1);short_reply=false;
    oversized_reply=true;assert(rwd_surface_describe(c,5,&s,why,sizeof(why))==-1);oversized_reply=false;
    assert(!rwd_surface_take(c,5,&s,why,sizeof(why))&&s.handle==3&&takes==1);
    assert(!rwd_surface_give_back(c,3,why,sizeof(why))&&closes==1);
    call_failure=true;assert(rwd_surface_describe(c,5,&s,why,sizeof(why))==-1);call_failure=false;
    rwd_texture_shape shape={80,2,16,16,1,1,1,1,5};
    assert(rwd_texture_for_surface((rwa_device*)1,(void*)5,0,&shape,&s,why,sizeof(why))==(rwa_texture*)9);
    assert(imports==1&&takes==1); /* metadata query must not allocate a second buffer */
    shape.width=17;assert(!rwd_texture_for_surface((rwa_device*)1,(void*)5,0,&shape,&s,why,sizeof(why))&&imports==1);
    rwd_gpu_close(c);
    puts("PASS actual Mali drawable bridge: native service/open/selector/size, no duplicate allocation, importer and bounds");
}
