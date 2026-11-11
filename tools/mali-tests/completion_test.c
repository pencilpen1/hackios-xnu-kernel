#include <assert.h>
#include "lancelot_drm.c"
static uint64_t issued,finished,now_ns=UINT64_C(5000000000);
static unsigned queries,transport_waits;
static uint32_t advertised_roles,last_submit_selector;
static _Thread_local bool reject_submit;
static uint64_t missing_token,error_token,protocol_token,slow_token,failed_token;
uint64_t lancelot_diag_ns(void){return now_ns;}
void lancelot_diag_anchor(void){}
int lm_transport_open(uintptr_t *c){*c=123;return 0;}
void lm_transport_close(uintptr_t c){assert(c==123);}
int lm_transport_map(uintptr_t c,uint32_t h,uintptr_t *a,uint64_t *n){(void)c;(void)h;(void)a;(void)n;errno=ENOTSUP;return -1;}
int lm_transport_unmap(uintptr_t c,uint32_t h,uintptr_t a){(void)c;(void)h;(void)a;return 0;}
int lm_transport_call(uintptr_t c,uint32_t selector,void *data,size_t size){
 assert(c==123);struct lmal_header*h=data;assert(h->size==size);
 if(selector==LMAL_SELECTOR_GET_CAPS){((struct lmal_caps*)data)->flags=LMAL_CAP_JOB_PROFILE|LMAL_CAP_ASYNC_SUBMIT|advertised_roles;return 0;}
 if(selector==LMAL_SELECTOR_SYNCOBJ_WAIT||selector==LMAL_SELECTOR_BO_WAIT){transport_waits++;return 0;}
 if(selector==LMAL_SELECTOR_SUBMIT||selector==LMAL_SELECTOR_SUBMIT_ACCESS){
  last_submit_selector=selector;
  if(selector==LMAL_SELECTOR_SUBMIT_ACCESS)assert(((struct lmal_submit_access*)data)->bo_readonly);
  struct lmal_submit *q=data;if(reject_submit)h->result=LMAL_ETIMEDOUT;else q->sequence=42;return 0;
 }
 assert(selector==LMAL_SELECTOR_JOB_PROFILE);queries++;
 struct lmal_job_profile*q=data;assert(size==sizeof(*q));uint64_t token=q->sequence;
 if(token==missing_token||token>issued){h->result=LMAL_EINVAL;return 0;}
 if(token==error_token){h->result=LMAL_EIO;return 0;}
 if(token>finished){h->result=LMAL_EBUSY;return 0;}
 q->start_ns=1000000;q->jm_ns=token==slow_token?20000000:1000000;
 q->end_ns=q->start_ns+q->jm_ns+5000;q->worker_ns=q->jm_ns+10000;q->hz=850000000;
 q->error=token==failed_token?LMAL_EIO:0;
 if(token==protocol_token)q->sequence++;
 return 0;
}
static struct device*peek(int fd){struct device*d=get(fd);assert(d);return d;}
static void *other_thread(void *unused){
 (void)unused;uint64_t epoch=99,seq=99;assert(!lancelot_drm_submit_status(&epoch,&seq)&&!epoch&&!seq);
 assert(drmIoctl(-1,DRM_IOCTL_PANFROST_SUBMIT,NULL)==-1);
 assert(lancelot_drm_submit_status(&epoch,&seq)==EINVAL&&epoch==1&&!seq);return NULL;
}
static void test_submit_observation(int fd){
 uint32_t handle=1,ro=0;struct drm_panfrost_submit p={.jc=0x1000,.bo_handle_count=1,.bo_handles=(uintptr_t)&handle};
 uint64_t epoch=0,seq=0;assert(!lancelot_drm_submit_status(&epoch,&seq));
 assert(!lancelot_drm_submit_access(fd,&p,&ro));assert(!lancelot_drm_submit_status(&epoch,&seq)&&seq==42&&!epoch);
 reject_submit=true;assert(lancelot_drm_submit_access(fd,&p,&ro)==-1&&errno==ETIMEDOUT);
 assert(lancelot_drm_submit_status(&epoch,&seq)==ETIMEDOUT&&epoch==1&&seq==42&&!lancelot_drm_last_seq());
 reject_submit=false;assert(!drmIoctl(fd,DRM_IOCTL_PANFROST_SUBMIT,&p));
 assert(lancelot_drm_submit_status(&epoch,&seq)==ETIMEDOUT&&epoch==1&&seq==42);
 pthread_t thread;assert(!pthread_create(&thread,NULL,other_thread,NULL));assert(!pthread_join(thread,NULL));
 assert(lancelot_drm_submit_status(&epoch,&seq)==ETIMEDOUT&&epoch==1);
 reject_submit=true;assert(drmIoctl(fd,DRM_IOCTL_PANFROST_SUBMIT,&p)==-1);
 assert(lancelot_drm_submit_status(&epoch,&seq)==ETIMEDOUT&&epoch==2);
 reject_submit=false;assert(lancelot_drm_submit_access(-1,&p,&ro)==-1&&errno==EBADF);
 assert(lancelot_drm_submit_status(&epoch,&seq)==EBADF&&epoch==3);
 puts("PASS actual DRM submit observation: access/ioctl failures remain visible across later success; accepted sequence retained; other threads isolated");
}
static void test_role_negotiation(void){
 uint32_t handle=1,ro=1;struct drm_panfrost_submit p={.jc=0x1000,.bo_handle_count=1,.bo_handles=(uintptr_t)&handle};
 const uint32_t capabilities[]={0,LMAL_CAP_SUBMIT_ACCESS,LMAL_CAP_SURFACE_READ_HINT};
 for(unsigned i=0;i<3;i++){
  advertised_roles=capabilities[i];int fd=drmOpenWithType("panfrost",NULL,DRM_NODE_RENDER);assert(fd>=0);
  assert(!lancelot_drm_submit_access(fd,&p,&ro));
  assert(last_submit_selector==(i?LMAL_SELECTOR_SUBMIT_ACCESS:LMAL_SELECTOR_SUBMIT));
  assert(!drmClose(fd));
 }
 advertised_roles=0;puts("PASS roles negotiated independently from RO PTE capability; legacy submits unchanged");
}
int main(void){
 test_role_negotiation();
 int fd=drmOpenWithType("panfrost",NULL,DRM_NODE_RENDER);assert(fd>=0);issued=2;
 test_submit_observation(fd);
 struct lm_completed_batch b={.ctx=0xabc,.seq=7,.vtq=1,.fsq=2,.at_ns=100000,.submit_end_ns=200000,
  .draws=2,.w=1080,.h=2400,.x1=1079,.y1=2399,.rts=1,.format={42},.modifier={0x123}};
 b.mix.pair[0]=(struct lm_shader_pair){21,22,2};b.mix.vs[0].id=21;b.mix.fs[0].id=22;
 strcpy(b.mix.vs[0].hash,"copied-vs");strcpy(b.mix.fs[0].hash,"copied-fs");
 errno=E2BIG;lancelot_drm_profile_batch(fd,&b);assert(errno==E2BIG&&!queries&&!transport_waits);
 b.mix.vs[0].id=999;b.format[0]=999;
 struct device*d=peek(fd);assert(d->profile_count==1&&d->profiles[0].batch.mix.vs[0].id==21&&d->profiles[0].batch.format[0]==42);put(d);
 lancelot_drm_profile_poll(fd);assert(errno==E2BIG&&queries==2&&!transport_waits);
 finished=1;lancelot_drm_profile_poll(fd);d=peek(fd);assert(d->profiles[0].read_vt&&!d->profiles[0].read_fs);put(d);
 finished=2;slow_token=2;uint32_t sync=1;
 assert(!drmSyncobjWait(fd,&sync,1,INT64_MAX,DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,NULL));
 d=peek(fd);assert(!d->profile_count&&d->profile_completed==1&&d->profile_emitted==1);put(d);
 assert(transport_waits==1&&errno==E2BIG);
 puts("PASS copied shader/resource snapshots and idle tail flushed by real syncobj wait; polling is nonblocking");
 b=(struct lm_completed_batch){.seq=8,.fsq=3,.at_ns=100,.submit_end_ns=200};issued=finished=3;
 lancelot_drm_profile_batch(fd,&b);lancelot_drm_profile_poll(fd);d=peek(fd);assert(d->profile_completed==2&&d->profile_emitted==1);put(d);
 failed_token=4;b.seq=9;b.fsq=4;issued=finished=4;lancelot_drm_profile_batch(fd,&b);lancelot_drm_profile_poll(fd);
 d=peek(fd);assert(d->profile_completed==3&&d->profile_emitted==2);put(d);
 missing_token=5;b.fsq=5;issued=finished=5;lancelot_drm_profile_batch(fd,&b);lancelot_drm_profile_poll(fd);
 error_token=6;b.fsq=6;issued=finished=6;lancelot_drm_profile_batch(fd,&b);lancelot_drm_profile_poll(fd);
 protocol_token=7;b.fsq=7;issued=finished=7;lancelot_drm_profile_batch(fd,&b);lancelot_drm_profile_poll(fd);
 d=peek(fd);assert(!d->profile_count&&d->profile_missing==3&&d->profile_query_errors==2);put(d);
 puts("PASS completed fast tasks counted, native errors emitted, overwritten/transport/malformed observations counted");
 issued=1000;finished=7;
 for(unsigned i=0;i<65;i++){b.seq=100+i;b.vtq=100+i;b.fsq=0;lancelot_drm_profile_batch(fd,&b);}
 d=peek(fd);assert(d->profile_count==64&&d->profile_dropped==1);put(d);
 finished=1000;lancelot_drm_profile_poll(fd);d=peek(fd);assert(!d->profile_count&&d->profile_completed==67&&d->profile_emitted==2);put(d);
 puts("PASS 64-entry FIFO bound, loss count and complete drain after publication");
 struct lancelot_job_profile result={.token=0xfeed};errno=0;
 assert(lancelot_drm_job_profile(fd,7,&result)<0&&errno==EPROTO&&result.token==0xfeed);
 b.seq=200;b.fsq=900;b.vtq=0;finished=899;lancelot_drm_profile_batch(fd,&b);
 assert(!drmClose(fd));puts("PASS close reports pending tail without claiming completion");return 0;
}
