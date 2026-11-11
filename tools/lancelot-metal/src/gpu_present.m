/* Preserve the iPhone display/layout ABI. Scale the final full-screen swap on
 * Mali into a retained native-size IOSurface, so the kernel only copies rows.
 * No live scanout page is written by the GPU and no CPU pixel mapping is used. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <libkern/OSCacheControl.h>
#include "present_policy.h"
#include "present_cache_hook.h"

static __thread unsigned inside;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static io_connect_t fb_connection;
static uint32_t panel_width,panel_height,render_width,render_height;
static id<MTLDevice> gpu;
static id<MTLCommandQueue> queue;
static id<MTLComputePipelineState> pipeline;
static struct { IOSurfaceRef surface;id<MTLTexture> texture;id<MTLCommandBuffer> command;BOOL pending; } slots[3];
static unsigned next_slot;
static uint64_t scaled,fallback;
static uint32_t setup_issues;
static BOOL in_place(id texture,IOSurfaceRef surface) {
    SEL query=sel_registerName("lancelotInPlaceSurface");
    return texture && [texture respondsToSelector:query] &&
        ((uint32_t(*)(id,SEL))objc_msgSend)(texture,query)==IOSurfaceGetID(surface);
}
static void release_setup(void) {
    for(unsigned i=0;i<3;i++) {
        [slots[i].texture release];slots[i].texture=nil;
        if(slots[i].surface){CFRelease(slots[i].surface);slots[i].surface=NULL;}
    }
    [pipeline release];pipeline=nil;[queue release];queue=nil;[gpu release];gpu=nil;
    fb_connection=0;
}
static void say_present(const char *reason) {
    char text[256];snprintf(text,sizeof(text),"lancelot-metal-present: pid=%d gpu_scaled=%llu fallback=%llu %s",getpid(),(unsigned long long)scaled,(unsigned long long)fallback,reason);
    sysctlbyname("kern.lancelot_hakios.say",NULL,NULL,text,strlen(text));
}
static void setup_issue(unsigned code,const char *reason) {
    if(code<32 && !(setup_issues&(1u<<code))) {
        setup_issues|=1u<<code;say_present(reason);
    }
}
static uint32_t number(io_service_t service,CFStringRef key) {
    CFTypeRef value=IORegistryEntryCreateCFProperty(service,key,kCFAllocatorDefault,0);int64_t n=0;
    if(value&&CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(value,kCFNumberSInt64Type,&n);
    if(value)CFRelease(value);return n>0&&n<=8192?(uint32_t)n:0;
}
static BOOL setup(io_connect_t connection) {
    if(fb_connection)return connection==fb_connection&&pipeline!=nil;
    io_service_t service=0;
    kern_return_t service_error=IOConnectGetService(connection,&service);
    if(service_error!=KERN_SUCCESS) {
        char why[96];snprintf(why,sizeof(why),"setup: IOConnectGetService failed kr=%x",service_error);
        setup_issue(0,why);return NO;
    }
    BOOL valid=IOObjectConformsTo(service,"IOMobileFramebuffer");
    if(valid) {
        panel_width=number(service,CFSTR("lancelot-panel-width"));panel_height=number(service,CFSTR("lancelot-panel-height"));
        render_width=number(service,CFSTR("lancelot-render-width"));render_height=number(service,CFSTR("lancelot-render-height"));
    }
    IOObjectRelease(service);
    if(!valid||!panel_width||!panel_height||!render_width||!render_height) {
        char why[128];snprintf(why,sizeof(why),"setup: framebuffer=%u panel=%ux%u render=%ux%u",
            valid,panel_width,panel_height,render_width,render_height);
        setup_issue(1,why);return NO;
    }
    fb_connection=connection;
    gpu=MTLCreateSystemDefaultDevice();
    if(!gpu||strcmp(object_getClassName(gpu),"RedwoodMetalDevice")){setup_issue(2,"setup: native Mali Metal device unavailable");release_setup();return NO;}
    queue=[gpu newCommandQueue];NSError *error=nil;
    id<MTLLibrary> library=[gpu newLibraryWithFile:@"/redwood/share/lancelot-effects.metallib" error:&error];
    id<MTLFunction> shader=[library newFunctionWithName:@"lancelot_scale"];
    pipeline=shader?[gpu newComputePipelineStateWithFunction:shader error:&error]:nil;
    [shader release];[library release];
    if(!pipeline||!queue){setup_issue(3,"GPU scaling pipeline unavailable");release_setup();return NO;}
    for(unsigned i=0;i<3;i++) {
        NSUInteger pitch=((NSUInteger)panel_width*4+63)&~63UL;
        NSDictionary *properties=@{(id)kIOSurfaceWidth:@(panel_width),(id)kIOSurfaceHeight:@(panel_height),
            (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@(pitch),
            (id)kIOSurfaceAllocSize:@(pitch*panel_height),(id)kIOSurfacePixelFormat:@(0x42475241u)};
        slots[i].surface=IOSurfaceCreate((CFDictionaryRef)properties);
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:panel_width height:panel_height mipmapped:NO];
        d.usage=MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        slots[i].texture=slots[i].surface?[gpu newTextureWithDescriptor:d iosurface:slots[i].surface plane:0]:nil;
        if(!in_place(slots[i].texture,slots[i].surface)){setup_issue(4,"native in-place scaling surface unavailable");release_setup();return NO;}
    }
    say_present("ready: GPU bilinear scale, native IOSurfaces, CPU row copy only");return YES;
}
static BOOL scale_swap(io_connect_t connection,const void *input,size_t size,uint8_t *output,unsigned *reserved) {
    if(strcmp(getprogname(),"backboardd")||!input||size!=LMP_SWAP_BYTES)return NO;
    BOOL changed=NO;pthread_mutex_lock(&gate);
    @autoreleasepool {
        if(!setup(connection)||!lmp_full_frame(input,size,render_width,render_height))goto done;
        unsigned chosen=3;
        for(unsigned k=0;k<3;k++) {
            unsigned i=(next_slot+k)%3;
            if(slots[i].pending)continue;
            if(slots[i].command && [slots[i].command status]<MTLCommandBufferStatusCompleted)continue;
            if(IOSurfaceIsInUse(slots[i].surface))continue;
            chosen=i;break;
        }
        if(chosen==3)goto done;
        IOSurfaceRef source=IOSurfaceLookup(lmp_word(input,0x54));
        if(!source)goto done;
        uint32_t format=IOSurfaceGetPixelFormat(source);
        if(IOSurfaceGetPlaneCount(source)||IOSurfaceGetWidth(source)!=render_width||IOSurfaceGetHeight(source)!=render_height||
           (format!=0x42475241u&&format!=0x52474241u)){CFRelease(source);goto done;}
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format==0x42475241u?MTLPixelFormatBGRA8Unorm:MTLPixelFormatRGBA8Unorm width:render_width height:render_height mipmapped:NO];
        d.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[gpu newTextureWithDescriptor:d iosurface:source plane:0];
        BOOL direct=in_place(texture,source);
        CFRelease(source);
        if(!direct){[texture release];goto done;}
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!encoder){[texture release];goto done;}
        uint32_t dims[]={render_width,render_height,panel_width,panel_height};
        [encoder setComputePipelineState:pipeline];[encoder setTexture:texture atIndex:0];[encoder setTexture:slots[chosen].texture atIndex:1];
        [encoder setBytes:dims length:sizeof(dims) atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake((panel_width+7)/8,(panel_height+7)/8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        [encoder endEncoding];[command commit];[texture release];
        if([command status]==MTLCommandBufferStatusError)goto done;
        [slots[chosen].command release];slots[chosen].command=[command retain];
        changed=lmp_rewrite(output,input,size,render_width,render_height,IOSurfaceGetID(slots[chosen].surface),panel_width,panel_height);
        if(changed){slots[chosen].pending=YES;*reserved=chosen;scaled++;next_slot=(chosen+1)%3;if(scaled==1||scaled%300==0)say_present("native-size swap handed to display");}
done:
        if(!changed){fallback++;if(fallback==1||fallback%600==0)say_present("original swap preserved");}
    }
    pthread_mutex_unlock(&gate);return changed;
}
static kern_return_t lancelot_connect(io_connect_t connection,uint32_t selector,const uint64_t *scalar,uint32_t count,
    const void *input,size_t input_size,uint64_t *outscalar,uint32_t *outcount,void *output,size_t *output_size) {
    if(inside||selector!=5||input_size!=LMP_SWAP_BYTES)return IOConnectCallMethod(connection,selector,scalar,count,input,input_size,outscalar,outcount,output,output_size);
    inside++;uint8_t swap[LMP_SWAP_BYTES];unsigned reserved=3;
    BOOL changed=scale_swap(connection,input,input_size,swap,&reserved);
    kern_return_t result=IOConnectCallMethod(connection,selector,scalar,count,changed?swap:input,input_size,outscalar,outcount,output,output_size);
    if(reserved<3){pthread_mutex_lock(&gate);slots[reserved].pending=NO;pthread_mutex_unlock(&gate);}
    inside--;return result;
}
static kern_return_t lancelot_connect_struct(io_connect_t connection,uint32_t selector,const void *input,size_t input_size,void *output,size_t *output_size) {
    if(inside||selector!=5||input_size!=LMP_SWAP_BYTES)return IOConnectCallStructMethod(connection,selector,input,input_size,output,output_size);
    inside++;uint8_t swap[LMP_SWAP_BYTES];unsigned reserved=3;BOOL changed=scale_swap(connection,input,input_size,swap,&reserved);
    kern_return_t result=IOConnectCallStructMethod(connection,selector,changed?swap:input,input_size,output,output_size);
    if(reserved<3){pthread_mutex_lock(&gate);slots[reserved].pending=NO;pthread_mutex_unlock(&gate);}
    inside--;return result;
}
__attribute__((used,section("__DATA,__interpose"))) static const struct {const void *replacement,*original;} interposes[]={
    {(const void *)lancelot_connect,(const void *)IOConnectCallMethod},
    {(const void *)lancelot_connect_struct,(const void *)IOConnectCallStructMethod}
};

/* Match the low address bits of 20H392's IOConnectCallStructMethod. The
 * cache stub can then be redirected by replacing its ADRP alone, without
 * relocating the function prologue or rewriting several live instructions. */
__asm__(".section __TEXT,__lpstub,regular,pure_instructions\n"
        ".p2align 12\n.space 0x6a4\n"
        ".globl _lancelot_present_cache_stub\n_lancelot_present_cache_stub:\n"
        "b _lancelot_connect_struct\n.text\n");
extern void lancelot_present_cache_stub(void);
extern const void *_dyld_get_shared_cache_range(size_t *length);
static int cache_hook_attempted;
static int query_cache_region(uintptr_t address,uint32_t depth,struct lp_vm_region *out,void *context) {
    (void)context;
    vm_address_t region=address;vm_size_t size=0;
    vm_region_submap_info_data_64_t info={0};
    mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr=vm_region_recurse_64(mach_task_self(),&region,&size,&depth,
        (vm_region_recurse_info_t)&info,&count);
    *out=(struct lp_vm_region){region,size,depth,info.protection,info.is_submap,kr};
    return kr==KERN_SUCCESS;
}
static void say_cache_mapping(const char *stage,const struct lp_vm_region *r) {
    char text[160];snprintf(text,sizeof(text),"cache %s: kr=%d depth=%u submap=%d prot=%x start=%llx bytes=%llx",
        stage,r->error,r->depth,r->submap,r->protection,(unsigned long long)r->start,(unsigned long long)r->bytes);
    say_present(text);
}
static void install_cache_present(const struct mach_header *header,intptr_t slide) {
    (void)slide;
    static const uint8_t wanted_uuid[16]={0xaf,0xd1,0x1f,0x8e,0x00,0x8f,0x32,0x76,0xbd,0x72,0xc0,0xe0,0xd9,0x6c,0xff,0xa6};
    if(strcmp(getprogname(),"backboardd") || header->magic!=MH_MAGIC_64)return;
    const struct mach_header_64 *h=(const struct mach_header_64 *)header;
    if(h->ncmds>1024 || h->sizeofcmds>1048576)return;
    const uint8_t *at=(const uint8_t *)(h+1),*end=at+h->sizeofcmds;BOOL matched=NO;
    for(uint32_t i=0;i<h->ncmds;i++) {
        if((size_t)(end-at)<sizeof(struct load_command))return;
        const struct load_command *cmd=(const struct load_command *)at;
        if(cmd->cmdsize<sizeof(*cmd) || cmd->cmdsize>(size_t)(end-at))return;
        if(cmd->cmd==LC_UUID && cmd->cmdsize>=sizeof(struct uuid_command))
            matched=!memcmp(((const struct uuid_command *)cmd)->uuid,wanted_uuid,16);
        at+=cmd->cmdsize;
    }
    if(!matched || __atomic_exchange_n(&cache_hook_attempted,1,__ATOMIC_ACQ_REL))return;
    size_t cache_size=0;uintptr_t cache=(uintptr_t)_dyld_get_shared_cache_range(&cache_size);
    uintptr_t entry=(uintptr_t)header+LP_SWAP_ENTRY_OFFSET;
    if(!cache || entry<cache || entry-cache>cache_size || cache_size-(entry-cache)<48) {
        say_present("cache hook refused: entry outside the shared cache");return;
    }
    const uint32_t *code=(const uint32_t *)entry;
    if(!lp_swap_entry(code)){say_present("cache hook refused: SwapEnd code differs");return;}
    uintptr_t stub=lp_call_target(entry+44,code[11]);
    if(stub<cache || stub-cache>cache_size || cache_size-(stub-cache)<12) {
        say_present("cache hook refused: stub outside the shared cache");return;
    }
    uint32_t before[3];memcpy(before,(const void *)stub,sizeof(before));uint32_t replacement=0;
    uintptr_t original=0;
    if(!lp_stub_destination(stub,before,&original) || original!=(uintptr_t)IOConnectCallStructMethod) {
        char why[160];snprintf(why,sizeof(why),"cache stub refused: stub=%llx decoded=%llx expected=%llx",
            (unsigned long long)stub,(unsigned long long)original,(unsigned long long)(uintptr_t)IOConnectCallStructMethod);
        say_present(why);return;
    }
    struct lp_vm_region info;
    if(!lp_executable_page(stub,vm_page_size,VM_PROT_EXECUTE,query_cache_region,NULL,&info)) {
        say_cache_mapping("leaf refused",&info);return;
    }
    say_cache_mapping("leaf verified",&info);
    vm_address_t page=stub&~((vm_address_t)vm_page_size-1);
    vm_address_t bridge=0;
    if(!lp_stub_plan(stub,before,original,(uintptr_t)lancelot_present_cache_stub,&replacement)) {
        bridge=page;
        kern_return_t allocated=vm_allocate(mach_task_self(),&bridge,vm_page_size,VM_FLAGS_ANYWHERE);
        if(allocated!=KERN_SUCCESS){info.error=allocated;say_cache_mapping("near bridge allocation refused",&info);return;}
        BOOL planned=lp_stub_plan(stub,before,original,bridge+LP_STUB_LOW,&replacement) &&
            lp_bridge_code((void *)bridge,vm_page_size,(uintptr_t)lancelot_connect_struct);
        if(planned) {
            sys_icache_invalidate((void *)bridge,vm_page_size);
            allocated=vm_protect(mach_task_self(),bridge,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
        }
        if(!planned || allocated!=KERN_SUCCESS) {
            vm_deallocate(mach_task_self(),bridge,vm_page_size);
            info.error=allocated;say_cache_mapping("near RX bridge refused",&info);return;
        }
        say_present("near RX bridge prepared for shared-cache ASLR beyond ADRP range");
    }
    /* iOS vm_protect rejects W+X. Prepare a private anonymous copy without
     * ever removing execute permission from the live cache mapping, then
     * remap the already-RX page in one kernel map operation. Every byte but
     * the validated ADRP is preserved, so in-flight callers remain valid. */
    vm_address_t staging=0;
    kern_return_t kr=vm_allocate(mach_task_self(),&staging,vm_page_size,VM_FLAGS_ANYWHERE);
    if(kr!=KERN_SUCCESS){if(bridge)vm_deallocate(mach_task_self(),bridge,vm_page_size);info.error=kr;say_cache_mapping("copy allocation refused",&info);return;}
    BOOL changed=NO,installed=NO;
    if(lp_copy_code_page((void *)staging,(const void *)page,vm_page_size,stub-page,before[0],replacement)) {
        sys_icache_invalidate((void *)staging,vm_page_size);
        kr=vm_protect(mach_task_self(),staging,vm_page_size,FALSE,info.protection);
        if(kr==KERN_SUCCESS && lp_code_page_unchanged((const void *)page,(const void *)staging,
            vm_page_size,stub-page,before[0])) {
            vm_address_t target=page;vm_prot_t current=0,maximum=0;
            kr=vm_remap(mach_task_self(),&target,vm_page_size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,
                mach_task_self(),staging,FALSE,&current,&maximum,VM_INHERIT_COPY);
            if(kr==KERN_SUCCESS && target==page) {
                installed=YES;
                sys_icache_invalidate((void *)page,vm_page_size);
                changed=__atomic_load_n((uint32_t *)stub,__ATOMIC_ACQUIRE)==replacement &&
                    (current&VM_PROT_EXECUTE) && !(current&VM_PROT_WRITE);
            }
        }
    }
    (void)vm_deallocate(mach_task_self(),staging,vm_page_size);
    if(!changed){if(bridge&&!installed)vm_deallocate(mach_task_self(),bridge,vm_page_size);info.error=kr;say_cache_mapping("RX remap refused",&info);return;}
    say_present("cache_swap_hook=1 verified 20H392; private RX page, one changed instruction");
}
__attribute__((constructor)) static void lancelot_present_prepare(void) {
    if(!strcmp(getprogname(),"backboardd"))_dyld_register_func_for_add_image(install_cache_present);
}
