/* Fixed-size first observed fatal failure; caller holds the existing Mali lock.
 * This is diagnostics only: it cannot clear offline/poison/quarantine, allocate,
 * access MMIO, release backing, or launch a replacement job. Zero-init once. */
#ifndef LANCELOT_MALI_FAULT_H
#define LANCELOT_MALI_FAULT_H
#include "lancelot_mali_core.h"
#define LM_FIRST_FAILURE_REPORT_NS UINT64_C(5000000000)
struct lm_first_failure {
    bool valid,reported;
    int32_t error,pid;
    uint32_t requirements;
    uint64_t at_ns,jc,report_ns;
    struct lm_job_result result;
};
static inline bool lm_first_failure_save(struct lm_first_failure *s,bool offline,
    int32_t error,int32_t pid,uint32_t requirements,uint64_t at_ns,uint64_t jc,
    const struct lm_job_result *r)
{
    if(!s||s->valid||!offline||!error||!r)return false;
    s->error=error;s->pid=pid;s->requirements=requirements;s->at_ns=at_ns;s->jc=jc;
    s->result=*r;s->valid=true;return true;
}
static inline bool lm_first_failure_should_report(struct lm_first_failure *s,uint64_t now_ns)
{
    if(!s||!s->valid)return false;
    if(s->reported&&(now_ns<s->report_ns||now_ns-s->report_ns<LM_FIRST_FAILURE_REPORT_NS))return false;
    s->reported=true;s->report_ns=now_ns;return true;
}
#endif
