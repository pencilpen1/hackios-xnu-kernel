/* MT6768 DDRPHY/DRAMC observation. Register fields and integer rate rounding:
 * LineageOS/android_kernel_xiaomi_mt6768@70cad0c271a0bba8765730dc3d55542fc38f1e76,
 * drivers/misc/mediatek/dramc/mt6768/mtk_dramc.c. No write operation exists here. */
#ifndef LANCELOT_DRAM_PROFILE_H
#define LANCELOT_DRAM_PROFILE_H
#include <stdint.h>
#include <stdbool.h>

enum lmd_bank { LMD_DRAMC, LMD_DDRPHY, LMD_EMI, LMD_BANKS };
struct lmd_io { void *cookie; int (*read32)(void *,enum lmd_bank,uint32_t,uint32_t *); };
struct lmd_clock {
    uint32_t shuffle,pll,pcw,div,ckdiv,cona,raw_rate,mtps,channels;
    int error;
    bool valid;
};
static inline bool lmd_read_allowed(enum lmd_bank bank,uint32_t off) {
    if(bank==LMD_DRAMC)return off==0xe4;
    if(bank==LMD_EMI)return off==0;
    if(bank!=LMD_DDRPHY)return false;
    if(off==0x510)return true;
    for(unsigned level=0;level<3;level++) {
        uint32_t base=0x500*level;
        if(off==base+0xd9c || off==base+0xd94 || off==base+0xda8 ||
           off==base+0xda0 || off==base+0xd18)return true;
    }
    return false;
}
static inline int lmd_read(const struct lmd_io *io,enum lmd_bank bank,uint32_t off,uint32_t *value) {
    if(!io || !io->read32 || !value || !lmd_read_allowed(bank,off))return -22;
    return io->read32(io->cookie,bank,off,value);
}
static inline int lmd_sample(const struct lmd_io *io,struct lmd_clock *s) {
    uint32_t status=0,select=0,status_after=0,select_after=0;
    int error=lmd_read(io,LMD_DRAMC,0xe4,&status);
    if(error)return error;
    s->shuffle=(status>>1)&3;
    if(s->shuffle>2)return -95;
    error=lmd_read(io,LMD_DDRPHY,0x510,&select);if(error)return error;
    s->pll=select>>31;
    uint32_t base=0x500*s->shuffle;
    error=lmd_read(io,LMD_DDRPHY,base+(s->pll?0xd94:0xd9c),&s->pcw);if(error)return error;
    error=lmd_read(io,LMD_DDRPHY,base+(s->pll?0xda0:0xda8),&s->div);if(error)return error;
    error=lmd_read(io,LMD_DDRPHY,base+0xd18,&s->ckdiv);if(error)return error;
    error=lmd_read(io,LMD_EMI,0,&s->cona);if(error)return error;
    error=lmd_read(io,LMD_DRAMC,0xe4,&status_after);if(error)return error;
    error=lmd_read(io,LMD_DDRPHY,0x510,&select_after);if(error)return error;
    if(((status_after>>1)&3)!=s->shuffle || (select_after>>31)!=s->pll)return -11;
    return 0;
}
static inline int lmd_capture(const struct lmd_io *io,struct lmd_clock *out) {
    if(!out)return -22;
    *out=(struct lmd_clock){0};
    struct lmd_clock a={0},b={0};
    int error=lmd_sample(io,&a);
    if(!error)error=lmd_sample(io,&b);
    if(!error && (a.shuffle!=b.shuffle || a.pll!=b.pll || a.pcw!=b.pcw ||
                 a.div!=b.div || a.ckdiv!=b.ckdiv || a.cona!=b.cona))error=-11;
    *out=a;
    if(!error) {
        unsigned channel_code=(a.cona>>8)&3;
        out->channels=channel_code<2?1u<<channel_code:0;
        out->raw_rate=((52u>>((a.div>>18)&3))*(a.pcw>>24))>>(a.div&7);
        out->raw_rate>>=(a.ckdiv>>27)&1;
        /* Lancelot's recorded DRAM type is LPDDR4X. Unknown encodings stay
         * invalid; no assumed fast OPP or extrapolated bandwidth is reported. */
        switch(out->raw_rate) {
        case 3588:out->mtps=3600;break;
        case 3198:out->mtps=3200;break;
        case 2392:out->mtps=2400;break;
        case 1599:out->mtps=1600;break;
        case 1534:out->mtps=1534;break;
        default:error=-95;break;
        }
        if(!out->channels)error=-95;
    }
    out->error=error;out->valid=error==0;
    if(error)out->mtps=0;
    return error;
}
#endif
