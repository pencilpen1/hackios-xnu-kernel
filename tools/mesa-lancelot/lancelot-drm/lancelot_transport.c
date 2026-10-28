/* SPDX-License-Identifier: MIT */
#include "lancelot_transport.h"
#include "lancelot_mali_uapi.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <time.h>
#include <limits.h>
/* Mesa uses CLOCK_MONOTONIC; LMAL wire deadlines use mach_absolute_time ns.
 * Convert through remaining duration, never assume the clocks share an epoch. */
static uint64_t wire_deadline(uint64_t absolute)
{
 struct timespec now;
 if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
 uint64_t mono=(uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
 uint64_t remaining=absolute>mono?absolute-mono:0;
 if(remaining>LMAL_MAX_WAIT_NS)remaining=LMAL_MAX_WAIT_NS;
 mach_timebase_info_data_t tb;
 if(mach_timebase_info(&tb)!=KERN_SUCCESS||!tb.denom)return 0;
 uint64_t raw=(uint64_t)(((__uint128_t)mach_absolute_time()*tb.numer)/tb.denom);
 return raw>UINT64_MAX-remaining?UINT64_MAX:raw+remaining;
}
static int kr_errno(kern_return_t kr) {
 switch (kr) {
 case kIOReturnNoMemory: errno=ENOMEM; break;
 case kIOReturnNoDevice: case kIOReturnNotFound: errno=ENODEV; break;
 case kIOReturnBadArgument: errno=EINVAL; break;
 case kIOReturnUnsupported: errno=ENOTSUP; break;
 case kIOReturnTimeout: errno=ETIMEDOUT; break;
 case kIOReturnBusy: errno=EBUSY; break;
 case kIOReturnNotPermitted: errno=EPERM; break;
 default: errno=EIO;
 }
 return -1;
}
int lm_transport_open(uintptr_t *out) {
 io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching(LMAL_SERVICE_NAME));
 if (!service) {fprintf(stderr,"LMAL: no service %s in the registry\n",LMAL_SERVICE_NAME);errno=ENODEV;return -1;}
 io_connect_t conn=IO_OBJECT_NULL;
 kern_return_t kr=IOServiceOpen(service,mach_task_self(),LMAL_OPEN_TYPE,&conn);
 IOObjectRelease(service);
 if(kr!=KERN_SUCCESS){fprintf(stderr,"LMAL: IOServiceOpen failed kr=%#x\n",(unsigned)kr);return kr_errno(kr);}
 fprintf(stderr,"LMAL: service opened\n");
 *out=conn;return 0;
}
void lm_transport_close(uintptr_t c){IOServiceClose((io_connect_t)c);}
int lm_transport_call(uintptr_t c,uint32_t selector,void *p,size_t n){
 if(selector==LMAL_SELECTOR_BO_WAIT&&n==sizeof(struct lmal_bo_wait)){
  struct lmal_bo_wait*q=p;q->absolute_monotonic_ns=wire_deadline(q->absolute_monotonic_ns);
 }
 if(selector==LMAL_SELECTOR_SYNCOBJ_WAIT&&n==sizeof(struct lmal_syncobj_wait)){
  struct lmal_syncobj_wait*q=p;q->absolute_monotonic_ns=wire_deadline(q->absolute_monotonic_ns);
 }
 size_t out=n;
 kern_return_t kr=IOConnectCallStructMethod((io_connect_t)c,selector,p,n,p,&out);
 if(kr!=KERN_SUCCESS)return kr_errno(kr);
 if(out!=n){errno=EPROTO;return -1;}
 return 0;
}
int lm_transport_map(uintptr_t c,uint32_t handle,uintptr_t *address,uint64_t *size){
 mach_vm_address_t a=0;mach_vm_size_t n=0;
 kern_return_t kr=IOConnectMapMemory64((io_connect_t)c,handle,mach_task_self(),&a,&n,kIOMapAnywhere);
 if(kr!=KERN_SUCCESS)return kr_errno(kr);
 *address=(uintptr_t)a;*size=n;return 0;
}
int lm_transport_unmap(uintptr_t c,uint32_t handle,uintptr_t a){
 kern_return_t kr=IOConnectUnmapMemory64((io_connect_t)c,handle,mach_task_self(),(mach_vm_address_t)a);
 return kr==KERN_SUCCESS?0:kr_errno(kr);
}
