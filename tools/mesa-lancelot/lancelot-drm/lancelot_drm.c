/* SPDX-License-Identifier: MIT
 * Panfrost Darwin backend. Linux ioctl layouts terminate here: the only wire
 * messages sent to IOKit are the independent, size-checked LMAL v1 structures.
 */
#include "xf86drm.h"
#include "lancelot_transport.h"
#include "lancelot_mali_uapi.h"
#include "drm-uapi/panfrost_drm.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct view {void *addr;size_t size;struct view *next;};
struct mapping {uint32_t handle;uintptr_t base;uint64_t size;bool closing,cpu_unmapped;struct view *views;struct mapping *next;};
struct device {uintptr_t conn;unsigned refs;pthread_mutex_t maps_lock;struct mapping *maps;};
struct fd_entry {int fd;struct device *dev;};
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static struct fd_entry registry[128];
static void put(struct device *d);
static struct device *get(int fd){
 struct device *d=NULL;
 pthread_mutex_lock(&registry_lock);
 for(unsigned i=0;i<128;i++)if(registry[i].dev&&registry[i].fd==fd){d=registry[i].dev;d->refs++;break;}
 pthread_mutex_unlock(&registry_lock);
 if(!d)errno=EBADF;
 return d;
}
static struct lmal_header header(size_t n){return (struct lmal_header){.magic=LMAL_UAPI_MAGIC,.version=LMAL_UAPI_VERSION,.size=(uint16_t)n};}
static int error_result(int32_t e){
 switch(e){case LMAL_EINVAL:errno=EINVAL;break;case LMAL_ENOMEM:errno=ENOMEM;break;
 case LMAL_EBUSY:errno=EBUSY;break;case LMAL_ENODEV:errno=ENODEV;break;
 case LMAL_ENOTSUP:errno=ENOTSUP;break;case LMAL_ETIMEDOUT:errno=ETIMEDOUT;break;
 case LMAL_EFAULT:errno=EFAULT;break;case LMAL_EPERM:errno=EPERM;break;default:errno=EIO;}
 return -1;
}
static int call(struct device *d,uint32_t selector,void *data,size_t n){
 struct lmal_header *h=data;*h=header(n);
 if(lm_transport_call(d->conn,selector,data,n))return -1;
 if(h->magic!=LMAL_UAPI_MAGIC||h->version!=LMAL_UAPI_VERSION||h->size!=n||h->reserved0||h->reserved1){errno=EPROTO;return -1;}
 return h->result?error_result(h->result):0;
}
static int fdcall(int fd,uint32_t selector,void *data,size_t n){struct device*d=get(fd);if(!d)return -1;int ret=call(d,selector,data,n);put(d);return ret;}
static int close_handle(struct device*d,uint32_t handle){struct lmal_bo_close q={.handle=handle};return call(d,LMAL_SELECTOR_BO_CLOSE,&q,sizeof(q));}
static int unmap_ack(struct device*d,uint32_t handle){struct lmal_bo_close q={.handle=handle};return call(d,LMAL_SELECTOR_BO_UNMAP,&q,sizeof(q));}
static void put(struct device*d){
 pthread_mutex_lock(&registry_lock);bool dead=!--d->refs;pthread_mutex_unlock(&registry_lock);
 if(!dead)return;
 struct mapping*m=d->maps;
 while(m){struct mapping*next=m->next;if(m->cpu_unmapped||!lm_transport_unmap(d->conn,m->handle,m->base))unmap_ack(d,m->handle);while(m->views){struct view*v=m->views;m->views=v->next;free(v);}free(m);m=next;}
 lm_transport_close(d->conn);pthread_mutex_destroy(&d->maps_lock);free(d);
}
static int register_fd(struct device*d,int fd){
 int ret=-1;pthread_mutex_lock(&registry_lock);
 for(unsigned i=0;i<128;i++)if(!registry[i].dev){registry[i]=(struct fd_entry){fd,d};d->refs++;ret=fd;break;}
 pthread_mutex_unlock(&registry_lock);if(ret<0)errno=EMFILE;return ret;
}
int drmOpenWithType(const char*name,const char*busid,int type){
 (void)busid;if(!name||strcmp(name,"panfrost")||type!=DRM_NODE_RENDER){fprintf(stderr,"LMAL: drmOpenWithType name=%s type=%d rejected\n",name?name:"(null)",type);errno=ENODEV;return -1;}
 struct device*d=calloc(1,sizeof(*d));if(!d){errno=ENOMEM;return -1;}
 if(lm_transport_open(&d->conn)){fprintf(stderr,"LMAL: transport open failed errno=%d\n",errno);free(d);return -1;}
 d->refs=1;pthread_mutex_init(&d->maps_lock,NULL);
 struct lmal_caps caps={0};
 if(call(d,LMAL_SELECTOR_GET_CAPS,&caps,sizeof(caps))){fprintf(stderr,"LMAL: GET_CAPS failed errno=%d\n",errno);put(d);return -1;}
 int fd=open("/dev/null",O_RDWR|O_CLOEXEC);if(fd<0){fprintf(stderr,"LMAL: /dev/null open failed errno=%d\n",errno);put(d);return -1;}
 int ret=register_fd(d,fd);if(ret<0)close(fd);put(d);fprintf(stderr,"LMAL: panfrost device fd=%d\n",ret);return ret;
}
int lancelot_drm_dup(int fd){struct device*d=get(fd);if(!d)return -1;int n=fcntl(fd,F_DUPFD_CLOEXEC,3);if(n>=0&&register_fd(d,n)<0){close(n);n=-1;}put(d);return n;}
int drmClose(int fd){struct device*d=NULL;pthread_mutex_lock(&registry_lock);for(unsigned i=0;i<128;i++)if(registry[i].dev&&registry[i].fd==fd){d=registry[i].dev;registry[i].dev=NULL;break;}pthread_mutex_unlock(&registry_lock);if(!d){errno=EBADF;return -1;}int ret=close(fd);put(d);return ret;}
drmVersionPtr drmGetVersion(int fd){struct device*d=get(fd);if(!d)return NULL;put(d);drmVersionPtr v=calloc(1,sizeof(*v));if(!v)return NULL;v->version_major=1;v->version_minor=1;v->name=strdup("panfrost");v->name_len=8;v->date=strdup("");v->desc=strdup("LancelotMali v1 Darwin");if(!v->name||!v->date||!v->desc){drmFreeVersion(v);return NULL;}return v;}
void drmFreeVersion(drmVersionPtr v){if(v){free(v->name);free(v->date);free(v->desc);free(v);}}
int drmGetCap(int fd,uint64_t cap,uint64_t*value){struct device*d=get(fd);if(!d)return -1;put(d);if(!value){errno=EINVAL;return -1;}*value=(cap==DRM_CAP_SYNCOBJ);return 0;}
int drmCloseBufferHandle(int fd,uint32_t handle){struct device*d=get(fd);if(!d)return -1;pthread_mutex_lock(&d->maps_lock);struct mapping*m=d->maps;for(;m&&m->handle!=handle;m=m->next){}int ret=0;if(m)m->closing=true;else ret=close_handle(d,handle);pthread_mutex_unlock(&d->maps_lock);put(d);return ret;}
int64_t lancelot_bo_import_size(int fd,uint32_t handle){struct lmal_bo_query q={.handle=handle};if(fdcall(fd,LMAL_SELECTOR_BO_QUERY,&q,sizeof(q)))return -1;if(q.size>INT64_MAX){errno=EOVERFLOW;return -1;}return(int64_t)q.size;}
void*lancelot_bo_map(int fd,uint32_t handle,uint64_t bo_size,off_t offset,size_t length,int prot,int flags,void*fixed){
 if(offset<0||!length||(uint64_t)offset>bo_size||length>bo_size-(uint64_t)offset||fixed||(flags&MAP_FIXED)||!(flags&MAP_SHARED)||(prot&PROT_EXEC)||!(prot&(PROT_READ|PROT_WRITE))){errno=EINVAL;return MAP_FAILED;}
 struct device*d=get(fd);if(!d)return MAP_FAILED;
 pthread_mutex_lock(&d->maps_lock);struct mapping*m=d->maps;for(;m&&m->handle!=handle;m=m->next){}
 if(m&&(m->closing||m->cpu_unmapped)){errno=EINVAL;goto fail;}
 if(!m){
  struct lmal_bo_query q={.handle=handle};
  if(call(d,LMAL_SELECTOR_BO_QUERY,&q,sizeof(q)))goto fail;
  if(q.size!=bo_size||q.map_type!=handle){errno=EPROTO;goto fail;}
  m=calloc(1,sizeof(*m));if(!m){errno=ENOMEM;goto fail;}
  m->handle=handle;
  if(lm_transport_map(d->conn,q.map_type,&m->base,&m->size)){free(m);goto fail;}
  if(m->size!=bo_size){if(!lm_transport_unmap(d->conn,q.map_type,m->base))unmap_ack(d,m->handle);free(m);errno=EPROTO;goto fail;}
  m->next=d->maps;d->maps=m;
 }
 struct view*v=malloc(sizeof(*v));if(!v){errno=ENOMEM;goto fail;}
 *v=(struct view){(void*)(m->base+(uintptr_t)offset),length,m->views};m->views=v;
 void*result=v->addr;pthread_mutex_unlock(&d->maps_lock);put(d);return result;
fail: pthread_mutex_unlock(&d->maps_lock);put(d);return MAP_FAILED;
}
int lancelot_bo_unmap(int fd,void*address,size_t length){
 struct device*d=get(fd);if(!d)return -1;int ret=-1;errno=EINVAL;
 pthread_mutex_lock(&d->maps_lock);
 struct mapping**mp=&d->maps;
 while(*mp){
  struct mapping*m=*mp;struct view**vp=&m->views;
  while(*vp){
   struct view*v=*vp;
   if(v->addr==address&&v->size==length){
    if(m->views==v&&!v->next){
     if(!m->cpu_unmapped){
      ret=lm_transport_unmap(d->conn,m->handle,m->base);
      if(ret)goto done;
      m->cpu_unmapped=true;
     }
     ret=unmap_ack(d,m->handle);
     if(ret)goto done;
     *mp=m->next;if(m->closing)ret=close_handle(d,m->handle);free(v);free(m);
    }else{*vp=v->next;free(v);ret=0;}
    goto done;
   }
   vp=&v->next;
  }
  mp=&m->next;
 }
done:pthread_mutex_unlock(&d->maps_lock);put(d);return ret;
}
int drmIoctl(int fd,unsigned long request,void*arg){
 if(!arg){errno=EINVAL;return -1;}
 if(request==DRM_IOCTL_PANFROST_GET_PARAM){struct drm_panfrost_get_param*p=arg;if(p->pad){errno=EINVAL;return -1;}struct lmal_get_param q={.param=p->param};int r=fdcall(fd,LMAL_SELECTOR_GET_PARAM,&q,sizeof(q));if(!r)p->value=q.value;return r;}
 if(request==DRM_IOCTL_PANFROST_CREATE_BO){struct drm_panfrost_create_bo*p=arg;if(p->flags&~(PANFROST_BO_NOEXEC|PANFROST_BO_HEAP)){errno=EINVAL;return -1;}struct lmal_bo_create q={.size=p->size,.flags=LMAL_BO_READ|LMAL_BO_WRITE|LMAL_BO_FIXED_BACKING|((p->flags&PANFROST_BO_NOEXEC)?LMAL_BO_NOEXEC:LMAL_BO_EXEC)|((p->flags&PANFROST_BO_HEAP)?LMAL_BO_HEAP:0)};int r=fdcall(fd,LMAL_SELECTOR_BO_CREATE,&q,sizeof(q));if(!r){if(!q.handle||q.actual_size<p->size||q.actual_size>UINT32_MAX){errno=EPROTO;return -1;}p->handle=q.handle;p->offset=q.gpu_va;p->size=(uint32_t)q.actual_size;}return r;}
 if(request==DRM_IOCTL_PANFROST_GET_BO_OFFSET){struct drm_panfrost_get_bo_offset*p=arg;struct lmal_bo_query q={.handle=p->handle};int r=fdcall(fd,LMAL_SELECTOR_BO_QUERY,&q,sizeof(q));if(!r)p->offset=q.gpu_va;return r;}
 if(request==DRM_IOCTL_PANFROST_MMAP_BO){errno=ENOTSUP;return -1;} /* No mmap device on Darwin. */
 if(request==DRM_IOCTL_PANFROST_WAIT_BO){struct drm_panfrost_wait_bo*p=arg;struct lmal_bo_wait q={.handle=p->handle,.flags=LMAL_BO_WAIT_READ|LMAL_BO_WAIT_WRITE,.absolute_monotonic_ns=p->timeout_ns};return fdcall(fd,LMAL_SELECTOR_BO_WAIT,&q,sizeof(q));}
 if(request==DRM_IOCTL_PANFROST_MADVISE){struct drm_panfrost_madvise*p=arg;if(p->madv!=PANFROST_MADV_WILLNEED&&p->madv!=PANFROST_MADV_DONTNEED){errno=EINVAL;return -1;}struct lmal_bo_query q={.handle=p->handle};int r=fdcall(fd,LMAL_SELECTOR_BO_QUERY,&q,sizeof(q));if(!r)p->retained=1;return r;}
 if(request==DRM_IOCTL_PANFROST_SUBMIT){struct drm_panfrost_submit*p=arg;if(p->bo_handle_count>LMAL_MAX_SUBMIT_BOS||p->in_sync_count>LMAL_MAX_SUBMIT_SYNCS||!p->bo_handle_count||!p->bo_handles||(p->in_sync_count&&!p->in_syncs)){errno=EINVAL;return -1;}struct lmal_submit q={.jc=p->jc,.requirements=p->requirements,.bo_count=p->bo_handle_count,.bo_handles=p->bo_handles,.in_sync_count=p->in_sync_count,.in_syncs=p->in_syncs,.out_sync=p->out_sync,.timeout_ns=LMAL_MAX_WAIT_NS};return fdcall(fd,LMAL_SELECTOR_SUBMIT,&q,sizeof(q));}
 if(request==DRM_IOCTL_GEM_CLOSE)return drmCloseBufferHandle(fd,((struct drm_gem_close*)arg)->handle);
 errno=ENOTSUP;return -1;
}
int drmCommandWriteRead(int fd,unsigned long index,void*p,unsigned long n){(void)fd;(void)index;(void)p;(void)n;errno=ENOTSUP;return -1;}
int drmCommandWrite(int fd,unsigned long index,const void*p,unsigned long n){return drmCommandWriteRead(fd,index,(void*)p,n);}
int drmSyncobjCreate(int fd,uint32_t flags,uint32_t*handle){if(!handle||flags&~DRM_SYNCOBJ_CREATE_SIGNALED){errno=EINVAL;return -1;}struct lmal_syncobj_create q={.flags=(flags&DRM_SYNCOBJ_CREATE_SIGNALED)?LMAL_SYNCOBJ_CREATE_SIGNALED:0};int r=fdcall(fd,LMAL_SELECTOR_SYNCOBJ_CREATE,&q,sizeof(q));if(!r)*handle=q.handle;return r;}
int drmSyncobjDestroy(int fd,uint32_t handle){struct lmal_syncobj_handle q={.handle=handle};return fdcall(fd,LMAL_SELECTOR_SYNCOBJ_DESTROY,&q,sizeof(q));}
static int sync_array(int fd,uint32_t sel,const uint32_t*h,uint32_t n){if(!h||!n||n>LMAL_MAX_SUBMIT_SYNCS){errno=EINVAL;return -1;}struct lmal_syncobj_array q={.handles=(uintptr_t)h,.count=n};return fdcall(fd,sel,&q,sizeof(q));}
int drmSyncobjReset(int fd,const uint32_t*h,uint32_t n){return sync_array(fd,LMAL_SELECTOR_SYNCOBJ_RESET,h,n);}
int drmSyncobjSignal(int fd,const uint32_t*h,uint32_t n){return sync_array(fd,LMAL_SELECTOR_SYNCOBJ_SIGNAL,h,n);}
int drmSyncobjWait(int fd,uint32_t*h,unsigned n,int64_t timeout,unsigned flags,uint32_t*first){if(!h||!n||n>LMAL_MAX_SUBMIT_SYNCS||flags&~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL|DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)){errno=EINVAL;return -1;}struct lmal_syncobj_wait q={.handles=(uintptr_t)h,.count=n,.flags=((flags&DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)?LMAL_SYNC_WAIT_ALL:0)|((flags&DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)?LMAL_SYNC_WAIT_FOR_SUBMIT:0),.absolute_monotonic_ns=(uint64_t)timeout};int r=fdcall(fd,LMAL_SELECTOR_SYNCOBJ_WAIT,&q,sizeof(q));if(!r&&first)*first=q.first_signaled;return r;}
int lancelot_drm_job_stats(int fd,struct lancelot_job_stats*s){if(!s){errno=EINVAL;return -1;}struct lmal_caps q={0};int r=fdcall(fd,LMAL_SELECTOR_GET_CAPS,&q,sizeof(q));if(!r)*s=(struct lancelot_job_stats){q.jobs_submitted,q.jobs_completed,q.faults};return r;}
/* There is no Unix sync_file/dma-buf export in v1. Refuse, never invent an fd. */
#define UNSUPPORTED() do{errno=ENOTSUP;return -1;}while(0)
int drmPrimeHandleToFD(int a,uint32_t b,uint32_t c,int*d){(void)a;(void)b;(void)c;(void)d;UNSUPPORTED();}
int drmPrimeFDToHandle(int fd,int surface,uint32_t*handle){
 if(!handle||surface<=0||((uint64_t)(unsigned)surface)>UINT32_MAX){errno=EINVAL;return -1;}
 struct device*d=get(fd);if(!d)return -1;
 struct lmal_surface_import q={.surface_id=(uint32_t)surface,.flags=LMAL_BO_READ|LMAL_BO_WRITE|LMAL_BO_FIXED_BACKING};
 int r=call(d,LMAL_SELECTOR_SURFACE_IMPORT,&q,sizeof(q));
 if(!r){if(!q.handle||!q.gpu_va||!q.size||q.map_type!=q.handle){errno=EPROTO;r=-1;}else *handle=q.handle;}
 put(d);return r;
}
int drmSyncobjFDToHandle(int a,int b,uint32_t*c){(void)a;(void)b;(void)c;UNSUPPORTED();}
int drmSyncobjHandleToFD(int a,uint32_t b,int*c){(void)a;(void)b;(void)c;UNSUPPORTED();}
int drmSyncobjImportSyncFile(int a,uint32_t b,int c){(void)a;(void)b;(void)c;UNSUPPORTED();}
int drmSyncobjExportSyncFile(int a,uint32_t b,int*c){(void)a;(void)b;(void)c;UNSUPPORTED();}
int drmSyncobjTimelineWait(int a,uint32_t*b,uint64_t*c,unsigned d,int64_t e,unsigned f,uint32_t*g){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;UNSUPPORTED();}
int drmSyncobjTimelineSignal(int a,const uint32_t*b,uint64_t*c,uint32_t d){(void)a;(void)b;(void)c;(void)d;UNSUPPORTED();}
int drmSyncobjTransfer(int a,uint32_t b,uint64_t c,uint32_t d,uint64_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;UNSUPPORTED();}
int drmSyncobjQuery(int a,uint32_t*b,uint64_t*c,uint32_t d){(void)a;(void)b;(void)c;(void)d;UNSUPPORTED();}
int drmGetDevice2(int a,uint32_t b,drmDevicePtr*c){(void)a;(void)b;(void)c;UNSUPPORTED();}
void drmFreeDevice(drmDevicePtr*p){if(p)*p=NULL;}
int drmGetDevices2(uint32_t a,drmDevicePtr b[],int c){(void)a;(void)b;(void)c;return 0;}
void drmFreeDevices(drmDevicePtr a[],int b){(void)a;(void)b;}
char*drmGetDeviceNameFromFd2(int a){(void)a;return strdup(LMAL_SERVICE_NAME);}
char*drmGetRenderDeviceNameFromFd(int a){return drmGetDeviceNameFromFd2(a);}
int drmGetNodeTypeFromFd(int a){(void)a;return DRM_NODE_RENDER;}
void*drmGetHashTable(void){return NULL;}
