/* Mali implementation of the Gaussian blur used by Siri's shader prewarm.
 * Other MPS kernels are declined before Apple's GPU-specific initializer;
 * Metal discovery and ordinary render/compute command buffers remain usable. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <math.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static void mps_say(const char *text) {
    char line[384];snprintf(line,sizeof(line),"lancelot-metal-mps: %s",text);
    sysctlbyname("kern.lancelot_hakios.say",NULL,NULL,line,strlen(line));
}
static BOOL mali_device(id device) {
    return device && strcmp(object_getClassName(device),"RedwoodMetalDevice")==0;
}
static void fail_command(id command,const char *reason) {
    mps_say(reason);
    SEL fail=sel_registerName("failWith:");
    if([command respondsToSelector:fail])((void(*)(id,SEL,const char *))objc_msgSend)(command,fail,reason);
}

@interface LancelotGaussianBlur : NSObject <NSCopying>
- (instancetype)initWithDevice:(id<MTLDevice>)device sigma:(float)sigma;
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)command sourceTexture:(id<MTLTexture>)source destinationTexture:(id<MTLTexture>)destination;
@end
@implementation LancelotGaussianBlur {
    id<MTLDevice> _device;
    id<MTLComputePipelineState> _pipeline;
    float _sigma;
    NSUInteger _edgeMode,_options;
    MTLOrigin _offset;
    MTLRegion _clipRect;
    NSString *_label;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device sigma:(float)sigma {
    if(!device || !isfinite(sigma) || sigma<0 || sigma>85) { [self release];return nil; }
    self=[super init];if(!self)return nil;
    _device=[device retain];_sigma=sigma;_clipRect=MTLRegionMake2D(0,0,NSUIntegerMax,NSUIntegerMax);
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithFile:@"/redwood/share/lancelot-effects.metallib" error:&error];
    id<MTLFunction> function=[library newFunctionWithName:@"lancelot_gaussian"];
    _pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
    [function release];[library release];
    if(!_pipeline){mps_say("Gaussian GPU pipeline unavailable; initialization declined");[self release];return nil;}
    mps_say("Gaussian blur uses two Mali compute passes; no CPU image convolution");return self;
}
- (void)dealloc {[_pipeline release];[_device release];[_label release];[super dealloc];}
- (id<MTLDevice>)device{return _device;}
- (float)sigma{return _sigma;}
- (NSUInteger)kernelWidth{return 2*(NSUInteger)ceilf(3*_sigma)+1;}
- (NSUInteger)kernelHeight{return [self kernelWidth];}
- (NSUInteger)edgeMode{return _edgeMode;}
- (void)setEdgeMode:(NSUInteger)value{_edgeMode=value;}
- (NSUInteger)options{return _options;}
- (void)setOptions:(NSUInteger)value{_options=value;}
- (MTLOrigin)offset{return _offset;}
- (void)setOffset:(MTLOrigin)value{_offset=value;}
- (MTLRegion)clipRect{return _clipRect;}
- (void)setClipRect:(MTLRegion)value{_clipRect=value;}
- (NSString *)label{return _label;}
- (void)setLabel:(NSString *)value {if(value!=_label){[_label release];_label=[value copy];}}
- (id)copyWithZone:(NSZone *)zone {
    LancelotGaussianBlur *copy=[[[self class] allocWithZone:zone] initWithDevice:_device sigma:_sigma];
    [copy setEdgeMode:_edgeMode];[copy setOptions:_options];[copy setOffset:_offset];[copy setClipRect:_clipRect];[copy setLabel:_label];return copy;
}
- (id)copyWithZone:(NSZone *)zone device:(id<MTLDevice>)device {
    LancelotGaussianBlur *copy=[[[self class] allocWithZone:zone] initWithDevice:device sigma:_sigma];
    [copy setEdgeMode:_edgeMode];[copy setOptions:_options];[copy setOffset:_offset];[copy setClipRect:_clipRect];[copy setLabel:_label];return copy;
}
- (BOOL)encode:(id<MTLCommandBuffer>)command source:(id<MTLTexture>)source destination:(id<MTLTexture>)destination {
    NSUInteger width=[source width],height=[source height];
    if(!command || !source || !destination || !width || !height || [destination width]!=width || [destination height]!=height ||
       [source textureType]!=MTLTextureType2D || [destination textureType]!=MTLTextureType2D ||
       [source device]!=_device || [destination device]!=_device || [command device]!=_device ||
       [source sampleCount]!=1 || [destination sampleCount]!=1 ||
       _edgeMode>1 || _offset.x || _offset.y || _offset.z || _clipRect.origin.x || _clipRect.origin.y || _clipRect.origin.z ||
       _clipRect.size.width<width || _clipRect.size.height<height) {
        mps_say("unsupported Gaussian region/texture parameters; encode declined");return NO;
    }
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
    descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
    descriptor.storageMode=MTLStorageModePrivate;
    id<MTLTexture> temporary=[_device newTextureWithDescriptor:descriptor];
    if(!temporary){mps_say("Gaussian intermediate allocation failed");return NO;}
    unsigned radius=(unsigned)ceilf(3*_sigma);
    struct {uint32_t width,height,radius,axis,edge,pad[3];float weights[513];} params={0};
    params.width=(uint32_t)width;params.height=(uint32_t)height;params.radius=radius;params.edge=(uint32_t)_edgeMode;
    double total=0;
    for(int i=-(int)radius;i<=(int)radius;i++) {
        double weight=radius?exp(-((double)i*i)/(2*(double)_sigma*_sigma)):1;
        params.weights[i+(int)radius]=(float)weight;total+=weight;
    }
    for(unsigned i=0;i<2*radius+1;i++)params.weights[i]/=(float)total;
    for(unsigned axis=0;axis<2;axis++) {
        params.axis=axis;
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!encoder){[temporary release];mps_say("Gaussian compute encoder unavailable");return NO;}
        [encoder setComputePipelineState:_pipeline];
        [encoder setTexture:axis?temporary:source atIndex:0];
        [encoder setTexture:axis?destination:temporary atIndex:1];
        [encoder setBytes:&params length:sizeof(params) atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake((width+7)/8,(height+7)/8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        [encoder endEncoding];
    }
    [temporary release];return YES;
}
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)command sourceTexture:(id<MTLTexture>)source destinationTexture:(id<MTLTexture>)destination {
    if(![self encode:command source:source destination:destination])fail_command(command,"Gaussian encode failed; command buffer marked as an error");
}
- (BOOL)encodeToCommandBuffer:(id<MTLCommandBuffer>)command inPlaceTexture:(id<MTLTexture> *)texture fallbackCopyAllocator:(id)allocator {
    (void)allocator;return texture?[self encode:command source:*texture destination:*texture]:NO;
}
@end

static IMP original_gaussian,original_kernel;
static pthread_mutex_t hook_lock=PTHREAD_MUTEX_INITIALIZER;
static BOOL signature_matches(Method method,unsigned count,BOOL sigma) {
    if(!method || method_getNumberOfArguments(method)!=count)return NO;
    char type[32];method_getReturnType(method,type,sizeof(type));if(strcmp(type,"@"))return NO;
    method_getArgumentType(method,0,type,sizeof(type));if(strcmp(type,"@"))return NO;
    method_getArgumentType(method,1,type,sizeof(type));if(strcmp(type,":"))return NO;
    method_getArgumentType(method,2,type,sizeof(type));if(type[0]!='@')return NO;
    if(sigma){method_getArgumentType(method,3,type,sizeof(type));if(strcmp(type,"f"))return NO;}
    return YES;
}
static id gaussian_init(id self,SEL selector,id device,float sigma) {
    if(!mali_device(device))return ((id(*)(id,SEL,id,float))__atomic_load_n(&original_gaussian,__ATOMIC_ACQUIRE))(self,selector,device,sigma);
    id replacement=[[LancelotGaussianBlur alloc] initWithDevice:device sigma:sigma];
    object_dispose(self); /* uninitialized allocation; Apple's initializer never ran */
    return replacement;
}
static id kernel_init(id self,SEL selector,id device) {
    if(!mali_device(device))return ((id(*)(id,SEL,id))__atomic_load_n(&original_kernel,__ATOMIC_ACQUIRE))(self,selector,device);
    char line[256];snprintf(line,sizeof(line),"%s has no Mali MPS implementation; declined before Apple's GPU initializer",object_getClassName(self));mps_say(line);
    object_dispose(self);return nil;
}
static void install_mps(__unused const struct mach_header *header,__unused intptr_t slide) {
    pthread_mutex_lock(&hook_lock);
    if(!original_gaussian) {
        Class cls=objc_getClass("MPSImageGaussianBlur");Method m=class_getInstanceMethod(cls,sel_registerName("initWithDevice:sigma:"));
        if(signature_matches(m,4,YES)) {
            __atomic_store_n(&original_gaussian,method_getImplementation(m),__ATOMIC_RELEASE);
            method_setImplementation(m,(IMP)gaussian_init);
            mps_say("Gaussian initializer hook installed");
        }
    }
    if(!original_kernel) {
        Class cls=objc_getClass("MPSKernel");Method m=class_getInstanceMethod(cls,sel_registerName("initWithDevice:"));
        if(signature_matches(m,3,NO)) {
            __atomic_store_n(&original_kernel,method_getImplementation(m),__ATOMIC_RELEASE);
            method_setImplementation(m,(IMP)kernel_init);
            mps_say("MPSKernel initializer guard installed");
        }
    }
    pthread_mutex_unlock(&hook_lock);
}
void lancelot_mps_prepare(void) {
    static dispatch_once_t once;
    dispatch_once(&once,^{
        /* add-image callbacks can run before Objective-C has registered the
         * new classes. Finish loading MPS, then resolve and replace methods;
         * merely seeing its Mach-O image is not sufficient. */
        void *framework=dlopen("/System/Library/Frameworks/MetalPerformanceShaders.framework/MetalPerformanceShaders",RTLD_NOW|RTLD_LOCAL);
        if(!framework)mps_say("MPS framework load failed");
        _dyld_register_func_for_add_image(install_mps);
    });
    install_mps(NULL,0);
    if(!original_gaussian || !original_kernel) {
        char line[384];Class gaussian=objc_getClass("MPSImageGaussianBlur"),kernel=objc_getClass("MPSKernel");
        Method gm=class_getInstanceMethod(gaussian,sel_registerName("initWithDevice:sigma:"));
        Method km=class_getInstanceMethod(kernel,sel_registerName("initWithDevice:"));
        snprintf(line,sizeof(line),"initializer hook unavailable: Gaussian=%s Kernel=%s",
            gm?method_getTypeEncoding(gm):"class/method absent",km?method_getTypeEncoding(km):"class/method absent");
        mps_say(line);
    }
}
