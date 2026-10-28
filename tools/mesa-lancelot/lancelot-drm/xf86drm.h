/* SPDX-License-Identifier: MIT
 * Minimal libdrm API surface for Panfrost on Darwin. No DRM kernel ioctl is used.
 */
#ifndef LANCELOT_XF86DRM_H
#define LANCELOT_XF86DRM_H
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "drm-uapi/drm.h"
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER 2
#define DRM_CLOEXEC O_CLOEXEC
#define DRM_RDWR O_RDWR
#define DRM_DEVICE_GET_PCI_REVISION 1
#define DRM_BUS_PLATFORM 2
typedef struct _drmVersion { int version_major,version_minor,version_patchlevel; int name_len; char *name; int date_len; char *date; int desc_len; char *desc; } drmVersion,*drmVersionPtr;
typedef struct _drmPciBusInfo { uint16_t domain; uint8_t bus,dev,func; } drmPciBusInfo,*drmPciBusInfoPtr;
typedef struct _drmPciDeviceInfo { uint16_t vendor_id,device_id,subvendor_id,subdevice_id; uint8_t revision_id; } drmPciDeviceInfo,*drmPciDeviceInfoPtr;
typedef struct _drmPlatformBusInfo { char fullname[512]; } drmPlatformBusInfo,*drmPlatformBusInfoPtr;
typedef struct _drmPlatformDeviceInfo { char **compatible; } drmPlatformDeviceInfo,*drmPlatformDeviceInfoPtr;
typedef struct _drmDevice { char **nodes; int available_nodes,bustype; union {drmPciBusInfoPtr pci; drmPlatformBusInfoPtr platform;} businfo; union {drmPciDeviceInfoPtr pci; drmPlatformDeviceInfoPtr platform;} deviceinfo; } drmDevice,*drmDevicePtr;
int drmOpenWithType(const char *,const char *,int);
int drmClose(int);
int lancelot_drm_dup(int);
int drmIoctl(int,unsigned long,void *);
int drmCommandWriteRead(int,unsigned long,void *,unsigned long);
int drmCommandWrite(int,unsigned long,const void *,unsigned long);
drmVersionPtr drmGetVersion(int);
void drmFreeVersion(drmVersionPtr);
int drmGetCap(int,uint64_t,uint64_t *);
int drmCloseBufferHandle(int,uint32_t);
int drmPrimeHandleToFD(int,uint32_t,uint32_t,int *);
int drmPrimeFDToHandle(int,int,uint32_t *);
int drmSyncobjCreate(int,uint32_t,uint32_t *);
int drmSyncobjDestroy(int,uint32_t);
int drmSyncobjReset(int,const uint32_t *,uint32_t);
int drmSyncobjSignal(int,const uint32_t *,uint32_t);
int drmSyncobjWait(int,uint32_t *,unsigned,int64_t,unsigned,uint32_t *);
int drmSyncobjFDToHandle(int,int,uint32_t *);
int drmSyncobjHandleToFD(int,uint32_t,int *);
int drmSyncobjImportSyncFile(int,uint32_t,int);
int drmSyncobjExportSyncFile(int,uint32_t,int *);
int drmSyncobjTimelineWait(int,uint32_t *,uint64_t *,unsigned,int64_t,unsigned,uint32_t *);
int drmSyncobjTimelineSignal(int,const uint32_t *,uint64_t *,uint32_t);
int drmSyncobjTransfer(int,uint32_t,uint64_t,uint32_t,uint64_t,uint32_t);
int drmSyncobjQuery(int,uint32_t *,uint64_t *,uint32_t);
int drmGetDevice2(int,uint32_t,drmDevicePtr *);
void drmFreeDevice(drmDevicePtr *);
int drmGetDevices2(uint32_t,drmDevicePtr [],int);
void drmFreeDevices(drmDevicePtr [],int);
char *drmGetDeviceNameFromFd2(int);
char *drmGetRenderDeviceNameFromFd(int);
int drmGetNodeTypeFromFd(int);
void *drmGetHashTable(void);
void *lancelot_bo_map(int,uint32_t,uint64_t,off_t,size_t,int,int,void *);
int lancelot_bo_unmap(int,void *,size_t);
int64_t lancelot_bo_import_size(int,uint32_t);
struct lancelot_job_stats { uint64_t submitted,completed,faults; };
int lancelot_drm_job_stats(int,struct lancelot_job_stats *);
#endif
