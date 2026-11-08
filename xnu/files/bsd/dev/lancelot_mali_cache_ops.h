/* Same line walker for XNU and the host coverage fixture. Callers complete
 * each range with DSB SY. Unrolling changes loop overhead, never coverage. */
#ifndef LANCELOT_MALI_CACHE_OPS_H
#define LANCELOT_MALI_CACHE_OPS_H
#include <stdint.h>
#include <stddef.h>
#ifndef LM_HOST_TEST
#define LM_CACHE_CLEAN_LINE(a) __asm__ volatile("dc civac, %0"::"r"(a):"memory")
#define LM_CACHE_CPU_LINE(a) __asm__ volatile("dc ivac, %0"::"r"(a):"memory")
#endif
/* Core ranges are retained, nonwrapping, at most 64 pages; line is a checked
 * power of two in [16,256]. Include both partial boundary lines as before. */
#define LM_CACHE_WALK(name,op) \
static inline void name(void *pointer,size_t bytes,uint32_t line) { \
    if(!bytes)return; \
    uintptr_t base=(uintptr_t)pointer&~(uintptr_t)(line-1); \
    uintptr_t end=(uintptr_t)pointer+bytes; \
    size_t count=(end-1-base)/line+1; \
    while(count>=4) { \
        op(base);op(base+line);op(base+2*line);op(base+3*line); \
        count-=4; if(!count)return; base+=4*line; \
    } \
    while(count) {op(base);if(!--count)return;base+=line;} \
}
LM_CACHE_WALK(lm_cache_clean_lines,LM_CACHE_CLEAN_LINE)
LM_CACHE_WALK(lm_cache_cpu_lines,LM_CACHE_CPU_LINE)
#undef LM_CACHE_WALK
#ifndef LM_HOST_TEST
#undef LM_CACHE_CLEAN_LINE
#undef LM_CACHE_CPU_LINE
#endif
#endif
