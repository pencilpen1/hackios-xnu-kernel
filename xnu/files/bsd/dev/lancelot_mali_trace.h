/* Bounded timing diagnostics for the MT6768 FIFO worker. */
#ifndef LANCELOT_MALI_TRACE_H
#define LANCELOT_MALI_TRACE_H
#include <stdbool.h>
#include <stdint.h>

struct lm_trace_gate {
    uint64_t last_detail_ns;
    uint32_t startup_details;
};

static inline bool
lm_trace_job(struct lm_trace_gate *gate, uint32_t mode,
             uint64_t queued_ns, uint64_t began_ns, uint64_t ended_ns, bool failed)
{
    if (mode == 1) return true;
    if (mode != 2 || !gate) return false;
    if (failed) return true;
    if (began_ns < queued_ns || ended_ns < began_ns) return false;
    if (gate->startup_details < 4) {
        gate->startup_details++;
        gate->last_detail_ns = ended_ns;
        return true;
    }
    if (began_ns - queued_ns < UINT64_C(20000000) &&
        ended_ns - began_ns < UINT64_C(10000000)) return false;
    if (ended_ns >= gate->last_detail_ns &&
        ended_ns - gate->last_detail_ns < UINT64_C(1000000000)) return false;
    gate->last_detail_ns = ended_ns;
    return true;
}
#endif
