/* Production loop expansion against an independent scalar coverage oracle. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
static uintptr_t next,end;
static uint32_t stride;
static unsigned expected_cpu,seen;
static void visit(uintptr_t address,unsigned cpu) {
    assert(cpu==expected_cpu&&address==next&&address<end);
    seen++;next+=stride;
}
#define LM_CACHE_CLEAN_LINE(a) visit(a,0)
#define LM_CACHE_CPU_LINE(a) visit(a,1)
#include "lancelot_mali_cache_ops.h"
static void check(uintptr_t start,size_t bytes,uint32_t line,unsigned cpu) {
    stride=line;expected_cpu=cpu;seen=0;next=start&~(uintptr_t)(line-1);end=start+bytes;
    if(cpu)lm_cache_cpu_lines((void*)start,bytes,line);
    else lm_cache_clean_lines((void*)start,bytes,line);
    unsigned count=0;
    if(bytes)for(uintptr_t a=start&~(uintptr_t)(line-1);a<end;a+=line)count++;
    assert(count==seen);
}
int main(void) {
    for(unsigned cpu=0;cpu<2;cpu++)for(uint32_t line=16;line<=256;line*=2) {
        for(unsigned offset=0;offset<line;offset++)for(unsigned n=0;n<=8*line+1;n++)
            check(0x100000+offset,n,line,cpu);
        for(unsigned pages=1;pages<=64;pages++)check(0x100000,pages*4096,line,cpu);
        check(UINTPTR_MAX-0xffff,0xf000,line,cpu);
    }
    puts("cache lines: actual unrolled clean/invalidate walker matches scalar every-line oracle, partial boundaries, 16..256-byte lines and 1..64-page ranges PASS");
    return 0;
}
