/* Serialized JM wall-occupancy accounting, not HW utilization or authority.
 * Feed only real launch-to-verified-idle intervals, independently of profiling.
 * No hardware access, completion synthesis, permission or applied-rate flag. */
#ifndef LANCELOT_MALI_LOAD_H
#define LANCELOT_MALI_LOAD_H
#include "lancelot_mali_dvfs.h"
#define LGD_WINDOW_NS UINT64_C(50000000)
#define LGD_MAX_CATCHUP_WINDOWS 256u
/* Experimental latency policy for short UI render/blit stages. This is a JM
 * wall-time target, not a frame deadline or claimed HW utilization. Only real
 * launch->fresh idle intervals can request/refresh the bounded burst hold. */
#define LGD_JOB_TARGET_NS UINT64_C(2000000)
#define LGD_BURST_HOLD_NS UINT64_C(1000000000)
/* Demand only. A long measured JM interval requests enough OPP headroom to
 * approach the target in one controller transaction instead of repeatedly
 * parking/reprogramming by two OPPs. Live platform caps still clamp it. */
static inline uint32_t lgd_latency_opp(uint32_t current,uint64_t job_ns) {
    struct lgd_opp from,goal;
    if(!lgd_get_opp(current,&from))return LGD_OPP_COUNT-1u;
    if(job_ns<LGD_JOB_TARGET_NS)return current;
    uint32_t target=current>1u?current-2u:0u;
    if(job_ns>UINT64_MAX/from.khz)return 0;
    uint64_t demand=job_ns*from.khz;
    while(target>0) {
        (void)lgd_get_opp(target,&goal);
        if(demand<=LGD_JOB_TARGET_NS*goal.khz)break;
        --target;
    }
    return target;
}
struct lgd_sampler {
    struct lgd_governor governor;
    struct lgd_load_window window,last;
    uint64_t cursor_ns,windows,discontinuities;
    uint32_t current_opp,demand_opp;
    uint32_t policy_opp,burst_opp;
    uint64_t burst_until_ns,last_job_ns,max_job_ns,pressure_jobs;
    uint64_t applied_up,applied_down;
    bool burst_valid;
    bool running,demand_valid;
};
static inline bool lgd_sampler_start(struct lgd_sampler *s,uint64_t now,uint32_t current) {
    if(!s||current>=LGD_OPP_COUNT)return false;
    *s=(struct lgd_sampler){.window={.start_ns=now,.valid=true},.cursor_ns=now,
        .current_opp=current,.demand_opp=current,.policy_opp=current,.burst_opp=current,.running=true};
    return true;
}
/* A clock discontinuity/missing interval breaks hysteresis, not permission.
 * No long suspend gap is disguised as a measured zero-job idle window. */
static inline void lgd_sampler_break(struct lgd_sampler *s,uint64_t now) {
    if(!s||!s->running)return;
    s->discontinuities++;s->governor=(struct lgd_governor){0};
    s->window=(struct lgd_load_window){.start_ns=now,.valid=true};
    s->last.valid=false;s->cursor_ns=now;s->demand_valid=false;
    s->burst_valid=false;s->burst_until_ns=0;s->policy_opp=s->current_opp;
}
static inline void lgd_sampler_stop(struct lgd_sampler *s) {
    if(s){s->running=false;s->demand_valid=false;s->window.valid=false;s->last.valid=false;
        s->burst_valid=false;s->burst_until_ns=0;}
}
static inline void lgd_sampler_demand(struct lgd_sampler *s,uint64_t now) {
    if(s->burst_valid&&now>=s->burst_until_ns)s->burst_valid=false;
    s->demand_opp=s->policy_opp;
    if(s->burst_valid) {
        if(s->burst_opp<s->demand_opp)s->demand_opp=s->burst_opp;
        s->demand_valid=true;
    }
}
static inline bool lgd_sampler_hold(struct lgd_sampler *s,uint32_t target,uint64_t at) {
    if(target>=LGD_OPP_COUNT||at>UINT64_MAX-LGD_BURST_HOLD_NS) {
        lgd_sampler_break(s,at);return false;
    }
    if(!s->burst_valid||at>=s->burst_until_ns)s->burst_opp=target;
    else if(target<s->burst_opp)s->burst_opp=target;
    s->burst_until_ns=at+LGD_BURST_HOLD_NS;s->burst_valid=true;
    return true;
}
/* Called only AFTER the real controller's complete guard/cap/readback/meter
 * transaction succeeded. Demand never pretends to be applied frequency. */
static inline bool lgd_sampler_applied(struct lgd_sampler *s,uint32_t current) {
    if(!s||!s->running||current>=LGD_OPP_COUNT)return false;
    if(current<s->current_opp)s->applied_up++;
    if(current>s->current_opp)s->applied_down++;
    s->current_opp=current;return true;
}
/* Global serialization makes accepted JM intervals disjoint. A job spanning
 * bucket boundaries is counted in each overlapping bucket, not as extra
 * launched jobs. At most 256 complete windows are processed per call. */
static inline bool lgd_sampler_advance(struct lgd_sampler *s,uint64_t end,bool busy) {
    if(!s||!s->running)return false;
    if(end<s->cursor_ns||s->cursor_ns<s->window.start_ns||
       s->cursor_ns-s->window.start_ns>=LGD_WINDOW_NS||
       end-s->window.start_ns>LGD_WINDOW_NS*LGD_MAX_CATCHUP_WINDOWS) {
        lgd_sampler_break(s,end);return false;
    }
    while(s->cursor_ns<end) {
        if(s->window.start_ns>UINT64_MAX-LGD_WINDOW_NS) {lgd_sampler_break(s,end);return false;}
        uint64_t boundary=s->window.start_ns+LGD_WINDOW_NS;
        uint64_t to=end<boundary?end:boundary;
        if(busy) {
            if(s->window.jobs==UINT32_MAX) {lgd_sampler_break(s,end);return false;}
            s->window.busy_ns+=to-s->cursor_ns;s->window.jobs++;
        }
        s->cursor_ns=to;
        if(to==boundary) {
            s->window.end_ns=boundary;s->last=s->window;
            uint32_t target;
            s->demand_valid=lgd_choose(&s->governor,s->current_opp,&s->window,&target);
            if(s->demand_valid) {
                s->policy_opp=target;
                /* A later empty bucket must not erase a genuine busy-bucket
                 * up request before the next quiescent controller call. */
                if(target<s->current_opp&&!lgd_sampler_hold(s,target,boundary))return false;
            }
            s->windows++;
            s->window=(struct lgd_load_window){.start_ns=boundary,.valid=true};
        }
    }
    lgd_sampler_demand(s,end);
    return true;
}
static inline bool lgd_sampler_idle(struct lgd_sampler *s,uint64_t now) {
    return lgd_sampler_advance(s,now,false);
}
static inline bool lgd_sampler_job(struct lgd_sampler *s,uint64_t start,uint64_t end) {
    if(!s||!s->running)return false;
    if(start<s->cursor_ns||end<=start) {lgd_sampler_break(s,end);return false;}
    bool refresh=s->burst_valid&&start<s->burst_until_ns;
    uint32_t held=s->burst_opp;
    if(!lgd_sampler_advance(s,start,false)) {lgd_sampler_break(s,end);return false;}
    if(!lgd_sampler_advance(s,end,true))return false;
    s->last_job_ns=end-start;
    if(s->last_job_ns>s->max_job_ns)s->max_job_ns=s->last_job_ns;
    if(s->last_job_ns>=LGD_JOB_TARGET_NS) {
        uint32_t target=lgd_latency_opp(s->current_opp,s->last_job_ns);
        if(!lgd_sampler_hold(s,target,end))return false;
        s->pressure_jobs++;
    } else if(refresh&&!lgd_sampler_hold(s,held,end))return false;
    lgd_sampler_demand(s,end);return true;
}
#endif
