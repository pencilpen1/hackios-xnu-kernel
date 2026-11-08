/* SPDX-License-Identifier: MIT
 * Bounded copies of batch metadata. Completion polling never waits or holds
 * rendering objects; it only prints snapshots when native JM work was slow.
 */
#ifndef LANCELOT_COMPLETION_PROFILE_H
#define LANCELOT_COMPLETION_PROFILE_H
#include "lancelot_shader_profile.h"
struct lm_completed_batch {
    uint64_t ctx,seq,vtq,fsq,at_ns,submit_end_ns;
    uint32_t draws,reason,w,h,x0,y0,x1,y1,samples,rts,preloads;
    uint32_t format[8];uint64_t modifier[8];
    uint64_t vtjc,fsjc;
    int32_t submit_error;
    struct lm_shader_mix mix;
};
#ifdef __cplusplus
extern "C" {
#endif
void lancelot_drm_profile_batch(int fd,const struct lm_completed_batch *batch);
void lancelot_drm_profile_poll(int fd);
#ifdef __cplusplus
}
#endif
#endif
