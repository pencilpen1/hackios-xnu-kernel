#include <assert.h>
#include "lancelot_drm.c"
static uint64_t issued,finished,now_ns=UINT64_C(5000000000);
static unsigned queries,transport_waits;
static uint64_t missing_token,error_token,protocol_token,slow_token,failed_token;
uint64_t lancelot_diag_ns(void){return now_ns;}
void lancelot_diag_anchor(void){}
int lm_transport_open(uintptr_t *c){*c=123;return 0;}
void lm_transport_close(uintptr_t c){assert(c==123);}
int lm_transport_map(uintptr_t c,uint32_t h,uintptr_t *a,uint64_t *n){(void)c;(void)h;(void)a;(void)n;errno=ENOTSUP;return -1;}
int lm_transport_unmap(uintptr_t c,uint32_t h,uintptr_t a){(void)c;(void)h;(void)a;return 0;}
int lm_transport_call(uintptr_t c,uint32_t selector,void *data,size_t size){
 assert(c==123);struct lmal_header*h=data;assert(h->size==size);
 if(selector==LMAL_SELECTOR_GET_CAPS){((struct lmal_caps*)data)->flags=LMAL_CAP_JOB_PROFILE|LMAL_CAP_ASYNC_SUBMIT;return 0;}
 if(selector==LMAL_SELECTOR_SYNCOBJ_WAIT||selector==LMAL_SELECTOR_BO_WAIT){transport_waits++;return 0;}
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
int main(void){
 int fd=drmOpenWithType("panfrost",NULL,DRM_NODE_RENDER);assert(fd>=0);issued=2;
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
