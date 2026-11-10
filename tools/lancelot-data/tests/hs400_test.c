#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mmc_hs400.h"
enum { GOOD, FALLING, NO_WINDOW, BAD_DS, SWITCH_FAIL, BAD_CAPS, FAILED_STOP };
struct model {
    uint32_t reg[0x1100/4],cmd,arg,phase,response,count,position,total,timing,width,mode;
    uint32_t switches[8],nswitch,tuning_reads,ds_reads,verified,depth,irq,resets;
    uint64_t now;
    uint8_t ext[512];
};
static bool data_cmd(unsigned cmd){return cmd==8||cmd==17||cmd==18||cmd==21;}
static bool bad_data(struct model *m) {
    if(m->timing==2) {
        unsigned tap=(m->reg[(LH_TOP+4)/4]>>5)&31,edge=(m->reg[1]>>1)&1;
        unsigned data=(m->reg[LH_TOP/4]>>7)&31;
        return m->mode==NO_WINDOW || tap!=data || tap<1 || tap>30 || edge!=(m->mode==FALLING);
    }
    if(m->timing==3) {
        unsigned tap=(m->reg[(LH_TOP+12)/4]>>10)&31;
        return m->mode==BAD_DS || tap<2 || tap>25;
    }
    return false;
}
static uint32_t rd(void *ctx,uint32_t r) {
    struct model*m=ctx;m->now++;
    switch(r) {
    case LM_CFG:return m->reg[0]|LM_CLOCK_STABLE;
    case LM_STATUS:return 0;
    case LM_PS:return LM_DAT0;
    case LM_RESPONSE:return m->response;
    case LM_FIFO:return m->phase==2 && m->position<m->total ? (m->total-m->position>128?128:m->total-m->position):0;
    case LM_INT:
        if(m->phase==1)return m->mode==FAILED_STOP && (m->cmd==12||m->cmd==13) && bad_data(m) ? 1u<<10 : LM_CMD_READY;
        if(m->phase==2)return bad_data(m)?1u<<15:(m->position==m->total?LM_DATA_DONE:0);
        return 0;
    case LM_RX: {
        assert(m->depth==1&&!m->irq&&m->position+4<=m->total);
        const uint8_t *source=m->cmd==21?lh_pattern:m->ext;
        uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)source[m->position++]<<(8*i);return v;
    }
    default:assert(r<0x1100&&!(r&3));return m->reg[r/4];
    }
}
static void wr(void *ctx,uint32_t r,uint32_t v) {
    struct model*m=ctx;m->now++;
    switch(r) {
    case LM_CFG:
        if(v&4u){m->resets++;m->phase=0;v&=~4u;}
        m->reg[0]=v;return;
    case LM_FIFO:assert(v==1u<<31);return;
    case LM_INT:
        if(v==LM_CMD_READY&&m->phase==1)m->phase=data_cmd(m->cmd)?2:0;
        return;
    case LM_ARG:m->arg=v;return;
    case LM_COUNT:m->count=v;return;
    case LM_CMD:
        m->cmd=v&63;m->phase=1;m->response=0x900;
        if(data_cmd(m->cmd)) {
            m->position=0;m->total=m->count*((m->cmd==21)?128:512);assert(m->total<=512);
            if(m->cmd==21){assert(m->timing==2);m->tuning_reads++;}
            if(m->cmd==8&&m->timing==3)m->ds_reads++;
        } else if(m->cmd==6) {
            unsigned index=(m->arg>>16)&255,value=(m->arg>>8)&255;
            assert(m->nswitch<8);m->switches[m->nswitch++]=(index<<8)|value;
            if(index==185) {
                if(value==2)assert(m->timing==1&&m->width==2);
                if(value==1)assert(m->timing==2);
                if(value==3){assert(m->timing==1&&m->width==6);if(m->mode==SWITCH_FAIL){m->response|=0x80;return;}}
                m->timing=value;
            } else {assert(index==183&&value==6&&m->timing==1);m->width=value;}
        } else assert(m->cmd==12||m->cmd==13);
        return;
    default:assert(r<0x1100&&!(r&3));m->reg[r/4]=v;return;
    }
}
static uint64_t ticks(void *ctx){struct model*m=ctx;assert(m->now<10000000);return ++m->now;}
static uint64_t enter(void *ctx){struct model*m=ctx;assert(!m->depth);m->depth=1;unsigned s=m->irq;m->irq=0;return s;}
static void leave(void *ctx,uint64_t s){struct model*m=ctx;assert(m->depth==1);m->depth=0;m->irq=(unsigned)s;}
static int verify(void *ctx) {
    struct model*m=ctx;m->verified++;
    assert(m->timing==3&&m->width==6&&((m->reg[0]>>20)&7)==7);
    assert(!(m->reg[1]&0x104) && !(m->reg[LH_TOP/4]&0x1f80));
    return !bad_data(m);
}
static void run(unsigned mode) {
    struct model m={.timing=1,.width=2,.mode=mode,.irq=1};
    m.reg[0]=9|2|(2u<<8);m.reg[LM_BUS_CFG/4]=2u<<16;
    m.reg[LH_TOP/4]=1u<<13;m.reg[(LH_TOP+4)/4]=1u<<11;
    m.ext[192]=8;m.ext[194]=2;m.ext[196]=mode==BAD_CAPS?3:0x57;m.ext[307]=31;m.ext[308]=1;
    struct lancelot_msdc c={.ctx=&m,.read=rd,.write=wr,.ticks=ticks,.timeout=500000,
        .critical_enter=enter,.critical_exit=leave,.rca=1};
    struct lancelot_hs400 result;
    int status=lancelot_hs400_negotiate(&c,384000000,1000000,verify,&m,&result);
    bool ok=mode==GOOD||mode==FALLING||mode==FAILED_STOP;
    assert((status==0)==ok && !c.bus_mode_change_enabled && c.timeout==500000 && !m.depth && m.irq==1);
    if(ok) {
        assert(m.nswitch==4&&m.switches[0]==0xb902&&m.switches[1]==0xb901&&m.switches[2]==0xb706&&m.switches[3]==0xb903);
        assert(result.clock_hz==192000000&&result.depth==32&&result.stage==8&&m.verified==1);
        assert(result.hs200_edge==(mode==FALLING)&&result.hs200_tap==16&&result.hs400_tap==14);
        assert(m.tuning_reads>=90&&m.ds_reads>=72);
        if(mode==FAILED_STOP)assert(m.resets>0);
    } else assert(!m.verified);
    unsigned before=m.nswitch;
    assert(lh_switch(&c,185,3)==LM_CONFIG && lh_switch(&c,179,1)==LM_CONFIG && m.nswitch==before);
}
int main(void) {
    unsigned length;assert(lh_tap(0,&length)==32&&!length);
    assert(lh_tap(0xc0000003,&length)==0&&length==2); /* no false wrapping eye */
    for(unsigned mode=GOOD;mode<=FAILED_STOP;mode++)run(mode);
    puts("PASS actual HS400 negotiation: measured HS200/DS windows, both edges, narrow/missing window rejection, 192 MHz mode bits, SDR/DDR switch order, unsupported cards and mutation gate");
}
