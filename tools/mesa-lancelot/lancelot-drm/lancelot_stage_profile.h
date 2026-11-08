/* Per-context scalar observations of submitted worksets, not GPU timing or
 * ownership decisions. Omitted bytes are BO metadata, not cache bytes saved. */
#ifndef LANCELOT_STAGE_PROFILE_H
#define LANCELOT_STAGE_PROFILE_H
#include <stdint.h>
struct lm_stage_profile {
    uint64_t from_ns,vt_calls,fs_calls,original_refs,kept_refs,omitted_refs,omitted_bytes,errors;
    uint64_t vt_omitted_refs,fs_omitted_refs;
    uint64_t deferred_maps,avoided_old_maps;
};
#endif
