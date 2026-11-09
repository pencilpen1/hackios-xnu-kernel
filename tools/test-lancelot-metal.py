"""Exercise production Metal completion, work execution and buffer cleanup."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,shutil,subprocess,sys
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
    script='/mnt/'+R.drive[0].lower()+R.as_posix()[2:]+'/tools/test-lancelot-metal.py'
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',script]))
O=R/'out/lancelot/tests'/('metal-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
O.mkdir(parents=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources={}
core=R/'tools/lancelot-metal/src/metal'
for name in ['rwm.h','cmdbuf.cpp','command.cpp','run.cpp','buffer.cpp','drawable.cpp','drawable.h','texture.cpp']:
    p=core/name;shutil.copy2(p,O/name);sources[p.relative_to(R).as_posix()]=sha(p)
for p in [R/'tools/lancelot-metal/tests/core_test.cpp',R/'tools/lancelot-metal/tests/completion_before.inc',
          R/'tools/lancelot-metal/tests/boot_test.c',R/'tools/lancelot-metal/src/metal/boot_policy.h',
          R/'tools/lancelot-metal/tests/drawable_test.cpp',R/'xnu/files/iokit/Kernel/lancelot_mali_uapi.h',
          R/'tools/lancelot-metal/tests/texture_test.cpp',
          R/'xnu/files/iokit/Kernel/lancelot_mali_queue_uapi.h',
          R/'tools/lancelot-metal/src/lancelot_metal_init.inc',R/'tools/mesa-lancelot/air/air_metal.h',Path(__file__)]:
    shutil.copy2(p,O/p.name);sources[p.relative_to(R).as_posix()]=sha(p)
for name,body in {'mach/mach_types.h':'#pragma once\n#include <stdint.h>\ntypedef uint32_t mach_port_t;\n',
                  'IOSurface/IOSurfaceRef.h':'''#pragma once
#include <stddef.h>
#include <stdint.h>
struct IOSurface;typedef struct IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceLockOptions;
#define kIOSurfaceLockReadOnly 1u
extern "C" {
const void*CFRetain(const void*);void CFRelease(const void*);
int IOSurfaceLock(IOSurfaceRef,uint32_t,uint32_t*);int IOSurfaceUnlock(IOSurfaceRef,uint32_t,uint32_t*);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef);void*IOSurfaceGetBaseAddress(IOSurfaceRef);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef);size_t IOSurfaceGetWidth(IOSurfaceRef);size_t IOSurfaceGetHeight(IOSurfaceRef);
size_t IOSurfaceGetBytesPerElement(IOSurfaceRef);size_t IOSurfaceGetAllocSize(IOSurfaceRef);
void*IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef,size_t);size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef,size_t);
size_t IOSurfaceGetWidthOfPlane(IOSurfaceRef,size_t);size_t IOSurfaceGetHeightOfPlane(IOSurfaceRef,size_t);
size_t IOSurfaceGetBytesPerElementOfPlane(IOSurfaceRef,size_t);uint32_t IOSurfaceGetSeed(IOSurfaceRef);
}
''',
                  'sys/sysctl.h':'#pragma once\n#include <stddef.h>\nstatic inline int sysctlbyname(const char*,void*,size_t*,void*,size_t){return 0;}\n'}.items():
    p=O/'host'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
for name,body in {'CoreFoundation/CoreFoundation.h':'#pragma once\n',
 'IOKit/IOReturn.h':'#pragma once\n#define kIOReturnSuccess 0\n',
 'IOKit/IOKitLib.h':'''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t io_connect_t;typedef uint32_t io_service_t;typedef int kern_return_t;
#define IO_OBJECT_NULL 0
#define KERN_SUCCESS 0
#define kIOReturnUnsupported (-95)
#define kIOReturnNotFound (-2)
extern "C" {void*IOServiceMatching(const char*);io_service_t IOServiceGetMatchingService(unsigned,void*);
kern_return_t IOServiceOpen(io_service_t,unsigned,unsigned,io_connect_t*);kern_return_t IOServiceClose(io_connect_t);
kern_return_t IOObjectRelease(unsigned);unsigned mach_task_self(void);
kern_return_t IOConnectCallStructMethod(io_connect_t,unsigned,const void*,size_t,void*,size_t*);}
'''}.items():
    p=O/'host'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
runs=[]
for cc in ['g++','clang++']:
    binary=O/cc
    commands=[('build',[cc,'-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-pthread','-I'+str(O/'host'),'-I'+str(O),
        *[O/name for name in ['core_test.cpp','cmdbuf.cpp','command.cpp','run.cpp','buffer.cpp']],'-o',binary]),
        ('run',[binary]),('async-run',[binary,'async'])]
    c='gcc' if cc=='g++' else 'clang';boot=O/(c+'-boot')
    commands += [('boot-build',[c,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-fsanitize=address,undefined','-I'+str(O),O/'boot_test.c','-o',boot]),('boot-run',[boot])]
    drawable=O/(cc+'-drawable')
    commands += [('drawable-build',[cc,'-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-Wno-multichar',
        '-fsanitize=address,undefined','-pthread','-Ddlopen=lancelot_model_dlopen','-Ddlsym=lancelot_model_dlsym',
        '-I'+str(O/'host'),'-I'+str(O),O/'drawable_test.cpp',O/'drawable.cpp','-o',drawable]),('drawable-run',[drawable])]
    texture=O/(cc+'-texture')
    commands += [('texture-build',[cc,'-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-pthread','-I'+str(O/'host'),'-I'+str(O),
        O/'texture_test.cpp',O/'texture.cpp','-o',texture]),('texture-run',[texture])]
    for stage,command in commands:
        p=subprocess.run(list(map(str,command)),capture_output=True,timeout=90,
             env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
        log=O/(cc+'-'+stage+'.log');log.write_bytes(p.stdout+p.stderr)
        runs.append({'stage':cc+'-'+stage,'exit':p.returncode,'log':log.name,'sha256':sha(log)})
        print(cc,stage,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-3000:],flush=True)
        if p.returncode:raise SystemExit(p.returncode)
assert all(sha(R/name)==value for name,value in sources.items())
record={'passed':True,'sources':sources,'runs':runs,'actual_production_core':True,
        'old_false_completion_reproduced':True,'timeout_errors_propagated':True,
        'concurrent_failed_wait_cannot_report_success':True,'encoded_work_failure_propagated':True,
        'native_Mesa_buffers_and_cleanup':True,'Metal_boot_admission_and_flag_parsing':True,
        'native_drawable_query_and_import_exercised':True,
        'actual_surface_copies_and_bounds_verified':True,
        'device_access':False,'hardware_verified':False}
(O/'result.json').write_text(json.dumps(record,indent=2)+'\n')
print('METAL CORE PASS',O,flush=True)
