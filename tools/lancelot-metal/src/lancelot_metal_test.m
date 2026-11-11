/* Public Metal API test for the native lancelot driver; no fixed shared-cache addresses. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
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
#include <math.h>
#include <spawn.h>
#include <poll.h>
extern char **environ;
static const char *testProgram;

static int heldSurfaceReader(int argc,char **argv) {
    if(argc!=5)return 2;
    alarm(15);
    IOSurfaceRef surface=IOSurfaceLookup((uint32_t)strtoul(argv[2],NULL,10));
    int ready=atoi(argv[3]),finish=atoi(argv[4]);char byte='R';
    if(!surface || IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL))return 3;
    int ok=write(ready,&byte,1)==1 && read(finish,&byte,1)==1;
    if(IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL))ok=0;
    CFRelease(surface);return ok?0:4;
}

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

static int displayConnection(void) {
    io_service_t expected=IOServiceGetMatchingService(0,IOServiceMatching("IOMobileFramebuffer")),actual=0;
    io_connect_t connection=0;int ok=0;uint32_t dimensions[4]={0};
    const CFStringRef keys[]={CFSTR("lancelot-panel-width"),CFSTR("lancelot-panel-height"),
        CFSTR("lancelot-render-width"),CFSTR("lancelot-render-height")};
    if(expected && IOServiceOpen(expected,mach_task_self(),0,&connection)==KERN_SUCCESS &&
        IOConnectGetService(connection,&actual)==KERN_SUCCESS && IOObjectIsEqualTo(expected,actual)) {
        ok=1;
        for(unsigned i=0;i<4;i++) {
            CFTypeRef value=IORegistryEntryCreateCFProperty(actual,keys[i],kCFAllocatorDefault,0);
            if(!value || CFGetTypeID(value)!=CFNumberGetTypeID() ||
                !CFNumberGetValue((CFNumberRef)value,kCFNumberSInt32Type,&dimensions[i]) ||
                !dimensions[i] || dimensions[i]>8192)ok=0;
            if(value)CFRelease(value);
        }
    }
    if(actual)IOObjectRelease(actual);if(connection)IOServiceClose(connection);if(expected)IOObjectRelease(expected);
    printf("lancelot-metal-test: display_connection=%s panel=%ux%u render=%ux%u\n",
        ok?"PASS":"FAIL",dimensions[0],dimensions[1],dimensions[2],dimensions[3]);return ok;
}

static int runSharedReader(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLComputePipelineState> pipeline) {
    const unsigned width=17,height=11,pitch=128;
    NSDictionary *properties=@{(id)kIOSurfaceWidth:@(width),(id)kIOSurfaceHeight:@(height),
        (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@(pitch),
        (id)kIOSurfaceAllocSize:@(pitch*height),(id)kIOSurfacePixelFormat:@(0x42475241u)};
    IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)properties);
    if(!surface || IOSurfaceLock(surface,0,NULL))return 0;
    uint8_t *pixels=IOSurfaceGetBaseAddress(surface);const uint8_t color[]={49,97,173,255};
    if(pixels)for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++)memcpy(pixels+y*pitch+4*x,color,4);
    int unlocked=IOSurfaceUnlock(surface,0,NULL);
    if(!pixels || unlocked){CFRelease(surface);return 0;}
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
    d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> source=[device newTextureWithDescriptor:d iosurface:surface plane:0];
    SEL inPlace=sel_registerName("lancelotInPlaceSurface");
    if(!source || ![source respondsToSelector:inPlace] ||
        ((uint32_t(*)(id,SEL))objc_msgSend)(source,inPlace)!=IOSurfaceGetID(surface)) {
        [source release];CFRelease(surface);return 0;
    }
    d.width=9;d.height=7;d.usage=MTLTextureUsageShaderWrite;
    id<MTLTexture> dest=[device newTextureWithDescriptor:d];
    int ready[2]={-1,-1},finish[2]={-1,-1},ok=0,status=0;pid_t pid=-1;
    if(!dest || pipe(ready) || pipe(finish))goto done;
    char sid[24],rf[24],ff[24];snprintf(sid,sizeof(sid),"%u",IOSurfaceGetID(surface));
    snprintf(rf,sizeof(rf),"%d",ready[1]);snprintf(ff,sizeof(ff),"%d",finish[0]);
    char *args[]={(char *)testProgram,"--surface-reader",sid,rf,ff,NULL};
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions))goto done;
    int spawnError=posix_spawn_file_actions_addclose(&actions,ready[0]);
    if(!spawnError)spawnError=posix_spawn_file_actions_addclose(&actions,finish[1]);
    if(!spawnError)spawnError=posix_spawn(&pid,testProgram,&actions,NULL,args,environ);
    posix_spawn_file_actions_destroy(&actions);
    if(spawnError)goto done;
    close(ready[1]);ready[1]=-1;close(finish[0]);finish[0]=-1;
    struct pollfd pollReady={ready[0],POLLIN,0};char byte=0;
    if(poll(&pollReady,1,5000)<=0 || read(ready[0],&byte,1)!=1 || byte!='R')goto done;
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    uint32_t dims[]={width,height,9,7};
    if(!encoder)goto done;
    [encoder setComputePipelineState:pipeline];[encoder setTexture:source atIndex:0];[encoder setTexture:dest atIndex:1];
    [encoder setBytes:dims length:sizeof(dims) atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(2,1,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[encoder endEncoding];
    ok=waitCommand(command); /* helper keeps the read lock throughout GPU completion */
    if(ok) {
        uint8_t output[9*7*4]={0};
        [dest getBytes:output bytesPerRow:9*4 fromRegion:MTLRegionMake2D(0,0,9,7) mipmapLevel:0];
        for(unsigned i=0;i<sizeof(output);i++)if(abs((int)output[i]-color[i%4])>1)ok=0;
    }
    if(write(finish[1],"D",1)!=1)ok=0;
done:
    for(unsigned i=0;i<2;i++){if(ready[i]>=0)close(ready[i]);if(finish[i]>=0)close(finish[i]);}
    if(pid>0) {
        if(!ok)kill(pid,SIGTERM);
        pid_t reaped;do{reaped=waitpid(pid,&status,0);}while(reaped<0&&errno==EINTR);
        ok=ok && reaped==pid && WIFEXITED(status) && WEXITSTATUS(status)==0;
    }
    [source release];[dest release];CFRelease(surface);
    printf("lancelot-metal-test: cross_process_IOSurface_reader=%s in_place=1 GPU_readback=1\n",ok?"PASS":"FAIL");
    return ok;
}

static int runEffects(id<MTLDevice> device,id<MTLCommandQueue> queue) {
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithFile:@"/redwood/share/lancelot-effects.metallib" error:&error];
    id<MTLFunction> scale=[library newFunctionWithName:@"lancelot_scale"];
    id<MTLComputePipelineState> pipeline=scale?[device newComputePipelineStateWithFunction:scale error:&error]:nil;
    if(!pipeline){printf("lancelot-metal-test: effects pipeline unavailable\n");return 0;}
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:17 height:11 mipmapped:NO];
    d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
    id<MTLTexture> source=[device newTextureWithDescriptor:d];d.width=9;d.height=7;
    id<MTLTexture> dest=[device newTextureWithDescriptor:d];
    float input[17*11*4],output[9*7*4];
    for(unsigned y=0;y<11;y++)for(unsigned x=0;x<17;x++)for(unsigned c=0;c<4;c++)input[(y*17+x)*4+c]=c==3?1.0f:(float)(x+2*y+c)/40;
    [source replaceRegion:MTLRegionMake2D(0,0,17,11) mipmapLevel:0 withBytes:input bytesPerRow:17*4*sizeof(float)];
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    uint32_t dims[]={17,11,9,7};
    [encoder setComputePipelineState:pipeline];[encoder setTexture:source atIndex:0];[encoder setTexture:dest atIndex:1];
    [encoder setBytes:dims length:sizeof(dims) atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(2,1,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[encoder endEncoding];
    if(!waitCommand(command))return 0;
    memset(output,0,sizeof(output));[dest getBytes:output bytesPerRow:9*4*sizeof(float) fromRegion:MTLRegionMake2D(0,0,9,7) mipmapLevel:0];
    for(unsigned y=0;y<7;y++)for(unsigned x=0;x<9;x++)for(unsigned c=0;c<4;c++) {
        float fx=fmaxf(0,((float)x+0.5f)*17/9-0.5f),fy=fmaxf(0,((float)y+0.5f)*11/7-0.5f);
        unsigned ix=(unsigned)fx,iy=(unsigned)fy,jx=ix<16?ix+1:16,jy=iy<10?iy+1:10;
        float a=input[(iy*17+ix)*4+c],b=input[(iy*17+jx)*4+c],e=input[(jy*17+ix)*4+c],f=input[(jy*17+jx)*4+c];
        float top=a+(b-a)*(fx-ix),bottom=e+(f-e)*(fx-ix),expected=top+(bottom-top)*(fy-iy);
        if(!isfinite(output[(y*9+x)*4+c]) || fabsf(output[(y*9+x)*4+c]-expected)>0.002f)return 0;
    }
    printf("lancelot-metal-test: GPU_bilinear_scale=PASS 17x11_to_9x7\n");
    if(!runSharedReader(device,queue,pipeline))return 0;
    [source release];[dest release];[pipeline release];[scale release];[library release];
    if(!dlopen("/System/Library/Frameworks/MetalPerformanceShaders.framework/MetalPerformanceShaders",RTLD_NOW|RTLD_GLOBAL))return 0;
    Class blurClass=NSClassFromString(@"MPSImageGaussianBlur");
    id blur=((id(*)(id,SEL,id,float))objc_msgSend)([blurClass alloc],sel_registerName("initWithDevice:sigma:"),device,1.0f);
    if(!blur || strcmp(object_getClassName(blur),"LancelotGaussianBlur"))return 0;
    d.width=8;d.height=8;source=[device newTextureWithDescriptor:d];dest=[device newTextureWithDescriptor:d];
    float impulse[8*8*4]={0},filtered[8*8*4];for(unsigned c=0;c<4;c++)impulse[(4*8+4)*4+c]=1;
    [source replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:impulse bytesPerRow:8*4*sizeof(float)];
    command=[queue commandBuffer];
    ((void(*)(id,SEL,id,id,id))objc_msgSend)(blur,sel_registerName("encodeToCommandBuffer:sourceTexture:destinationTexture:"),command,source,dest);
    if(!waitCommand(command))return 0;
    [dest getBytes:filtered bytesPerRow:8*4*sizeof(float) fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
    double weights[7],total=0;for(int i=-3;i<=3;i++){weights[i+3]=exp(-(double)(i*i)/2);total+=weights[i+3];}
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)for(unsigned c=0;c<4;c++) {
        double expected=x>=1&&y>=1?weights[x-1]*weights[y-1]/(total*total):0;
        if(!isfinite(filtered[(y*8+x)*4+c]) || fabs(filtered[(y*8+x)*4+c]-expected)>0.002)return 0;
    }
    printf("lancelot-metal-test: Mali_MPS_Gaussian=PASS CPU_reference_compared\n");
    [source release];[dest release];[blur release];return 1;
}

static int runTests(BOOL ca)
{
    if(!displayConnection())return 0;
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
    /* Draw A, clear B without a draw, then draw A twice. A cached framebuffer
     * must notice the clear-only switch and preserve B's independent pixels. */
    id<MTLTexture> alternate=[device newTextureWithDescriptor:description];
    if(!alternate)return 0;
    MTLRenderPassDescriptor *otherPass=[MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *otherAttachment=otherPass.colorAttachments[0];
    otherAttachment.texture=alternate;otherAttachment.loadAction=MTLLoadActionClear;
    otherAttachment.storeAction=MTLStoreActionStore;otherAttachment.clearColor=MTLClearColorMake(0.2,0.4,0.6,1);
    id<MTLCommandBuffer> clearOther=[queue commandBuffer];
    id<MTLRenderCommandEncoder> otherEncoder=[clearOther renderCommandEncoderWithDescriptor:otherPass];
    if(!otherEncoder)return 0;[otherEncoder endEncoding];if(!waitCommand(clearOther))return 0;
    attachment.loadAction=MTLLoadActionLoad;
    id<MTLCommandBuffer> repeat=[queue commandBuffer];
    id<MTLRenderCommandEncoder> repeated=[repeat renderCommandEncoderWithDescriptor:pass];
    if(!repeated)return 0;[repeated setRenderPipelineState:pipeline];
    [repeated drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [repeated drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [repeated endEncoding];if(!waitCommand(repeat))return 0;
    [alternate getBytes:pixels bytesPerRow:16*4 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
    const int otherExpected[4]={51,102,153,255};
    for(unsigned i=0;i<sizeof(pixels);i++)if(abs((int)pixels[i]-otherExpected[i%4])>1)return 0;
    [texture getBytes:pixels bytesPerRow:16*4 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
    for(unsigned i=0;i<sizeof(pixels);i++)if(abs((int)pixels[i]-expected[i%4])>1)return 0;
    [alternate release];printf("lancelot-metal-test: framebuffer_draw_clear_switch=PASS pixels=512\n");
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
    if(!runEffects(device,queue))return 0;
    [pipeline release];[pipelineDescription release];[fragment release];[vertex release];[texture release];
    [compute release];[kernel release];[library release];[output release];[source release];[queue release];
    return 1;
}

int main(int argc,char**argv)
{
    setbuf(stdout,NULL);setbuf(stderr,NULL);
    testProgram=argv[0];
    @autoreleasepool {
        if(argc>1&&!strcmp(argv[1],"--surface-reader"))return heldSurfaceReader(argc,argv);
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
