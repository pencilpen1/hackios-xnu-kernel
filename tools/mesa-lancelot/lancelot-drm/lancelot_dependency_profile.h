/* CPU metadata only: bounded resource/producer/consumer observations. */
#ifndef LANCELOT_DEPENDENCY_PROFILE_H
#define LANCELOT_DEPENDENCY_PROFILE_H
#include <stdint.h>
#include <stdbool.h>
#define LM_DEP_SLOTS 32u
struct lm_dep_identity {
    uint64_t resource,va,bytes,producer_fb,consumer_fb;
    uint32_t handle,kind,bind,width,height,target;
};
struct lm_dep_cost {
    struct lm_dep_identity key;
    uint64_t count,ns,max_ns,producer_draws,consumer_draws;
};
struct lm_dependency_profile {
    uint64_t first_ns,last_ns,overflow_count,overflow_ns;
    bool started;
    struct lm_dep_cost slots[LM_DEP_SLOTS];
};
static inline bool lm_dep_same(const struct lm_dep_identity *a,const struct lm_dep_identity *b) {
    return a->resource==b->resource&&a->va==b->va&&a->handle==b->handle&&a->kind==b->kind&&
           a->producer_fb==b->producer_fb&&a->consumer_fb==b->consumer_fb&&a->bytes==b->bytes&&
           a->bind==b->bind&&a->width==b->width&&a->height==b->height&&a->target==b->target;
}
static inline void lm_dep_record(struct lm_dependency_profile *p,const struct lm_dep_identity *key,
                                uint64_t begin,uint64_t end,uint32_t producer_draws,uint32_t consumer_draws) {
    if(end<begin)return;
    if(!p->started){p->started=true;p->first_ns=begin;}
    p->last_ns=end;
    for(unsigned i=0;i<LM_DEP_SLOTS;i++) {
        struct lm_dep_cost *c=&p->slots[i];
        if(c->count&&!lm_dep_same(&c->key,key))continue;
        if(!c->count)c->key=*key;
        c->count++;c->ns+=end-begin;c->producer_draws+=producer_draws;c->consumer_draws+=consumer_draws;
        if(end-begin>c->max_ns)c->max_ns=end-begin;
        return;
    }
    p->overflow_count++;p->overflow_ns+=end-begin;
}
static inline bool lm_dep_due(const struct lm_dependency_profile *p) {
    return p->started&&p->last_ns>=p->first_ns&&p->last_ns-p->first_ns>=UINT64_C(1000000000);
}
struct lm_texture_profile {
    uint64_t first_ns,last_ns,calls,trimmed_calls,zero_shader_calls,omitted_slots,omitted_views,omitted_bytes;
    bool started;
};
#endif
