/* SPDX-License-Identifier: MIT
 * Negotiated Panfrost FIFO transport for the existing LancelotMali service.
 * CPU syncobjs remember real kernel queue tokens; enqueue never signals one.
 */
#include "lancelot_mali_queue_uapi.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
struct lmq_bridge_bo { uint64_t size,va; uint32_t flags; bool used; };
struct lmq_bridge_fence { uint64_t token; uint32_t references; int error; bool done; };
struct lmq_bridge_sync { uint64_t token; int error; bool used,signaled; uint64_t generation; struct lmq_bridge_fence *fence; };
struct lmq_bridge {
 uintptr_t connection; bool queued, access, pair, asynchronous;
 uint64_t max_bo_bytes;
 pthread_mutex_t lock;
 uint64_t last_token,submitted,completed,faults,generation;
 struct lmq_bridge_fence *fences[LMQ_FENCE_SLOTS]; uint32_t orphan_count;
 struct lmq_bridge_bo bos[LMQ_MAX_BOS];
 struct lmq_bridge_sync syncs[LMAL_MAX_SYNCOBJS];
};
static uint64_t lmq_mono(void) {
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
 return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static int lmq_error(void) {
 switch(errno){case EINVAL:return LMAL_EINVAL;case ENOMEM:return LMAL_ENOMEM;
 case EBUSY:return LMAL_EBUSY;case ENODEV:return LMAL_ENODEV;case ENOTSUP:return LMAL_ENOTSUP;
 case ETIMEDOUT:return LMAL_ETIMEDOUT;case EPERM:return LMAL_EPERM;default:return LMAL_EIO;}
}
static int lmq_raw(struct lmq_bridge *c,uint32_t selector,void *q,size_t size) {
 return legacy_transport_call(c->connection,selector,q,size)?lmq_error():LMAL_OK;
}
static int lmq_fence_refresh(struct lmq_bridge *c,struct lmq_bridge_fence *f,uint64_t ns) {
 if(f->done)return f->error;
 struct lmq_wait wait={f->token,(int64_t)ns};
 int error=lmq_raw(c,LMQ_WAIT,&wait,sizeof(wait));
 if(error==LMAL_ETIMEDOUT)return error;
 f->done=true;f->error=error;
 (void)lmq_raw(c,LMQ_ACK,&wait,sizeof(wait));
 return error;
}
static int lmq_refresh(struct lmq_bridge *c,struct lmq_bridge_sync *s,uint64_t ns) {
 if(!s->used)return LMAL_EINVAL;
 if(s->fence){int error=lmq_fence_refresh(c,s->fence,ns);if(error!=LMAL_ETIMEDOUT){s->signaled=true;s->error=error;}return error;}
 return s->signaled?s->error:LMAL_ETIMEDOUT;
}
static void lmq_sync_drop(struct lmq_bridge_sync *s) {
 if(s->fence){if(s->fence->references)s->fence->references--;s->fence=NULL;}
}
int lm_transport_open(uintptr_t *out) {
 if(!out){errno=EINVAL;return -1;}
 struct lmq_bridge *c=calloc(1,sizeof(*c));if(!c){errno=ENOMEM;return -1;}
 if(legacy_transport_open(&c->connection)){free(c);return -1;}
 struct lmal_caps caps={0};caps.h.magic=LMAL_UAPI_MAGIC;caps.h.version=LMAL_UAPI_VERSION;caps.h.size=sizeof(caps);
 if(legacy_transport_call(c->connection,LMAL_SELECTOR_GET_CAPS,&caps,sizeof(caps)) || caps.h.result){
  legacy_transport_close(c->connection);free(c);errno=ENODEV;return -1;
 }
 c->queued=(caps.flags&LMAL_CAP_QUEUED_DRM)!=0;
 c->asynchronous=(caps.flags&LMAL_CAP_ASYNC_SUBMIT)!=0;
 c->max_bo_bytes=caps.max_bo_bytes?caps.max_bo_bytes:LMAL_MAX_BO_BYTES;
 if(c->max_bo_bytes>LMQ_MAX_BO_BYTES){legacy_transport_close(c->connection);free(c);errno=EPROTO;return -1;}
 c->access=(caps.flags&LMAL_CAP_SUBMIT_ACCESS)!=0;
 c->pair=(caps.flags&(LMAL_CAP_SUBMIT_PAIR_SYNC|LMAL_CAP_CHECK_PAIR_SYNC))==(LMAL_CAP_SUBMIT_PAIR_SYNC|LMAL_CAP_CHECK_PAIR_SYNC);
 if(pthread_mutex_init(&c->lock,NULL)){legacy_transport_close(c->connection);free(c);errno=ENOMEM;return -1;}
 fprintf(stderr,"lancelot-transport: queued=%u async_submit=%u access=%u pair=%u max_bo_bytes=%llu\n",
  c->queued,c->asynchronous,c->access,c->pair,(unsigned long long)c->max_bo_bytes);
 *out=(uintptr_t)c;return 0;
}
void lm_transport_close(uintptr_t connection) {
 struct lmq_bridge *c=(struct lmq_bridge *)connection;if(!c)return;
 legacy_transport_close(c->connection);
 for(uint32_t i=0;i<LMQ_FENCE_SLOTS;i++)free(c->fences[i]);
 pthread_mutex_destroy(&c->lock);free(c);
}
static int lmq_reap(struct lmq_bridge *c) {
 c->orphan_count=0;
 for(uint32_t i=0;i<LMQ_FENCE_SLOTS;i++)if(c->fences[i]) {
  struct lmq_bridge_fence *f=c->fences[i];
  if(!f->references) {
   int e=lmq_fence_refresh(c,f,0);
   if(e!=LMAL_ETIMEDOUT){free(f);c->fences[i]=NULL;}
   else c->orphan_count++;
  }
 }
 return 0;
}
static int lmq_call_locked(struct lmq_bridge *c,uint32_t selector,void *data,size_t n) {
 switch(selector) {
 case LMAL_SELECTOR_CHECK_PAIR_SYNC:case LMAL_SELECTOR_SUBMIT_PAIR_SYNC:{
  bool check=selector==LMAL_SELECTOR_CHECK_PAIR_SYNC;
  if(!c->pair)return LMAL_ENOTSUP;
  if(n!=(check?sizeof(struct lmal_check_pair_sync):sizeof(struct lmal_submit_pair_sync)))return LMAL_EINVAL;
  struct lmal_submit_pair_sync *q=data;
  uint32_t out=q->out_sync,in_count=q->in_sync_count;uint64_t in=q->in_syncs;
  if(in_count>LMAL_MAX_SUBMIT_SYNCS || (in_count&&!in) || (out && (out>LMAL_MAX_SYNCOBJS || !c->syncs[out-1].used)))return LMAL_EINVAL;
  const uint32_t *syncs=(const uint32_t *)(uintptr_t)in;
  for(uint32_t i=0;i<in_count;i++) {
   if(!syncs[i] || syncs[i]>LMAL_MAX_SYNCOBJS || !c->syncs[syncs[i]-1].used || syncs[i]==out)return LMAL_EINVAL;
   struct lmq_bridge_sync *s=&c->syncs[syncs[i]-1];
   if(!check){int e=lmq_refresh(c,s,LMAL_MAX_WAIT_NS);if(e)return e;}
  }
  q->in_sync_count=0;q->in_syncs=0;q->out_sync=0;
  int e=legacy_transport_call(c->connection,selector,data,n)?lmq_error():q->h.result;
  q->in_sync_count=in_count;q->in_syncs=in;q->out_sync=out;
  if(!check && !e && (q->attempted_mask!=3 || q->completed_mask!=3 || !q->sequence[0] || !q->sequence[1] || q->stage_error[0] || q->stage_error[1]))e=LMAL_EIO;
  if(!check && out) {
   struct lmq_bridge_sync *s=&c->syncs[out-1];lmq_sync_drop(s);
   s->token=0;s->error=e;s->signaled=true;
  }
  return e;}
 case LMAL_SELECTOR_GET_CAPS:{
  if(n!=sizeof(struct lmal_caps))return LMAL_EINVAL;
  (void)lmq_reap(c);
  return legacy_transport_call(c->connection,selector,data,n)?lmq_error():0;}
 case LMAL_SELECTOR_JOB_PROFILE:{
  if(n!=sizeof(struct lmal_job_profile))return LMAL_EINVAL;
  int e=lmq_raw(c,selector,data,n);return e?e:((struct lmal_job_profile*)data)->h.result;}
 case LMAL_SELECTOR_GET_PARAM:{
  if(n!=sizeof(struct lmal_get_param))return LMAL_EINVAL;
  struct lmal_get_param *q=data;struct lmq_param p={q->param,0,0};
  int e=lmq_raw(c,LMQ_PARAM,&p,sizeof(p));if(!e)q->value=p.value;return e;}
 case LMAL_SELECTOR_BO_CREATE:{
  if(n!=sizeof(struct lmal_bo_create))return LMAL_EINVAL;
  struct lmal_bo_create *q=data;if(!q->size || q->size>c->max_bo_bytes || q->size>UINT32_MAX)return LMAL_EINVAL;
  struct lmq_create p={(uint32_t)q->size,((q->flags&LMAL_BO_NOEXEC)?LMQ_BO_NOEXEC:0)|((q->flags&LMAL_BO_HEAP)?LMQ_BO_HEAP:0),0,0,0};
  int e=lmq_raw(c,LMQ_CREATE,&p,sizeof(p));if(e)return e;
  if(!p.handle || p.handle>LMQ_MAX_BOS || !p.offset || p.size<q->size)return LMAL_EIO;
  c->bos[p.handle-1]=(struct lmq_bridge_bo){p.size,p.offset,q->flags,true};
  q->handle=q->map_type=p.handle;q->gpu_va=p.offset;q->actual_size=p.size;return 0;}
 case LMAL_SELECTOR_BO_QUERY:{
  if(n!=sizeof(struct lmal_bo_query))return LMAL_EINVAL;
  struct lmal_bo_query *q=data;if(!q->handle || q->handle>LMQ_MAX_BOS || !c->bos[q->handle-1].used)return LMAL_EINVAL;
  struct lmq_bridge_bo *b=&c->bos[q->handle-1];q->size=b->size;q->gpu_va=b->va;q->flags=b->flags;q->map_type=q->handle;return 0;}
 case LMAL_SELECTOR_BO_CLOSE:{
  if(n!=sizeof(struct lmal_bo_close))return LMAL_EINVAL;
  struct lmal_bo_close *q=data;struct lmq_close p={q->handle,0};int e=lmq_raw(c,LMQ_CLOSE,&p,sizeof(p));
  if(!e && q->handle && q->handle<=LMQ_MAX_BOS)memset(&c->bos[q->handle-1],0,sizeof(c->bos[0]));
  return e;}
 case LMAL_SELECTOR_BO_UNMAP:return n==sizeof(struct lmal_bo_close)?0:LMAL_EINVAL;
 case LMAL_SELECTOR_BO_WAIT:{
  if(n!=sizeof(struct lmal_bo_wait))return LMAL_EINVAL;
  struct lmal_bo_wait *q=data;uint64_t now=lmq_mono(),left=q->absolute_monotonic_ns>now?q->absolute_monotonic_ns-now:0;
  if(left>LMAL_MAX_WAIT_NS)left=LMAL_MAX_WAIT_NS;
  struct lmq_wait_bo p={q->handle,0,(int64_t)left};return lmq_raw(c,LMQ_WAIT_BO,&p,sizeof(p));}
 case LMAL_SELECTOR_SURFACE_QUERY:case LMAL_SELECTOR_SURFACE_IMPORT:{
  bool import=selector==LMAL_SELECTOR_SURFACE_IMPORT;
  if(n!=(import?sizeof(struct lmal_surface_import):sizeof(struct lmal_surface_query)))return LMAL_EINVAL;
  uint32_t sid=import?((struct lmal_surface_import *)data)->surface_id:((struct lmal_surface_query *)data)->surface_id;
  struct lmq_surface p={0};p.surface=sid;p.handle=import?0:LMQ_DESCRIBE;
  int e=lmq_raw(c,LMQ_SURFACE,&p,sizeof(p));if(e)return e;
  if(import){
   struct lmal_surface_import *q=data;if(!p.handle || p.handle>LMQ_MAX_BOS)return LMAL_EIO;
   q->handle=q->map_type=p.handle;q->gpu_va=0;
   struct lmq_offset offset={p.handle,0,0};e=lmq_raw(c,LMQ_OFFSET,&offset,sizeof(offset));if(e)return e;
   q->gpu_va=offset.offset;q->size=p.size;q->offset=p.offset;q->row_bytes=p.rowBytes;q->width=p.width;q->height=p.height;q->pixel_format=p.pixelFormat;q->planes=p.planes;
   c->bos[p.handle-1]=(struct lmq_bridge_bo){p.size,offset.offset,q->flags,true};
  }else{struct lmal_surface_query *q=data;q->handle=0;q->size=p.size;q->offset=p.offset;q->row_bytes=p.rowBytes;q->width=p.width;q->height=p.height;q->pixel_format=p.pixelFormat;q->planes=p.planes;}return 0;}
 case LMAL_SELECTOR_SYNCOBJ_CREATE:{
  if(n!=sizeof(struct lmal_syncobj_create))return LMAL_EINVAL;
  struct lmal_syncobj_create *q=data;if(q->flags&~LMAL_SYNCOBJ_CREATE_SIGNALED)return LMAL_EINVAL;
  for(uint32_t i=0;i<LMAL_MAX_SYNCOBJS;i++)if(!c->syncs[i].used){c->syncs[i]=(struct lmq_bridge_sync){0,0,true,(q->flags&LMAL_SYNCOBJ_CREATE_SIGNALED)!=0,++c->generation,NULL};q->handle=i+1;return 0;}
  return LMAL_ENOMEM;}
 case LMAL_SELECTOR_SYNCOBJ_SNAPSHOT:{
  if(n!=sizeof(struct lmal_syncobj_snapshot))return LMAL_EINVAL;
  struct lmal_syncobj_snapshot *q=data;
  if(!q->source || q->source>LMAL_MAX_SYNCOBJS || !c->syncs[q->source-1].used)return LMAL_EINVAL;
  struct lmq_bridge_sync *source=&c->syncs[q->source-1];
  /* A never-submitted syncobj has no fence to snapshot. */
  if(!source->fence && !source->signaled)return LMAL_EBUSY;
  if(c->generation==UINT64_MAX)return LMAL_EBUSY;
  for(uint32_t i=0;i<LMAL_MAX_SYNCOBJS;i++)if(!c->syncs[i].used){
   c->syncs[i]=*source;c->syncs[i].generation=++c->generation;
   if(c->syncs[i].fence)c->syncs[i].fence->references++;
   q->handle=i+1;return LMAL_OK;
  }
  return LMAL_ENOMEM;}
 case LMAL_SELECTOR_SYNCOBJ_DESTROY:{
  if(n!=sizeof(struct lmal_syncobj_handle))return LMAL_EINVAL;
  uint32_t h=((struct lmal_syncobj_handle *)data)->handle;if(!h || h>LMAL_MAX_SYNCOBJS || !c->syncs[h-1].used)return LMAL_EINVAL;
  struct lmq_bridge_sync *s=&c->syncs[h-1];lmq_sync_drop(s);
  memset(s,0,sizeof(*s));(void)lmq_reap(c);return 0;}
 case LMAL_SELECTOR_SYNCOBJ_RESET:case LMAL_SELECTOR_SYNCOBJ_SIGNAL:{
  if(n!=sizeof(struct lmal_syncobj_array))return LMAL_EINVAL;
  struct lmal_syncobj_array *q=data;if(!q->handles || !q->count || q->count>LMAL_MAX_SUBMIT_SYNCS)return LMAL_EINVAL;
  const uint32_t *h=(const uint32_t *)(uintptr_t)q->handles;
  for(uint32_t i=0;i<q->count;i++)if(!h[i] || h[i]>LMAL_MAX_SYNCOBJS || !c->syncs[h[i]-1].used)return LMAL_EINVAL;
  if(q->count>UINT64_MAX-c->generation)return LMAL_EBUSY;
  for(uint32_t i=0;i<q->count;i++){
   lmq_sync_drop(&c->syncs[h[i]-1]);
   c->syncs[h[i]-1]=(struct lmq_bridge_sync){0,0,true,selector==LMAL_SELECTOR_SYNCOBJ_SIGNAL,++c->generation,NULL};
  }
  (void)lmq_reap(c);return 0;}
 case LMAL_SELECTOR_SYNCOBJ_WAIT:{
  if(n!=sizeof(struct lmal_syncobj_wait))return LMAL_EINVAL;
  struct lmal_syncobj_wait *q=data;if(!q->handles || !q->count || q->count>LMAL_MAX_SUBMIT_SYNCS)return LMAL_EINVAL;
  const uint32_t *h=(const uint32_t *)(uintptr_t)q->handles;
  for(uint32_t i=0;i<q->count;i++)if(!h[i] || h[i]>LMAL_MAX_SYNCOBJS || !c->syncs[h[i]-1].used)return LMAL_EINVAL;
  struct lmq_bridge_sync snapshots[LMAL_MAX_SUBMIT_SYNCS];
  for(uint32_t i=0;i<q->count;i++){
   snapshots[i]=c->syncs[h[i]-1];
   if(snapshots[i].fence)snapshots[i].fence->references++;
  }
  uint64_t began=lmq_mono(),end=q->absolute_monotonic_ns;
  if(end<began)end=began;
  if(end-began>LMAL_MAX_WAIT_NS)end=began+LMAL_MAX_WAIT_NS;
  int result=LMAL_ETIMEDOUT;
  for(;;){
   uint32_t ready=0;
   for(uint32_t i=0;i<q->count;i++){
    struct lmq_bridge_sync *s=&snapshots[i];
    if(!s->fence && !s->signaled && (q->flags&LMAL_SYNC_WAIT_FOR_SUBMIT)) {
     if(!c->syncs[h[i]-1].used || c->syncs[h[i]-1].generation!=s->generation){result=LMAL_EINVAL;goto wait_done;}
     if(c->syncs[h[i]-1].fence || c->syncs[h[i]-1].signaled){
      *s=c->syncs[h[i]-1];if(s->fence)s->fence->references++;
     }
    }
    int e=lmq_refresh(c,s,0);
    if(e!=LMAL_ETIMEDOUT && e){result=e;goto wait_done;}
    if(!e){if(!ready)q->first_signaled=i;ready++;}
   }
   if((q->flags&LMAL_SYNC_WAIT_ALL)?ready==q->count:ready!=0){result=0;break;}
   if(lmq_mono()>=end)break;
   pthread_mutex_unlock(&c->lock);usleep(50);pthread_mutex_lock(&c->lock);
  }
wait_done:
  for(uint32_t i=0;i<q->count;i++)lmq_sync_drop(&snapshots[i]);
  (void)lmq_reap(c);return result;}
 case LMAL_SELECTOR_SUBMIT:case LMAL_SELECTOR_SUBMIT_ACCESS:{
  if(n!=(selector==LMAL_SELECTOR_SUBMIT?sizeof(struct lmal_submit):sizeof(struct lmal_submit_access)))return LMAL_EINVAL;
  int reaped=lmq_reap(c);if(reaped)return reaped;
  struct lmal_submit *q=data;if(q->reserved || !q->bo_count || q->bo_count>LMQ_MAX_BOS || !q->bo_handles || q->in_sync_count>LMAL_MAX_SUBMIT_SYNCS || (q->in_sync_count && !q->in_syncs))return LMAL_EINVAL;
  const uint32_t *h=(const uint32_t *)(uintptr_t)q->in_syncs;
  for(uint32_t i=0;i<q->in_sync_count;i++){
   if(!h[i] || h[i]>LMAL_MAX_SYNCOBJS || !c->syncs[h[i]-1].used)return LMAL_EINVAL;
   struct lmq_bridge_sync *s=&c->syncs[h[i]-1];
   if(s->fence){int ready=lmq_refresh(c,s,0);if(ready!=LMAL_ETIMEDOUT && ready)return ready;}
   if(s->signaled && s->error)return s->error;
   if(!s->signaled && (!s->token || s->token>c->last_token))return LMAL_EBUSY;
  }
  if(q->out_sync && (q->out_sync>LMAL_MAX_SYNCOBJS || !c->syncs[q->out_sync-1].used))return LMAL_EINVAL;
  for(uint32_t i=0;i<q->in_sync_count;i++)if(h[i]==q->out_sync)return LMAL_EINVAL;
  uint32_t slot;for(slot=0;slot<LMQ_FENCE_SLOTS && c->fences[slot];slot++){}
  if(slot==LMQ_FENCE_SLOTS)return LMAL_EBUSY;
  struct lmq_bridge_fence *f=calloc(1,sizeof(*f));if(!f)return LMAL_ENOMEM;
  struct lmq_submit p={q->jc,0,0,0,q->bo_handles,q->bo_count,q->requirements};
  int e;
  if(selector==LMAL_SELECTOR_SUBMIT_ACCESS){
   if(!c->access){free(f);return LMAL_ENOTSUP;}
   struct lmal_submit_access *a=data;
   if(!a->bo_readonly){free(f);return LMAL_EINVAL;}
   struct lmq_submit_access raw={p,a->bo_readonly};
   e=lmq_raw(c,LMQ_SUBMIT_ACCESS,&raw,sizeof(raw));p=raw.submit;
  }else e=lmq_raw(c,LMQ_SUBMIT,&p,sizeof(p));
  if(e){free(f);return e;}
  if(!p.jc || p.jc<=c->last_token){free(f);return LMAL_EIO;}
  c->last_token=p.jc;c->submitted++;q->sequence=p.jc;q->job_status=0;
  f->token=p.jc;c->fences[slot]=f;
  if(q->out_sync){
   struct lmq_bridge_sync *out=&c->syncs[q->out_sync-1];lmq_sync_drop(out);
   out->token=p.jc;out->error=0;out->signaled=false;out->fence=f;f->references++;
  }
  /* Panfrost's normal SUBMIT returns after enqueue, as in the donor. The
   * syncobj remains pending and BO_WAIT/CLOSE protect descriptors and scratch.
   * Older kernels keep their synchronous contract. The unchanged submit ABI
   * has no completion flag; explicit completion uses syncobj or BO waits. */
  if(!c->asynchronous) {
   e=lmq_fence_refresh(c,f,q->timeout_ns);
  }
  return e;}
 default:return LMAL_ENOTSUP;
 }
}
int lm_transport_call(uintptr_t connection,uint32_t selector,void *data,size_t n) {
 struct lmq_bridge *c=(struct lmq_bridge *)connection;if(!c || !data){errno=EINVAL;return -1;}
 /* The public AIR/Metal extensions use the already-supported native layouts,
  * not an LMAL header. They touch no mutable bridge syncobj state. In
  * particular, a native wait must let another thread submit on this client. */
 if(selector==LMQ_WAIT || selector==LMQ_SURFACE_SEQ){
  if(!c->queued){errno=ENOTSUP;return -1;}
  if(selector==LMQ_WAIT){
   if(n!=sizeof(struct lmq_wait)){errno=EINVAL;return -1;}
   struct lmq_wait*q=data;
   if(q->timeout_ns<0)q->timeout_ns=0;
   if((uint64_t)q->timeout_ns>LMAL_MAX_WAIT_NS)q->timeout_ns=LMAL_MAX_WAIT_NS;
  }else{
   if(n!=sizeof(struct lmq_surface_seq)){errno=EINVAL;return -1;}
   struct lmq_surface_seq*q=data;
   if(!q->surface || q->pad){errno=EINVAL;return -1;}
  }
  return legacy_transport_call(c->connection,selector,data,n);
 }
 if(!c->queued)return legacy_transport_call(c->connection,selector,data,n);
 if(n<sizeof(struct lmal_header)){errno=EINVAL;return -1;}
 struct lmal_header *h=data;if(h->magic!=LMAL_UAPI_MAGIC || h->version!=LMAL_UAPI_VERSION || h->size!=n){errno=EPROTO;return -1;}
 pthread_mutex_lock(&c->lock);h->result=lmq_call_locked(c,selector,data,n);pthread_mutex_unlock(&c->lock);return 0;
}
int lm_transport_map(uintptr_t connection,uint32_t handle,uintptr_t *address,uint64_t *size) {
 struct lmq_bridge *c=(struct lmq_bridge *)connection;
 return !c?(errno=EINVAL,-1):legacy_transport_map(c->connection,handle,address,size);
}
int lm_transport_unmap(uintptr_t connection,uint32_t handle,uintptr_t address) {
 struct lmq_bridge *c=(struct lmq_bridge *)connection;
 return !c?(errno=EINVAL,-1):legacy_transport_unmap(c->connection,handle,address);
}
