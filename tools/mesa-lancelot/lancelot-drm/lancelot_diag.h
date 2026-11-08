/* Diagnostic-only Mach clock. Timeout/cache clocks keep their own semantics. */
#ifndef LANCELOT_DIAG_H
#define LANCELOT_DIAG_H
#include <stdint.h>
uint64_t lancelot_diag_ns(void);
void lancelot_diag_anchor(void);
/* Calling thread's last successful synchronous submit, zero on failure. */
uint64_t lancelot_drm_last_submit(void);
#endif
