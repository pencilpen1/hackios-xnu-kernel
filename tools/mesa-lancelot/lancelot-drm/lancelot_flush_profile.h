/* CPU observations of flush lookups, not GPU jobs or completion signals. */
#ifndef LANCELOT_FLUSH_PROFILE_H
#define LANCELOT_FLUSH_PROFILE_H
#include <stdint.h>
struct lm_flush_profile { uint64_t from_ns,calls,missing_fbo,idle; };
#endif
