/* CoreAnimation Metal resource and function-constant adaptation through Objective-C methods. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static NSString *const shaderPath = @"/redwood/share/QuartzCore-default.metallib";
static const void *constantsKey = "lancelotMetalConstants";
static NSURL *(*originalURL)(id, SEL, NSString *, NSString *);
static BOOL prepared;

static NSURL *resourceURL(id bundle, SEL selector, NSString *name, NSString *extension)
{
    if ([[bundle bundleIdentifier] isEqualToString:@"com.apple.QuartzCore"] &&
        [name isEqualToString:@"default"] && [extension isEqualToString:@"metallib"] &&
        access([shaderPath fileSystemRepresentation], R_OK) == 0) {
        return [NSURL fileURLWithPath:shaderPath];
    }
    return originalURL(bundle, selector, name, extension);
}

static size_t constantBytes(NSUInteger type)
{
    const struct {NSUInteger first, element;} groups[]={{3,4},{16,2},{29,4},{33,4},{37,2},{41,2},{45,1},{49,1},{53,1}};
    for (unsigned i=0; i<sizeof(groups)/sizeof(groups[0]); i++)
        if (type>=groups[i].first && type<groups[i].first+4)
            return groups[i].element*(type-groups[i].first+1);
    return type==81 || type==85 ? 8 : 0;
}

static void saveConstant(id values, id key, const void *value, NSUInteger type)
{
    if (value==NULL || constantBytes(type)==0) return;
    NSMutableDictionary *saved=objc_getAssociatedObject(values,constantsKey);
    if (!saved) {
        saved=[NSMutableDictionary dictionary];
        objc_setAssociatedObject(values,constantsKey,saved,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    uint64_t bits=0;
    size_t bytes=constantBytes(type);
    memcpy(&bits,value,bytes<sizeof(bits)?bytes:sizeof(bits));
    [saved setObject:@(bits) forKey:key];
}
static void (*originalIndex)(id,SEL,const void*,NSUInteger,NSUInteger);
static void (*originalName)(id,SEL,const void*,NSUInteger,NSString*);
static void (*originalRange)(id,SEL,const void*,NSUInteger,NSRange);
static void setIndex(id values,SEL selector,const void*value,NSUInteger type,NSUInteger index)
{
    saveConstant(values,@(index),value,type);
    originalIndex(values,selector,value,type,index);
}
static void setName(id values,SEL selector,const void*value,NSUInteger type,NSString*name)
{
    saveConstant(values,name,value,type);
    originalName(values,selector,value,type,name);
}
static void setRange(id values,SEL selector,const void*value,NSUInteger type,NSRange range)
{
    size_t bytes=constantBytes(type);
    for (NSUInteger i=0;value && bytes && i<range.length;i++)
        saveConstant(values,@(range.location+i),(const uint8_t*)value+i*bytes,type);
    originalRange(values,selector,value,type,range);
}
static id plainFunction(id library,NSString*name,id values)
{
    id function=[library newFunctionWithName:name];
    NSDictionary* saved=values?objc_getAssociatedObject(values,constantsKey):nil;
    if (!function || !saved) return function;
    NSMutableDictionary* indices=[NSMutableDictionary dictionary];
    NSDictionary* declared=[function functionConstantsDictionary];
    for(id key in saved) {
        id index=key;
        if ([key isKindOfClass:[NSString class]]) {
            id constant=[declared objectForKey:key];
            if (!constant) continue;
            index=@([(MTLFunctionConstant*)constant index]);
        }
        [indices setObject:[saved objectForKey:key] forKey:index];
    }
    objc_setAssociatedObject(function,sel_registerName("redwoodFunctionConstants"),indices,OBJC_ASSOCIATION_RETAIN);
    return function;
}
static id functionWithValues(id library,SEL selector,NSString*name,id values,NSError**error)
{
    (void)selector;
    if(error)*error=nil;
    return plainFunction(library,name,values);
}
static void functionWithHandler(id library,SEL selector,NSString*name,id values,void(^handler)(id,NSError*))
{
    (void)selector;
    id function=plainFunction(library,name,values);
    if(handler)handler(function,nil);
    [function release];
}
static struct {Class cls; IMP original;} descriptorMethods[32];
static unsigned descriptorCount;
static id functionWithDescriptor(id library,SEL selector,id descriptor,NSError**error)
{
    NSString* name=[descriptor name];
    id values=[descriptor constantValues];
    if(name && values) {
        id function=plainFunction(library,name,values);
        if(function) {if(error)*error=nil;return function;}
    }
    for(Class cls=object_getClass(library);cls;cls=class_getSuperclass(cls))
        for(unsigned i=0;i<descriptorCount;i++)if(descriptorMethods[i].cls==cls)
            return ((id(*)(id,SEL,id,NSError**))descriptorMethods[i].original)(library,selector,descriptor,error);
    if(error)*error=[NSError errorWithDomain:@"LancelotMetal" code:1 userInfo:@{NSLocalizedDescriptionKey:@"No library function for the requested descriptor"}];
    return nil;
}

/* Called before CAWindowServer creates its Metal renderer, without creating a device here. */
int lancelot_metal_ca_prepare(void)
{
    if(prepared)return 1;
    if(access([shaderPath fileSystemRepresentation],R_OK)!=0) {
        fprintf(stderr,"lancelot-metal-ca: QuartzCore AIR resource unavailable\n");return 0;
    }
    Class values=objc_getClass("MTLFunctionConstantValuesInternal");
    if(!values)values=objc_getClass("MTLFunctionConstantValues");
    Class library=objc_getClass("_MTLLibrary");
    Method url=class_getInstanceMethod([NSBundle class],@selector(URLForResource:withExtension:));
    Method index=class_getInstanceMethod(values,@selector(setConstantValue:type:atIndex:));
    Method name=class_getInstanceMethod(values,@selector(setConstantValue:type:withName:));
    Method range=class_getInstanceMethod(values,@selector(setConstantValues:type:withRange:));
    Method sync=class_getInstanceMethod(library,@selector(newFunctionWithName:constantValues:error:));
    Method async=class_getInstanceMethod(library,@selector(newFunctionWithName:constantValues:completionHandler:));
    if(!url || !index || !name || !range || !sync || !async) {
        fprintf(stderr,"lancelot-metal-ca: required Objective-C methods unavailable\n");return 0;
    }
    originalURL=(void*)method_setImplementation(url,(IMP)resourceURL);
    originalIndex=(void*)method_setImplementation(index,(IMP)setIndex);
    originalName=(void*)method_setImplementation(name,(IMP)setName);
    originalRange=(void*)method_setImplementation(range,(IMP)setRange);
    method_setImplementation(sync,(IMP)functionWithValues);
    method_setImplementation(async,(IMP)functionWithHandler);
    unsigned count=0;Class* classes=objc_copyClassList(&count);
    SEL selector=@selector(newFunctionWithDescriptor:error:);
    Protocol* protocol=objc_getProtocol("MTLLibrary");
    for(unsigned i=0;classes && i<count && descriptorCount<32;i++) {
        if(protocol && !class_conformsToProtocol(classes[i],protocol))continue;
        unsigned methodCount=0;Method* methods=class_copyMethodList(classes[i],&methodCount);
        for(unsigned k=0;k<methodCount && descriptorCount<32;k++)if(method_getName(methods[k])==selector) {
            descriptorMethods[descriptorCount].cls=classes[i];
            descriptorMethods[descriptorCount++].original=method_setImplementation(methods[k],(IMP)functionWithDescriptor);
        }
        free(methods);
    }
    free(classes);
    prepared=YES;
    fprintf(stderr,"lancelot-metal-ca: AIR resource and constants ready; descriptor_methods=%u no_fixed_cache_addresses\n",descriptorCount);
    return 1;
}
