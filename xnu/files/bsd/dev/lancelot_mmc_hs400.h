/* MT6768 8-bit / 1.8 V HS200 -> HS400 negotiation. Board capabilities and
 * initial DS delay come from lancelot's live Android DT, not another device.
 * Delay taps are measured anew; no successful Android tap is assumed valid
 * across boots. Failure prevents mounting/writing in an unverified mode. */
#ifndef LANCELOT_MMC_HS400_H
#define LANCELOT_MMC_HS400_H
#include "lancelot_mmc_mode.h"
#define LH_TOP 0x1000u
struct lancelot_hs400 {
    uint32_t stage,hs200_map,hs200_edge,hs200_tap,hs400_map,hs400_tap,depth;
    uint64_t clock_hz;
};
static unsigned lh_tap(uint32_t map,unsigned *length) {
    unsigned best=0,start=0;
    for(unsigned i=0;i<32;) {
        if(!(map&(1u<<i))){i++;continue;}
        unsigned first=i;
        do{i++;}while(i<32&&(map&(1u<<i)));
        if(i-first>best){start=first;best=i-first;}
    }
    *length=best;
    /* Delay cells form a bounded line, not a circular phase ring. */
    return best ? start+(start?best/2:best/3) : 32;
}
static void lh_field(struct lancelot_msdc *m,unsigned reg,uint32_t mask,uint32_t value) {
    m->write(m->ctx,reg,(m->read(m->ctx,reg)&~mask)|(value&mask));
}
static int lh_switch(struct lancelot_msdc *m,unsigned index,unsigned value) {
    return lm_command(m,6u|(7u<<7),(3u<<24)|(index<<16)|(value<<8));
}
static int lh_reset_failed_point(struct lancelot_msdc *m) {
    if(m->recovery_status==LM_OK)return LM_OK;
    /* A deliberately bad sampling point can leave the host data engine
     * busy after STOP fails. Reset the host before trying the next tap;
     * never let that stale state invalidate every remaining sample. */
    m->write(m->ctx,LM_CFG,m->read(m->ctx,LM_CFG)|4u);
    if(lm_wait_clear(m,LM_CFG,4u)!=LM_OK)return LM_IO;
    m->write(m->ctx,LM_FIFO,1u<<31);
    if(lm_wait_clear(m,LM_FIFO,1u<<31)!=LM_OK)return LM_IO;
    m->write(m->ctx,LM_INT,LM_EVENTS);
    return LM_OK;
}
/* JEDEC 8-bit CMD21 tuning block, independent of on-disk contents. */
static const uint8_t lh_pattern[128]={
    0xff,0xff,0x00,0xff,0xff,0xff,0x00,0x00,0xff,0xff,0xcc,0xcc,0xcc,0x33,0xcc,0xcc,
    0xcc,0x33,0x33,0xcc,0xcc,0xcc,0xff,0xff,0xff,0xee,0xff,0xff,0xff,0xee,0xee,0xff,
    0xff,0xff,0xdd,0xff,0xff,0xff,0xdd,0xdd,0xff,0xff,0xff,0xbb,0xff,0xff,0xff,0xbb,
    0xbb,0xff,0xff,0xff,0x77,0xff,0xff,0xff,0x77,0x77,0xff,0x77,0xbb,0xdd,0xee,0xff,
    0xff,0xff,0xff,0x00,0xff,0xff,0xff,0x00,0x00,0xff,0xff,0xcc,0xcc,0xcc,0x33,0xcc,
    0xcc,0xcc,0x33,0x33,0xcc,0xcc,0xcc,0xff,0xff,0xff,0xee,0xff,0xff,0xff,0xee,0xee,
    0xff,0xff,0xff,0xdd,0xff,0xff,0xff,0xdd,0xdd,0xff,0xff,0xff,0xbb,0xff,0xff,0xff,
    0xbb,0xbb,0xff,0xff,0xff,0x77,0xff,0xff,0xff,0x77,0x77,0xff,0x77,0xbb,0xdd,0xee
};
static int lh_tuning_block(struct lancelot_msdc *m) {
    uint8_t bytes[128];
    if(lm_data_read(m,21u|(1u<<7)|(1u<<11)|(128u<<16),0,1,bytes,sizeof(bytes)))return 0;
    for(unsigned i=0;i<128;i++)if(bytes[i]!=lh_pattern[i])return 0;
    return 1;
}
static int lh_hs200_tune(struct lancelot_msdc *m,struct lancelot_hs400 *r) {
    unsigned best_length=0;
    for(unsigned edge=0;edge<2;edge++) {
        /* MT6768 data-tune + async FIFO uses the same CMD/DAT sweep. */
        lh_field(m,0x04,0x106,edge?0x106:0);
        uint32_t map=0;
        for(unsigned tap=0;tap<32;tap++) {
            lh_field(m,LH_TOP,0x1f80,tap<<7);
            lh_field(m,LH_TOP+4,0x3e0,tap<<5);
            bool valid=true;
            for(unsigned repeat=0;repeat<3;repeat++)if(!lh_tuning_block(m)){
                valid=false;if(lh_reset_failed_point(m)!=LM_OK)return LM_IO;break;
            }
            if(valid)map|=1u<<tap;
        }
        unsigned length,tap=lh_tap(map,&length);
        if(length>best_length){best_length=length;r->hs200_map=map;r->hs200_tap=tap;r->hs200_edge=edge;}
        if(length>=12)break;
    }
    if(best_length<4)return LM_IO;
    lh_field(m,0x04,0x106,r->hs200_edge?0x106:0);
    lh_field(m,LH_TOP,0x1f80,r->hs200_tap<<7);
    lh_field(m,LH_TOP+4,0x3e0,r->hs200_tap<<5);
    return lh_tuning_block(m)?LM_OK:LM_IO;
}
static bool lh_ext_matches(const uint8_t a[512],const uint8_t b[512]) {
    const unsigned fields[]={192,194,196,212,213,214,215,307,308};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)if(a[fields[i]]!=b[fields[i]])return false;
    return true;
}
static int lancelot_hs400_negotiate(struct lancelot_msdc *m,uint64_t source,
    uint64_t ticks_per_second,int (*verify)(void *),void *ctx,struct lancelot_hs400 *r) {
    uint8_t ext[512],check[512];
    *r=(struct lancelot_hs400){0};r->stage=1;
    if(!verify || lm_width(m->read(m->ctx,LM_BUS_CFG))!=8 || source!=384000000 ||
       !ticks_per_second || lancelot_msdc_ext_csd(m,ext)!=LM_OK || (ext[196]&0x50)!=0x50 ||
       !(ext[308]&1) || (ext[307]&31)<7)return LM_CONFIG;
    r->depth=(ext[307]&31)+1;
    uint32_t low_cfg=m->read(m->ctx,LM_CFG),low_iocon=m->read(m->ctx,4);
    uint32_t low_top=m->read(m->ctx,LH_TOP),low_cmd=m->read(m->ctx,LH_TOP+4);
    if(((low_cfg>>20)&3)!=0 || lm_clock_hz(low_cfg,source)>52000000)return LM_CONFIG;
    uint32_t high_cfg=low_cfg&~((3u<<20)|(0xfffu<<8)|(1u<<22));
    uint64_t timeout=m->timeout;
    m->bus_mode_change_enabled=true;
    int result=LM_IO;
    r->stage=2;
    if(lh_switch(m,185,2)!=LM_OK || lm_program_clock(m,high_cfg)!=LM_OK)goto done;
    /* MT6768 TOP base and reset DS value match the live lancelot DT. */
    m->write(m->ctx,LH_TOP+0x0c,0x12814);
    lh_field(m,0xb8,1u<<28,0);
    lh_field(m,0x220,0x1f,2);
    lh_field(m,0xb0,7u<<7,0); /* Android DT has no latch-ck override */
    r->stage=3;
    m->timeout=ticks_per_second/100; /* bound each failed tuning point to 10 ms */
    if(lh_hs200_tune(m,r)!=LM_OK)goto done;
    m->timeout=timeout;
    /* HS400 data is sampled from DS; retain tuned CMD while resetting the
     * HS200 DAT tap/edge, as MT6768's prepare-HS400 path requires. */
    uint32_t high_iocon=m->read(m->ctx,4)&~0x104u;
    uint32_t high_top=m->read(m->ctx,LH_TOP)&~0x1f80u,high_cmd=m->read(m->ctx,LH_TOP+4);
    r->stage=4;
    if(lh_switch(m,185,1)!=LM_OK || lm_program_clock(m,low_cfg)!=LM_OK)goto done;
    m->write(m->ctx,4,low_iocon);m->write(m->ctx,LH_TOP,low_top);m->write(m->ctx,LH_TOP+4,low_cmd);
    if(lm_card_ready(m)!=LM_OK || lh_switch(m,183,6)!=LM_OK || lm_card_ready(m)!=LM_OK)goto done;
    r->stage=5;
    if(lh_switch(m,185,3)!=LM_OK || lm_program_clock(m,high_cfg|(3u<<20)|(1u<<22))!=LM_OK)goto done;
    m->write(m->ctx,4,high_iocon);m->write(m->ctx,LH_TOP,high_top);m->write(m->ctx,LH_TOP+4,high_cmd);
    r->stage=6;
    uint32_t map=0;
    m->timeout=ticks_per_second/100;
    for(unsigned tap=0;tap<32;tap++) {
        lh_field(m,LH_TOP+0x0c,(1u<<16)|(31u<<10),(1u<<16)|(tap<<10));
        bool valid=true;
        for(unsigned repeat=0;repeat<3;repeat++) {
            if(lancelot_msdc_ext_csd(m,check)!=LM_OK || !lh_ext_matches(ext,check)){
                valid=false;if(lh_reset_failed_point(m)!=LM_OK)goto done;break;
            }
        }
        if(valid)map|=1u<<tap;
    }
    unsigned length;
    r->hs400_map=map;r->hs400_tap=lh_tap(map,&length);
    if(length<4)goto done;
    lh_field(m,LH_TOP+0x0c,31u<<10,r->hs400_tap<<10);
    m->timeout=timeout;
    r->stage=7;
    if(lm_card_ready(m)!=LM_OK || !verify(ctx))goto done;
    r->clock_hz=source/2;result=LM_OK;r->stage=8;
done:
    m->timeout=timeout;m->bus_mode_change_enabled=false;
    return result;
}
#endif
