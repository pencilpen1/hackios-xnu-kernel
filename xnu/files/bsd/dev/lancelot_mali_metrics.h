/* Read-only derived clock/tick arithmetic, shared with host fixtures. No MMIO. */
#ifndef LANCELOT_MALI_METRICS_H
#define LANCELOT_MALI_METRICS_H
#include <stdint.h>
#include <stdbool.h>
static inline bool lm_metrics_cpu_hz(uint32_t con0,uint32_t con1,uint32_t cfg,uint64_t *hz) {
    *hz=0;unsigned mux=(cfg>>9)&3u,div=(cfg>>17)&31u,pos=(con1>>24)&7u;
    if(!mux){*hz=26000000u;return true;}
    if(mux!=1||!(con0&1)||pos>4||!(con1&0x3fffffu))return false;
    unsigned n=1,d=1;
    switch(div) {
    case 0:case 8:case 16:case 24:break;
    case 9:n=3;d=4;break;case 10:n=1;d=2;break;case 11:n=1;d=4;break;
    case 17:n=4;d=5;break;case 18:n=3;d=5;break;case 19:n=2;d=5;break;case 20:n=1;d=5;break;
    case 25:n=5;d=6;break;case 26:n=4;d=6;break;case 27:n=3;d=6;break;case 28:n=2;d=6;break;case 29:n=1;d=6;break;
    default:return false;
    }
    *hz=UINT64_C(26000000)*(con1&0x3fffffu)*n/(UINT64_C(16384)*(1u<<pos)*d);
    if(!*hz||*hz>UINT64_C(3000000000)){*hz=0;return false;}return true;
}
static inline bool lm_metrics_ticks(const uint32_t before[4],const uint32_t now[4],uint32_t delta[4]) {
    uint64_t total=0;
    for(unsigned i=0;i<4;i++){delta[i]=now[i]-before[i];if(delta[i]>=0x80000000u)return false;total+=delta[i];}
    return total!=0;
}
static inline uint32_t lm_metrics_busy_bps(const uint32_t delta[4],unsigned idle) {
    uint64_t total=0;for(unsigned i=0;i<4;i++)total+=delta[i];
    return total?(uint32_t)((total-delta[idle])*10000u/total):0;
}
#endif
