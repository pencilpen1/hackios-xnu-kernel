/* Observational per-context counters. No graphics pointers or device access. */
#ifndef LANCELOT_BATCH_PROFILE_H
#define LANCELOT_BATCH_PROFILE_H
#include <stdint.h>
#include <stdbool.h>
enum lm_batch_reason {
    LM_BATCH_EVICT, LM_BATCH_FRESH, LM_BATCH_READ_WRITER, LM_BATCH_WRITE_USER,
    LM_BATCH_FLUSH_CURRENT, LM_BATCH_FLUSH_OTHER, LM_BATCH_SYNC_WRITER,
    LM_BATCH_SYNC_USERS, LM_BATCH_REASONS
};
struct lm_batch_cost { uint64_t count,ns,draws,bos,max_ns; };
struct lm_batch_profile {
    uint64_t from_ns,to_ns,invalid;
    bool started;
    struct lm_batch_cost reason[LM_BATCH_REASONS];
};
static inline void
lm_batch_record(struct lm_batch_profile *p,unsigned reason,uint64_t begin,
                uint64_t end,uint32_t draws,uint32_t bos)
{
    if(reason>=LM_BATCH_REASONS||end<begin) {p->invalid++;return;}
    if(!p->started) {p->started=true;p->from_ns=begin;}
    p->to_ns=end;
    struct lm_batch_cost *c=&p->reason[reason];uint64_t cost=end-begin;
    c->count++;c->ns+=cost;c->draws+=draws;c->bos+=bos;
    if(cost>c->max_ns)c->max_ns=cost;
}
static inline bool lm_batch_report_due(const struct lm_batch_profile *p)
{
    return p->started&&p->to_ns>=p->from_ns&&
           p->to_ns-p->from_ns>=UINT64_C(1000000000);
}
#endif
