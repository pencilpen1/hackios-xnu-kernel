/* SPDX-License-Identifier: APSL-2.0
 * Same-clock metadata for GPU diagnostics, sampled independently of GPU work.
 */
#ifndef LANCELOT_MALI_CPU_PROFILE_H
#define LANCELOT_MALI_CPU_PROFILE_H
#include <stdint.h>
struct lancelot_cpu_profile {
    uint64_t sampled_ns,from_ns[8],hz[2];
    uint32_t busy_bps[8],valid_mask,hz_valid_mask;
    int32_t domain[8];
};
#endif
