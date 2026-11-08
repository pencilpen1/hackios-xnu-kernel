/* Serialized, allocation-free GPU diagnostic policy. Not part of the user ABI. */
#ifndef LANCELOT_MALI_PROFILE_H
#define LANCELOT_MALI_PROFILE_H
#include <stdbool.h>
#include <stdint.h>
#define LM_PROFILE_STARTUP_JOBS 192u
#define LM_PROFILE_LEGACY_PAIRS 384u
#define LM_PROFILE_STEADY_NS UINT64_C(1000000000)
struct lm_profile_policy {
    uint32_t legacy_pairs;
    uint64_t last_ns[2];
    bool seen[2];
};
/* 0 off; 1 unchanged legacy cap; 2 first192, then <=1 pair/s per requirement.
 * Requirements 0 and 1 are sampled independently so a frequently submitted
 * vertex/tiler job cannot starve the fragment measurement (or conversely).
 * Call once after each completed serialized submission, with its real sequence.
 * No timeout, cache, ownership, launch, completion or recovery decision uses it. */
static inline bool lm_profile_sample(struct lm_profile_policy *p,uint32_t mode,
    uint64_t sequence,uint32_t requirements,uint64_t now_ns)
{
    if(!p||!sequence||requirements>1||!mode||mode>2)return false;
    if(mode==1) {
        if(p->legacy_pairs>=LM_PROFILE_LEGACY_PAIRS ||
           (sequence>LM_PROFILE_STARTUP_JOBS&&(sequence%16)))return false;
        p->legacy_pairs++;return true;
    }
    if(sequence>LM_PROFILE_STARTUP_JOBS&&p->seen[requirements]&&
       (now_ns<p->last_ns[requirements]||now_ns-p->last_ns[requirements]<LM_PROFILE_STEADY_NS))return false;
    p->seen[requirements]=true;p->last_ns[requirements]=now_ns;return true;
}
#endif
