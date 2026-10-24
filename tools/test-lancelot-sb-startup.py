#!/usr/bin/env python3
"""ASan/UBSan tests of actual startup guards and original-preserving probes."""
import argparse
from pathlib import Path
import subprocess
import tempfile

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define __unused __attribute__((unused))
typedef void *id;
typedef const char *SEL;
typedef bool BOOL;
typedef void (*IMP)(void);
typedef struct { IMP imp; const char *types; } FakeMethod;
typedef FakeMethod *Method;
typedef const char *Class;
typedef struct { const char *name, *types; ptrdiff_t offset; } FakeIvar;
typedef FakeIvar *Ivar;
typedef struct { const char *dli_fname; void *dli_fbase; } Dl_info;
struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct uuid_command { uint32_t cmd, cmdsize; unsigned char uuid[16]; };
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_ARM64 0x100000c
#define LC_UUID 0x1b
static uintptr_t slide = 0x161f8000;
static struct { struct mach_header_64 header; struct uuid_command uuid[2]; } image;
static FakeMethod method;
static FakeMethod probe_methods[6];
static bool automatic_tables;
static FakeIvar ivars[] = {{"_state","i",8},{"_settingUpActivationState","B",12},
 {"_setupRequiredReason","Q",8},{"_inSetupModeReadyToExit","B",16},{"_setupWantedForDeviceMigration","B",17}};
static const char *args, *path;
static size_t object_size;
static bool missing_class, missing_method, missing_ivar, lookup;
static int sysctl_error, replacements;
static void *base;
static void say(const char *format, ...) { (void)format; }
static int sysctlbyname(const char *n, void *out, size_t *s, void *in, size_t z) {
 (void)n;(void)in;(void)z;if(sysctl_error)return sysctl_error;
 size_t len=strlen(args);if(len>*s)len=*s;memcpy(out,args,len);*s=len;return 0;
}
static int dladdr(const void *imp, Dl_info *out) { (void)imp;out->dli_fname=path;out->dli_fbase=base;return lookup; }
static Class objc_getClass(const char *n) { return missing_class ? NULL : n; }
static SEL sel_registerName(const char *s) { return s; }
static Method class_method(Class c, SEL s, bool instance) {
 (void)c;if(missing_method)return NULL;if(!automatic_tables)return &method;
 const char *selectors[]={"_setupActivationState","_setSetupRequiredReason:","init",
  "initializeSystemServicesBeforeApplicationDidFinishLaunching","applicationDidFinishLaunching:","_completeStartupAfterMainSceneConnect:"};
 for(unsigned i=0;i<6;i++)if(!strcmp(s,selectors[i]) && instance==(i!=3))return &probe_methods[i];return NULL;
}
static Method class_getInstanceMethod(Class c,SEL s) { return class_method(c,s,true); }
static Method class_getClassMethod(Class c,SEL s) { return class_method(c,s,false); }
static IMP method_getImplementation(Method m) { return m->imp; }
static const char *method_getTypeEncoding(Method m) { return m->types; }
static void method_setImplementation(Method m, IMP p) { m->imp=p;replacements++; }
static size_t class_getInstanceSize(Class c) { return automatic_tables && !strcmp(c,"SBLockdownManager") ? 32 : object_size; }
static Ivar class_getInstanceVariable(Class c,const char *n) {
 (void)c;if(missing_ivar)return NULL;
 for(unsigned i=0;i<5;i++)if(!strcmp(n,ivars[i].name))return &ivars[i];return NULL;
}
static ptrdiff_t ivar_getOffset(Ivar v) { return v->offset; }
static const char *ivar_getTypeEncoding(Ivar v) { return v->types; }
typedef void *CFRunLoopObserverRef;
typedef unsigned long CFRunLoopActivity;
#define kCFRunLoopBeforeWaiting 32
#define kCFRunLoopCommonModes NULL
static CFRunLoopObserverRef CFRunLoopObserverCreate(void *a,CFRunLoopActivity b,bool c,long d,
 void (*e)(CFRunLoopObserverRef,CFRunLoopActivity,void*),void *f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return NULL; }
static void *CFRunLoopGetMain(void) { return NULL; }
static void CFRunLoopAddObserver(void *a,CFRunLoopObserverRef b,void *c) { (void)a;(void)b;(void)c; }
static void CFRelease(void *v) { (void)v; }
#define dispatch_async(queue,block) ((void)0)
'''

SUFFIX = r'''
static void reset(void) {
 memset(&image,0,sizeof(image));image.header.magic=MH_MAGIC_64;image.header.cputype=CPU_TYPE_ARM64;
 image.header.ncmds=1;image.header.sizeofcmds=sizeof(image.uuid[0]);image.uuid[0].cmd=LC_UUID;
 image.uuid[0].cmdsize=sizeof(image.uuid[0]);
 const unsigned char uuid[16]={0x22,0x03,0x4a,0xba,0x9a,0x9a,0x3b,0x94,0x93,0xa5,0xa0,0xde,0x02,0xbe,0xfd,0x2e};
 memcpy(image.uuid[0].uuid,uuid,16);base=&image;lookup=true;
 path="/System/Library/PrivateFrameworks/SpringBoard.framework/SpringBoard";
 args="cpus=1 sbstartup=observe gpu=0";object_size=48;
 method.imp=(IMP)(0x1c5aa608cUL+slide);method.types="B24@0:8Q16";
 missing_class=missing_method=missing_ivar=false;sysctl_error=0;replacements=0;
 automatic_tables=false;
 const uintptr_t addresses[]={0x1c5991a78UL,0x1c5aa608cUL,0x1c587f834UL,0x1c587f5f4UL,0x1c58808a0UL,0x1c58813c8UL};
 const char *types[]={"v16@0:8","B24@0:8Q16","@16@0:8","v16@0:8","v24@0:8@16","v24@0:8@16"};
 for(unsigned i=0;i<6;i++){probe_methods[i].imp=(IMP)(addresses[i]+slide);probe_methods[i].types=types[i];}
 ivars[2].types="Q";ivars[2].offset=8;
}
static bool valid(void) {
 static const struct sb_startup_ivar check={"_setupRequiredReason","Q",8};
 return sb_startup_verified_method('-',"SBSetupManager","_setSetupRequiredReason:",0x1c5aa608cUL,"B24@0:8Q16",48,&check,1)!=NULL;
}
static unsigned forwarded;
static id expected_self, expected_arg;
static SEL expected_selector;
static unsigned long expected_reason;
static BOOL answer;
static void activation(id self, SEL sel) {
 assert(self==expected_self && sel==expected_selector);forwarded++;
 assert(*(int*)((char*)self+8)==3 && *((unsigned char*)self+12)==1);
 *(int*)((char*)self+8)=1;*((unsigned char*)self+12)=0;
}
static BOOL setup_call(id self,SEL sel,unsigned long reason) {
 assert(self==expected_self && sel==expected_selector && reason==expected_reason);forwarded++;
 *(unsigned long*)((char*)self+8)=reason;return answer;
}
static id init_call(id self,SEL sel) { assert(self==expected_self && sel==expected_selector);forwarded++;return expected_arg; }
static void services_call(id self,SEL sel) { assert(self==expected_self && sel==expected_selector);forwarded++; }
static void phase_call(id self,SEL sel,id arg) { assert(self==expected_self && sel==expected_selector && arg==expected_arg);forwarded++; }
int main(void) {
 reset();assert(valid());
 missing_class=true;assert(!valid());reset();missing_method=true;assert(!valid());
 reset();method.imp=(IMP)((uintptr_t)method.imp+4);assert(!valid());
 reset();method.imp=NULL;assert(!valid());
 reset();method.types="v24@0:8q16";assert(!valid());reset();method.types=NULL;assert(!valid());
 reset();object_size=56;assert(!valid());reset();missing_ivar=true;assert(!valid());
 reset();ivars[2].offset=16;assert(!valid());reset();ivars[2].types="q";assert(!valid());
 reset();ivars[2].types=NULL;assert(!valid());
 reset();lookup=false;assert(!valid());reset();base=NULL;assert(!valid());
 reset();path=NULL;assert(!valid());reset();path="/wrong/SpringBoard";assert(!valid());
 reset();image.header.magic=0;assert(!valid());reset();image.header.cputype=7;assert(!valid());
 reset();image.header.ncmds=257;assert(!valid());reset();image.header.sizeofcmds=65537;assert(!valid());
 reset();image.header.sizeofcmds=4;assert(!valid());reset();image.uuid[0].cmdsize=4;assert(!valid());
 reset();image.uuid[0].cmdsize=28;assert(!valid());reset();image.uuid[0].cmdsize=8;assert(!valid());
 reset();image.uuid[0].uuid[0]^=1;assert(!valid());reset();image.uuid[0].cmd=0;assert(!valid());
 reset();image.header.ncmds=0;assert(!valid());
 reset();image.header.sizeofcmds++;assert(!valid());
 reset();image.header.ncmds=2;image.header.sizeofcmds=48;image.uuid[1]=image.uuid[0];assert(!valid());
 reset();assert(sb_startup_observe_enabled());
 for(unsigned i=0;i<6;i++) {
  const char *bad[]={"sbstartup=observeX","Xsbstartup=observe","sbstartup=compat","","nosbstartup=observe","sbstartup=observe=1"};
  args=bad[i];assert(!sb_startup_observe_enabled());assert(sb_startup_install()==0);assert(replacements==0);
 }
 reset();sysctl_error=5;assert(!sb_startup_observe_enabled());
 reset();args="sbstartup=observe";assert(sb_startup_observe_enabled());
 reset();method.types="wrong";assert(sb_startup_install()==0 && replacements==0);
 reset();automatic_tables=true;assert(sb_startup_install()==6 && replacements==6);
 assert(probe_methods[0].imp==(IMP)sb_startup_activation && probe_methods[1].imp==(IMP)sb_startup_setup);
 assert((uintptr_t)sb_startup_original_activation==0x1c5991a78UL+slide);
 assert((uintptr_t)sb_startup_original_setup==0x1c5aa608cUL+slide);
 reset();automatic_tables=true;
 assert(sb_startup_verified_method('-',"SpringBoard","initializeSystemServicesBeforeApplicationDidFinishLaunching",0x1c587f5f4UL,"v16@0:8",0,NULL,0)==NULL);
 /* Execute only our wrappers against real host functions; never execute fake cache IMPs. */
 union { unsigned long align; unsigned char bytes[48]; } obj;memset(&obj,0,sizeof(obj));
 expected_self=&obj;expected_selector="fixture";forwarded=0;
 *(int*)(obj.bytes+8)=3;obj.bytes[12]=1;sb_startup_original_activation=activation;
 sb_startup_activation(expected_self,expected_selector);
 assert(forwarded==1 && *(int*)(obj.bytes+8)==1 && obj.bytes[12]==0);
 sb_startup_original_setup=setup_call;
 for(unsigned i=0;i<4;i++) {
  expected_reason=i==3 ? ~0UL : i;answer=(i&1)!=0;unsigned before=forwarded;
  assert(sb_startup_setup(expected_self,expected_selector,expected_reason)==answer);
  assert(forwarded==before+1 && *(unsigned long*)(obj.bytes+8)==expected_reason);
 }
 sb_startup_original_init=init_call;expected_arg=NULL;assert(sb_startup_init(expected_self,expected_selector)==NULL);
 expected_arg=(id)0x1234;assert(sb_startup_init(expected_self,expected_selector)==expected_arg);
 sb_startup_original_services=services_call;sb_startup_services(expected_self,expected_selector);
 sb_startup_original_did_launch=phase_call;sb_startup_did_launch(expected_self,expected_selector,expected_arg);
 sb_startup_original_scene=phase_call;sb_startup_scene(expected_self,expected_selector,expected_arg);
 assert(forwarded==10);
 puts("startup UUID/IMP/ABI/layout/token guard and original forwarding fixtures passed");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, required=True)
    args = ap.parse_args()
    source = args.source.read_text().replace("#include <mach-o/loader.h>", "")
    with tempfile.TemporaryDirectory(prefix="sb-startup-") as directory:
        path = Path(directory)
        (path / "fixture.c").write_text(PREFIX + source + SUFFIX)
        subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                        str(path / "fixture.c"), "-o", str(path / "fixture")], check=True)
        subprocess.run([str(path / "fixture")], check=True)


if __name__ == "__main__":
    main()
