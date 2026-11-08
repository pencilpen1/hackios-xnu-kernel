/* SPDX-License-Identifier: MIT */
static int
lm_profile_query(struct device *d,uint64_t token,struct lancelot_job_profile *out)
{
 struct lmal_job_profile q={.sequence=token};int e=call(d,LMAL_SELECTOR_JOB_PROFILE,&q,sizeof(q));
 if(!e){
  if(q.sequence!=token||q.end_ns<q.start_ns||q.jm_ns>q.end_ns-q.start_ns){errno=EPROTO;return -1;}
  *out=(struct lancelot_job_profile){q.sequence,q.start_ns,q.end_ns,q.jm_ns,q.worker_ns,q.hz,q.error};
 }
 return e;
}
static void
lm_profile_report(const struct lm_profile_pending *p)
{
 const struct lm_completed_batch *b=&p->batch;
 uint64_t now=lancelot_diag_ns(),done=p->vt.end_ns>p->fs.end_ns?p->vt.end_ns:p->fs.end_ns;
 uint64_t submit=b->submit_end_ns>=b->at_ns?b->submit_end_ns-b->at_ns:0;int pid=getpid();
 fprintf(stderr,"lancelot-completed-batch: pid=%d ctx=%llx seq=%llu vtq=%llu fsq=%llu at=%llu done=%llu reported=%llu submit_us=%llu vt_jm_us=%llu fs_jm_us=%llu vt_worker_us=%llu fs_worker_us=%llu draws=%u reason=%u w=%u h=%u samples=%u preloads=%x submit_error=%d scope=native_completion\n",
  pid,(unsigned long long)b->ctx,(unsigned long long)b->seq,(unsigned long long)b->vtq,(unsigned long long)b->fsq,
  (unsigned long long)(b->at_ns/1000),(unsigned long long)(done/1000),(unsigned long long)(now/1000),(unsigned long long)(submit/1000),
  (unsigned long long)(p->vt.jm_ns/1000),(unsigned long long)(p->fs.jm_ns/1000),
  (unsigned long long)(p->vt.worker_ns/1000),(unsigned long long)(p->fs.worker_ns/1000),
  b->draws,b->reason,b->w,b->h,b->samples,b->preloads,b->submit_error);
 fprintf(stderr,"lancelot-batch-q: v=2 clock=mach_absolute_time pid=%d ctx=%llx seq=%llu vtq=%llu fsq=%llu at=%llu done=%llu\n",
  pid,(unsigned long long)b->ctx,(unsigned long long)b->seq,(unsigned long long)b->vtq,(unsigned long long)b->fsq,
  (unsigned long long)(b->at_ns/1000),(unsigned long long)(b->submit_end_ns/1000));
 const struct lm_shader_mix *m=&b->mix;
 for(unsigned i=0;i<LM_SHADER_PAIRS;i++){
  if(!m->pair[i].draws)continue;
  for(unsigned stage=0;stage<2;stage++){
   const struct lm_shader_meta *s=stage?&m->fs[i]:&m->vs[i];if(!s->id)continue;
   fprintf(stderr,"lancelot-shader-meta: v=2 pid=%d seq=%llu id=%u stage=%u hash=%s bytes=%u regs=%u tls=%u ubos=%u textures=%u samplers=%u cache=%u compile_us=%llu materialize_us=%llu source=CPU_binary_saved\n",
    pid,(unsigned long long)b->seq,s->id,s->stage,s->hash,s->bytes,s->regs,s->tls,s->ubos,s->textures,s->samplers,s->cache,
    (unsigned long long)s->compile_us,(unsigned long long)s->materialize_us);
  }
  const struct lm_shader_work *w=&m->work[i];
  fprintf(stderr,"lancelot-draw-work: pid=%d ctx=%llx seq=%llu pair=%u n=%u area=%llu full=%u blend=%u fetch=%u fpk=%u sidefx=%u scope=scissor_upper_bound\n",
   pid,(unsigned long long)b->ctx,(unsigned long long)b->seq,i,m->pair[i].draws,(unsigned long long)w->scissor_pixels,
   w->full_framebuffer,w->blend_destination,w->framebuffer_fetch,w->forward_kill,w->side_effects);
 }
 fprintf(stderr,"lancelot-shader-batch: pid=%d ctx=%llx seq=%llu at=%llu done=%llu wall_us=%llu vtjc=%llx fsjc=%llx draws=%u other=%u v0=%u f0=%u n0=%u v1=%u f1=%u n1=%u v2=%u f2=%u n2=%u v3=%u f3=%u n3=%u error=%d helpers=untracked\n",
  pid,(unsigned long long)b->ctx,(unsigned long long)b->seq,(unsigned long long)(b->at_ns/1000),
  (unsigned long long)(b->submit_end_ns/1000),(unsigned long long)(submit/1000),
  (unsigned long long)b->vtjc,(unsigned long long)b->fsjc,b->draws,m->other,
  m->pair[0].vs,m->pair[0].fs,m->pair[0].draws,m->pair[1].vs,m->pair[1].fs,m->pair[1].draws,
  m->pair[2].vs,m->pair[2].fs,m->pair[2].draws,m->pair[3].vs,m->pair[3].fs,m->pair[3].draws,
  b->submit_error?b->submit_error:p->fs.error?p->fs.error:p->vt.error);
 fprintf(stderr,"lancelot-completed-config: pid=%d ctx=%llx seq=%llu x0=%u y0=%u x1=%u y1=%u rts=%u helpers_untracked=%u scope=scissor_upper_bound\n",
  pid,(unsigned long long)b->ctx,(unsigned long long)b->seq,b->x0,b->y0,b->x1,b->y1,b->rts,m->other);
 for(unsigned i=0;i<b->rts&&i<8;i++)
  fprintf(stderr,"lancelot-completed-resource: pid=%d ctx=%llx seq=%llu rt=%u format=%u modifier=%llx\n",
   pid,(unsigned long long)b->ctx,(unsigned long long)b->seq,i,b->format[i],(unsigned long long)b->modifier[i]);
}
/* Run with maps/registry independent, only bridge calls take its own lock. */
static void
lm_profile_poll(struct device *d)
{
 while(d->profile_count){
  struct lm_profile_pending *p=&d->profiles[d->profile_head];
  bool absent=false;
  if(!p->read_vt&&p->batch.vtq){
   int e=lm_profile_query(d,p->batch.vtq,&p->vt);
   if(!e){p->read_vt=true;}
   else if(errno!=EBUSY){absent=true;if(errno!=EINVAL)d->profile_query_errors++;}}
  if(!p->read_fs&&p->batch.fsq){
   int e=lm_profile_query(d,p->batch.fsq,&p->fs);
   if(!e){p->read_fs=true;}
   else if(errno!=EBUSY){absent=true;if(errno!=EINVAL)d->profile_query_errors++;}}
  if(absent){d->profile_missing++;p->used=false;d->profile_head=(d->profile_head+1)%64;d->profile_count--;continue;}
  if((p->read_vt||!p->batch.vtq)&&(p->read_fs||!p->batch.fsq)){
   d->profile_completed++;
   if(p->vt.jm_ns>=10000000u||p->fs.jm_ns>=10000000u||p->vt.error||p->fs.error||p->batch.submit_error){lm_profile_report(p);d->profile_emitted++;}
   p->used=false;
   d->profile_head=(d->profile_head+1)%64;d->profile_count--;
  }
  else break; /* one pending FIFO boundary, no scan of all outstanding jobs */
 }
}
void
lancelot_drm_profile_batch(int fd,const struct lm_completed_batch *batch)
{
 if(!batch||(!batch->vtq&&!batch->fsq))return;
 int saved_errno=errno;struct device*d=get(fd);if(!d){errno=saved_errno;return;}
 if(!(d->caps&LMAL_CAP_JOB_PROFILE)){put(d);errno=saved_errno;return;}
 pthread_mutex_lock(&d->profile_lock);
 lm_profile_poll(d);
 if(d->profile_count==64)d->profile_dropped++;
 else{unsigned slot=(d->profile_head+d->profile_count)%64;memset(&d->profiles[slot],0,sizeof(d->profiles[slot]));
  d->profiles[slot].batch=*batch;d->profiles[slot].used=true;d->profile_count++;}
 if(d->profile_dropped!=d->profile_reported_dropped||d->profile_missing!=d->profile_reported_missing||d->profile_query_errors!=d->profile_reported_errors){
  fprintf(stderr,"lancelot-profile-loss: pid=%d dropped=%llu overwritten=%llu query_errors=%llu pending=%u bounded=64\n",
   getpid(),(unsigned long long)d->profile_dropped,(unsigned long long)d->profile_missing,
   (unsigned long long)d->profile_query_errors,d->profile_count);
  d->profile_reported_dropped=d->profile_dropped;d->profile_reported_missing=d->profile_missing;d->profile_reported_errors=d->profile_query_errors;
 }
 pthread_mutex_unlock(&d->profile_lock);put(d);errno=saved_errno;
}
void
lancelot_drm_profile_poll(int fd)
{
 int saved_errno=errno;struct device*d=get(fd);if(!d){errno=saved_errno;return;}
 if(d->caps&LMAL_CAP_JOB_PROFILE){pthread_mutex_lock(&d->profile_lock);lm_profile_poll(d);pthread_mutex_unlock(&d->profile_lock);}
 put(d);errno=saved_errno;
}
