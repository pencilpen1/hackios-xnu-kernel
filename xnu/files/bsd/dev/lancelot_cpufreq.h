/* MT6768 CPU_LEVEL_6 hybrid-DVFS request/feedback contract.
 * Reference: Shockwave 224fe470, mtk_cpufreq_hybrid.c; verified Android tables
 * and WFI request slots on lancelot. Firmware owns PLL/rails/CCI sequencing.
 * Cold table/mailbox initialization is isolated in lancelot_cpufreq_sspm.h;
 * raw PLL and PMIC voltage writes remain forbidden. */
#ifndef LANCELOT_CPUFREQ_H
#define LANCELOT_CPUFREQ_H
#include <stdint.h>
#include <stdbool.h>
#include "lancelot_cpufreq_tables.h"
#define LCF_SRAM_PA UINT64_C(0x00110c00)
#define LCF_SRAM_SIZE 0x1400u
#define LCF_REQUEST 0x37cu
#define LCF_CURRENT 0x354u
#define LCF_LIMIT 0x320u
#define LCF_LOG 0x3d0u
#define LCF_LOG_COUNT 150u
#define LCF_CCI_MAP 0xfa0u
#define LCF_REJECT_GUARD 1u
#define LCF_REJECT_TABLE 2u
#define LCF_REJECT_INDEX 4u
#define LCF_REJECT_LIMIT 8u
#define LCF_REJECT_STATE 16u
#define LCF_REJECT_LOG 32u
#define LCF_REJECT_TORN 64u
#define LCF_REJECT_CCI 128u
static const uint16_t lcf_mhz[3][16] = {
    {1800,1625,1500,1450,1375,1325,1275,1175,1100,1050,999,950,900,850,774,500},
    {2000,1950,1900,1850,1800,1710,1621,1532,1443,1354,1295,1176,1087,998,909,850},
    {1277,1120,1049,1014,961,909,856,821,768,733,698,663,628,593,558,500}
};
struct lcf_snapshot {
    uint32_t guard[5], guard_end[5], pvt[3][16], method[3][16], sentinel[3];
    uint32_t current[3], request[2], limit[2];
    uint32_t sched_disabled, stress, pause, sched[2];
    uint32_t cci[sizeof(lcf_cci_map)/4];
    uint64_t latest_log;
};
/* Stale SRAM by itself is never a live firmware admission. The adapter also
 * requires advancing firmware logs and stable PLL/current-index agreement. */
static inline uint32_t lcf_snapshot_reject(const struct lcf_snapshot *s)
{
    uint32_t reject=0;
    if (s->guard[0]!=0x55aa55aa || s->guard[1]!=0xaa55aa55 ||
         s->guard[2]!=0x55aa55aa || s->guard[3]!=0xaa55aa55 ||
         s->guard[4]!=0x55aa55aa) reject|=LCF_REJECT_GUARD;
    for (unsigned i=0;i<5;i++) if (s->guard[i]!=s->guard_end[i]) reject|=LCF_REJECT_TORN;
    /*0/1 controls ONLY schedule assist. WFI votes still work with assist off. */
    if (s->sched_disabled>1 || s->stress || s->pause) reject|=LCF_REJECT_STATE;
    if (!s->latest_log) reject|=LCF_REJECT_LOG;
    for (unsigned c=0;c<3;c++) {
        if (s->current[c]>=16) reject|=LCF_REJECT_INDEX;
        if (s->sentinel[c]!=UINT32_MAX) reject|=LCF_REJECT_TABLE;
        for (unsigned i=0;i<16;i++) {
            uint32_t row=s->pvt[c][i], selector=row>>16;
            uint32_t method=s->method[c][i], pos=method&0xffff, div=method>>16;
            if ((row&0xffff)!=lcf_mhz[c][i] || selector<24 || selector>96 ||
                (i && selector>(s->pvt[c][i-1]>>16)) ||
                (pos!=1 && pos!=2 && pos!=4) || div!=1) reject|=LCF_REJECT_TABLE;
            if (selector!=lcf_voltage[c][i] || pos!=lcf_postdiv[c][i]) reject|=LCF_REJECT_TABLE;
        }
    }
    for (unsigned c=0;c<2;c++) {
        uint32_t limit=s->limit[c]>>16, base=s->limit[c]&0xffff;
        if (limit>base || base>=16) reject|=LCF_REJECT_LIMIT;
        if (s->request[c]>=16) reject|=LCF_REJECT_INDEX;
        if (s->sched[c]) reject|=LCF_REJECT_STATE;
    }
    for (unsigned i=0;i<sizeof(lcf_cci_map)/4;i++) {
        const uint8_t *p=(const uint8_t *)lcf_cci_map+i*4;
        uint32_t want=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        if (s->cci[i]!=want) reject|=LCF_REJECT_CCI;
    }
    return reject;
}
static inline bool lcf_snapshot_valid(const struct lcf_snapshot *s)
{
    return lcf_snapshot_reject(s)==0;
}
/* MT6768 get_cur_phy_freq()/pll_to_clk(): admit enumerated encodings only.
 * Encoding0 is the observed loader/SSPM unity state: get_clkdiv returns1 and
 * set_cur_freq_wrapper does not rewrite an unchanged ratio. In r2 real SSPM
 * ACKs and the requested500/850MHz PLL changes retained cfg.div0. Admit that
 * specific encoding, not every default case in the reference decoder. */
static inline bool lcf_clock_hz(uint32_t con0, uint32_t con1, uint32_t cfg,
    uint64_t *hz)
{
    *hz=0;
    unsigned pos=(con1>>24)&7, div=(cfg>>17)&31, n=1, d=1;
    if (!(con0&1) || ((cfg>>9)&3)!=1 || pos>4 || !(con1&0x3fffff)) return false;
    switch (div) {
    case 0: case 8: case 16: case 24: break;
    case 9:n=3;d=4;break;case 10:d=2;break;case 11:d=4;break;
    case 17:n=4;d=5;break;case 18:n=3;d=5;break;case 19:n=2;d=5;break;case 20:d=5;break;
    case 25:n=5;d=6;break;case 26:n=4;d=6;break;case 27:n=3;d=6;break;case 28:n=2;d=6;break;case 29:d=6;break;
    default:return false;
    }
    *hz=UINT64_C(26000000)*(con1&0x3fffff)*n/(UINT64_C(16384)*(1u<<pos)*d);
    return *hz!=0 && *hz<=UINT64_C(3000000000);
}
static inline bool lcf_request_allowed(uint32_t request, uint32_t limits)
{
    unsigned limit=limits>>16, base=limits&0xffff;
    return request<16 && limit<=base && base<16 && request>=limit && request<=base;
}
/* A cold loader state is not yet an SSPM current-OPP contract. Keep the
 * conservative lowest initial request for inherited encoding0; do not treat
 * the boot PLL read alone as a successfully initialized firmware OPP.
 * Current index, decoded PLL, voltage and log must pass runtime checks.
 * All other unsupported encodings still reject before table/mailbox writes. */
static inline bool lcf_initial_request(unsigned cluster, uint32_t con0,
    uint32_t con1, uint32_t cfg, unsigned *request, bool *frequency_known)
{
    if (cluster>1 || !request || !frequency_known) return false;
    *request=16;*frequency_known=false;
    if (((cfg>>17)&31)==0) {
        if (!(con0&1) || ((cfg>>9)&3)!=1 || ((con1>>24)&7)>4 || !(con1&0x3fffff)) return false;
        *request=15;
        return true;
    }
    uint64_t hz;
    if (!lcf_clock_hz(con0,con1,cfg,&hz)) return false;
    for (unsigned i=0;i<16;i++) {
        uint64_t want=(uint64_t)lcf_mhz[cluster][i]*1000000;
        if (hz+1000000>=want && hz<=want+1000000) {
            unsigned cap=0; /* Rated CPU_LEVEL_6 maxima: LL1800 / L2000MHz. */
            *request=i<cap?cap:i;*frequency_known=true;
            return true;
        }
    }
    return false;
}
/* Busiest logical CPU in the physical cluster, not average across idle cores.
 * Admit the rated CPU_LEVEL_6 maximum, not a permanent maximum-frequency vote.
 * All targets still obey load policy, firmware/rail feedback and PPM limits. */
static inline unsigned lcf_target(unsigned cluster, unsigned busy,
    unsigned current, uint32_t limits)
{
    unsigned cap=0, limit=limits>>16, base=limits&0xffff;
    if (cluster>1 || current>=16 || busy>100 || limit>base || base>=16) return 16;
    if (limit<cap) limit=cap;
    if (base<limit) return 16; /* Conflicting floor: do not override it. */
    unsigned target=current;
    /* GPU/IO sleeps can make an interactive CPU burst a modest duty cycle.
     * Reach a moderate floor on real >=15% load, then raise in bounded steps.
     * Every step still waits for firmware/PLL/rail feedback in the adapter. */
    if (busy>=65 && target>limit) target=target-limit>3?target-3:limit;
    else if (busy>=35 && target>limit) target--;
    else if (busy>=15) {
        unsigned floor=cluster?12:10;
        if (floor<limit) floor=limit;
        if (target>floor) target=floor;
    } else if (target<base) target++;
    if (target<limit) target=limit;
    if (target>base) target=base;
    return target;
}
struct lcf_load_policy { uint64_t hold_until[2]; unsigned low[2]; };
static inline unsigned lcf_policy_target(struct lcf_load_policy *p,unsigned c,
    unsigned busy,unsigned current,uint32_t limits,uint64_t at_ns,bool boost)
{
    unsigned target=lcf_target(c,busy,current,limits);
    if (!p || c>1 || target>=16) return 16;
    if (!boost) {p->hold_until[c]=0;p->low[c]=0;return limits&0xffff;}
    if (busy>=15) {
        p->low[c]=0;
        p->hold_until[c]=at_ns<=UINT64_MAX-UINT64_C(300000000)?at_ns+UINT64_C(300000000):UINT64_MAX;
    } else {
        if (p->low[c]<2) p->low[c]++;
        if ((at_ns<p->hold_until[c] || p->low[c]<2) && target>current)
            target=lcf_request_allowed(current,limits)?current:target;
    }
    /* A tightening PPM limit always wins over the hold. */
    unsigned limit=limits>>16,cap=0;
    if (limit<cap) limit=cap;
    if (target<limit) target=limit;
    return target;
}
#endif
