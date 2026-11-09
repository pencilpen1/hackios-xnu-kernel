/* Public Metal API test for the native lancelot driver; no fixed shared-cache addresses. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern kern_return_t set_properties(io_registry_entry_t,CFTypeRef) __asm("_IORegistryEntrySetCFProperties");

static int discovery(BOOL enabled)
{
    io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching("IOAcceleratorES"));
    if(!service)return 0;
    NSDictionary* values=@{@"MetalPluginName":enabled?@"RedwoodMetal":@"",
                           @"MetalPluginClassName":enabled?@"RedwoodMetalDevice":@""};
    kern_return_t kr=set_properties(service,(CFTypeRef)values);
    int ok=kr==KERN_SUCCESS;
    if(enabled && ok) {
        CFTypeRef plugin=IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginName"),NULL,0);
        CFTypeRef cls=IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginClassName"),NULL,0);
        ok=plugin && cls && CFEqual(plugin,CFSTR("RedwoodMetal")) && CFEqual(cls,CFSTR("RedwoodMetalDevice"));
        if(plugin)CFRelease(plugin);if(cls)CFRelease(cls);
    }
    IOObjectRelease(service);
    printf("lancelot-metal-test: discovery=%s result=%d kr=%#x\n",enabled?"on":"off",ok,kr);
    return ok;
}

static int waitCommand(id<MTLCommandBuffer> command)
{
    if(!command)return 0;
    [command commit];[command waitUntilCompleted];
    if([command status]!=MTLCommandBufferStatusCompleted || [command error]) {
        printf("lancelot-metal-test: command failed status=%lu error=%s\n",
               (unsigned long)[command status],[[[command error] description] UTF8String]);return 0;
    }
    return 1;
}

static int runTests(BOOL ca)
{
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if(!device || ![[device name] containsString:@"Mali"] ||
       ![(id)device respondsToSelector:sel_registerName("mesaDevice")]) {
        printf("lancelot-metal-test: no native Mali MTLDevice\n");return 0;
    }
    printf("lancelot-metal-test: device=%s class=%s\n",[[device name] UTF8String],object_getClassName(device));
    id<MTLCommandQueue> queue=[device newCommandQueue];
    id<MTLBuffer> source=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if(!queue || !source || !output || ![source contents] || ![output contents])return 0;
    SEL address=sel_registerName("gpuAddress");
    uint64_t va=[(id)source respondsToSelector:address]?((uint64_t(*)(id,SEL))objc_msgSend)(source,address):0;
    if(!va) {printf("lancelot-metal-test: buffer has no Mali GPU address\n");return 0;}
    memset([source contents],0xab,256);memset([output contents],0,256);
    id<MTLCommandBuffer> copy=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[copy blitCommandEncoder];
    if(!blit)return 0;
    [blit copyFromBuffer:source sourceOffset:0 toBuffer:output destinationOffset:0 size:256];
    [blit endEncoding];
    dispatch_semaphore_t handler=dispatch_semaphore_create(0);
    __block BOOL handlerOK=NO;
    [copy addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        handlerOK=[finished status]==MTLCommandBufferStatusCompleted && [finished error]==nil;
        dispatch_semaphore_signal(handler);
    }];
    if(!waitCommand(copy) || dispatch_semaphore_wait(handler,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)) || !handlerOK)
        return 0;
    for(unsigned i=0;i<256;i++)if(((uint8_t*)[output contents])[i]!=0xab)return 0;
    dispatch_release(handler);
    printf("lancelot-metal-test: buffer_copy_and_completed_handler=PASS gpu_va=%llx\n",(unsigned long long)va);

    NSError* error=nil;
    id<MTLLibrary> library=[device newLibraryWithFile:@"/redwood/share/lancelot-test.metallib" error:&error];
    id<MTLFunction> kernel=[library newFunctionWithName:@"lancelot_fill"];
    id<MTLComputePipelineState> compute=kernel?[device newComputePipelineStateWithFunction:kernel error:&error]:nil;
    if(!library || !compute) {printf("lancelot-metal-test: compute pipeline failed: %s\n",[[error description] UTF8String]);return 0;}
    memset([output contents],0,256);
    id<MTLCommandBuffer> dispatch=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[dispatch computeCommandEncoder];
    if(!encoder)return 0;
    [encoder setComputePipelineState:compute];[encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [encoder endEncoding];if(!waitCommand(dispatch))return 0;
    for(unsigned i=0;i<64;i++)if(((uint32_t*)[output contents])[i]!=0x12340000+i) {
        printf("lancelot-metal-test: compute mismatch index=%u\n",i);return 0;
    }
    printf("lancelot-metal-test: AIR_compute_readback=PASS words=64\n");

    MTLTextureDescriptor* description=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:16 height:16 mipmapped:NO];
    [description setUsage:MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead];
    id<MTLTexture> texture=[device newTextureWithDescriptor:description];
    id<MTLFunction> vertex=[library newFunctionWithName:@"lancelot_vertex"];
    id<MTLFunction> fragment=[library newFunctionWithName:@"lancelot_fragment"];
    MTLRenderPipelineDescriptor* pipelineDescription=[[MTLRenderPipelineDescriptor alloc] init];
    [pipelineDescription setVertexFunction:vertex];[pipelineDescription setFragmentFunction:fragment];
    [[[pipelineDescription colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:MTLPixelFormatRGBA8Unorm];
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pipelineDescription error:&error];
    if(!texture || !vertex || !fragment || !pipeline) {
        printf("lancelot-metal-test: render pipeline failed: %s\n",[[error description] UTF8String]);return 0;
    }
    MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor* attachment=[[pass colorAttachments] objectAtIndexedSubscript:0];
    [attachment setTexture:texture];[attachment setLoadAction:MTLLoadActionClear];[attachment setStoreAction:MTLStoreActionStore];
    [attachment setClearColor:MTLClearColorMake(0,0,0,0)];
    id<MTLCommandBuffer> draw=[queue commandBuffer];
    id<MTLRenderCommandEncoder> renderer=[draw renderCommandEncoderWithDescriptor:pass];
    if(!renderer)return 0;
    [renderer setRenderPipelineState:pipeline];[renderer drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [renderer endEncoding];if(!waitCommand(draw))return 0;
    uint8_t pixels[16*16*4]={};
    [texture getBytes:pixels bytesPerRow:16*4 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
    const int expected[4]={64,128,191,255};
    for(unsigned i=0;i<sizeof(pixels);i++)if(abs((int)pixels[i]-expected[i%4])>1) {
        printf("lancelot-metal-test: render mismatch byte=%u actual=%u\n",i,pixels[i]);return 0;
    }
    printf("lancelot-metal-test: AIR_render_readback=PASS pixels=256\n");
    if(ca) {
        void* helper=dlopen("/redwood/lib/liblancelot-metal-ca.dylib",RTLD_NOW|RTLD_LOCAL);
        int(*prepare)(void)=helper?(int(*)(void))dlsym(helper,"lancelot_metal_ca_prepare"):NULL;
        if(!prepare || !prepare())return 0;
        NSBundle* quartz=[NSBundle bundleWithIdentifier:@"com.apple.QuartzCore"];
        if(!quartz)quartz=[NSBundle bundleWithPath:@"/System/Library/Frameworks/QuartzCore.framework"];
        NSURL* resource=[quartz URLForResource:@"default" withExtension:@"metallib"];
        if(!resource || ![[resource path] isEqualToString:@"/redwood/share/QuartzCore-default.metallib"])return 0;
        id<MTLLibrary> shaders=[device newLibraryWithFile:[resource path] error:&error];
        if(!shaders || [[shaders functionNames] count]<100)return 0;
        printf("lancelot-metal-test: QuartzCore_library=PASS functions=%lu\n",(unsigned long)[[shaders functionNames] count]);
        [shaders release];
    }
    [pipeline release];[pipelineDescription release];[fragment release];[vertex release];[texture release];
    [compute release];[kernel release];[library release];[output release];[source release];[queue release];
    return 1;
}

int main(int argc,char**argv)
{
    setbuf(stdout,NULL);setbuf(stderr,NULL);
    @autoreleasepool {
        BOOL child=argc>1 && !strncmp(argv[1],"--verify-",9);
        BOOL ca=argc>1 && (!strcmp(argv[1],"--ca") || !strcmp(argv[1],"--verify-ca"));
        if(child) {
            alarm(45);
            @try {int ok=runTests(ca);printf("lancelot-metal-test: verified=%d scope=%s\n",ok,ca?"ca":"basic");return ok?0:1;}
            @catch(NSException*exception) {printf("lancelot-metal-test: exception=%s\n",[[exception description] UTF8String]);return 1;}
        }
        if(!discovery(YES))return 1;
        pid_t pid=fork();
        if(pid==0) {
            char*args[]={argv[0],ca?"--verify-ca":"--verify-basic",NULL};
            execv(argv[0],args);_exit(127);
        }
        int status=0;pid_t collected=-1;
        if(pid>0)do{collected=waitpid(pid,&status,0);}while(collected<0 && errno==EINTR);
        BOOL ok=pid>0 && collected==pid && WIFEXITED(status) && WEXITSTATUS(status)==0;
        if(!ok)discovery(NO);
        printf("lancelot-metal-test: admission=%s discovery=%s child_status=%d\n",ok?"PASS":"FAIL",ok?"on":"off",status);
        return ok?0:1;
    }
}
