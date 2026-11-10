/* Time-driven 128-byte receive FIFO with backpressure and independent CPU
 * scheduling. This verifies transport behavior, not physical device timings. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_msdc.h"
enum mode { STREAM, FIRST_DELAY, MID_DELAY, STUCK, BAD_CRC, LATE_CRC, SHORT,
    COMMAND_STUCK, DONE_BEFORE_DRAIN, OVERFLOW_COUNT };
struct model {
    enum mode mode;
    uint64_t now, command_at, gap_until, production_at, entered, max_mask;
    unsigned irq, initial_irq, depth, phase, command, total, produced, consumed;
    unsigned fifo_bytes, count, argument, inten, response, pending_count;
    unsigned irq_services, waits, pauses, commands, samples, max_burst;
    bool gap_used, injected;
    uint8_t bytes[65536];
#ifndef BEFORE_FIFO
    struct lm_read_sample sample;
#endif
};
static void produce(struct model *m) {
    if(m->phase!=2)return;
    if(m->mode==FIRST_DELAY && m->now-m->command_at<15600)return;
    if(m->mode==MID_DELAY && m->produced>=256 && !m->gap_used) {
        m->gap_used=true;m->gap_until=m->now+15600;
    }
    if(m->now<m->gap_until)return;
    if((m->mode==STUCK || m->mode==SHORT) && m->produced>=256)return;
    while(m->fifo_bytes+4<=128 && m->produced<m->total && m->now>=m->production_at+1) {
        m->fifo_bytes+=4;m->produced+=4;m->production_at++;
    }
    if(m->fifo_bytes==128) {m->pauses++;m->production_at=m->now;}
}
static void advance(struct model *m, unsigned us) {m->now+=us;assert(m->now<1000000);produce(m);}
static uint32_t rd(void *ctx,uint32_t reg) {
    struct model *m=ctx;advance(m,1);
    switch(reg) {
    case LM_CFG:return 9;
    case LM_STATUS:return 0;
    case LM_PS:return LM_DAT0;
    case LM_INTEN:return m->inten;
    case LM_RESPONSE:return m->response;
    case LM_FIFO:
        if(m->phase!=2)return 0;
        return m->mode==OVERFLOW_COUNT ? 132 : m->fifo_bytes;
    case LM_INT:
        if(m->phase==1)return m->mode==COMMAND_STUCK && m->command!=12 && m->command!=13 && m->command!=23 ? 0 : LM_CMD_READY;
        if(m->phase!=2)return 0;
        if(m->mode==BAD_CRC && m->consumed>=128)return 1u<<15;
        if(m->mode==LATE_CRC && m->consumed==m->total)return LM_DATA_DONE|(1u<<15);
        if(m->mode==SHORT && m->produced==256)return LM_DATA_DONE;
        if(m->mode==DONE_BEFORE_DRAIN && m->consumed>=128)return LM_DATA_DONE;
        return m->produced==m->total ? LM_DATA_DONE : 0;
    case LM_RX: {
        assert(m->depth==1 && !m->irq && m->fifo_bytes>=4 && m->consumed+4<=m->total);
        uint32_t value;memcpy(&value,m->bytes+m->consumed,4);
        m->consumed+=4;m->fifo_bytes-=4;return value;
    }
    default:assert(0);return 0;
    }
}
static void wr(void *ctx,uint32_t reg,uint32_t value) {
    struct model *m=ctx;advance(m,1);
    switch(reg) {
    case LM_INTEN:m->inten=value;return;
    case LM_FIFO:assert(value==1u<<31);m->fifo_bytes=0;return;
    case LM_COUNT:m->count=value;return;
    case LM_ARG:m->argument=value;return;
    case LM_INT:
        if(value==LM_CMD_READY && m->phase==1) {
            m->phase=(m->command==8||m->command==17||m->command==18)?2:0;
            m->production_at=m->now;
        }
        return;
    case LM_CMD:
        assert(!(m->inten&LM_EVENTS));
        m->command=value&63;m->response=0x900;m->phase=1;
        if(m->command==23){m->pending_count=m->argument;return;}
        if(m->command==12||m->command==13)return;
        assert(m->command==8||m->command==17||m->command==18);
#ifdef BEFORE_FIFO
        assert(m->depth==1 && !m->irq);
#else
        assert(!m->depth && m->irq==m->initial_irq);
#endif
        if(m->command==18)assert(m->pending_count==m->count);
        m->commands++;m->total=m->count*512;m->command_at=m->now;return;
    default:assert(0);
    }
}
static uint64_t ticks(void *ctx){struct model *m=ctx;advance(m,1);return m->now;}
static uint64_t enter(void *ctx){struct model *m=ctx;assert(!m->depth);m->depth=1;m->entered=m->now;unsigned prior=m->irq;m->irq=0;return prior;}
static void leave(void *ctx,uint64_t state) {
    struct model *m=ctx;assert(m->depth==1&&!m->irq);
    uint64_t elapsed=m->now-m->entered;if(elapsed>m->max_mask)m->max_mask=elapsed;
    m->depth=0;m->irq=(unsigned)state;
    if(m->irq) {
        m->irq_services++;
        /* Simulate a timer interrupt and occasional preemption while the
         * hardware fills/stalls its FIFO. No other thread owns the controller. */
        advance(m,m->irq_services%11==0 ? 300 : 2);
    }
}
static void wait_idle(void *ctx){struct model *m=ctx;if(m->irq){assert(!m->depth);m->waits++;m->irq_services++;advance(m,100);}}
static void burst(void *ctx,uint8_t *out,unsigned words) {
    struct model *m=ctx;assert(words&&words<=32);if(words*4>m->max_burst)m->max_burst=words*4;
    for(unsigned i=0;i<words;i++){uint32_t word=rd(ctx,LM_RX);memcpy(out+4*i,&word,4);}
}
#ifndef BEFORE_FIFO
static void sample(void *ctx,const struct lm_read_sample *s){struct model *m=ctx;assert(!m->depth);m->sample=*s;m->samples++;}
#endif
static void run(enum mode mode,unsigned count,unsigned initial_irq,unsigned adapter,unsigned align) {
    struct model m={.mode=mode,.irq=initial_irq,.initial_irq=initial_irq,.inten=0xa5a5ffff};
    for(unsigned i=0;i<sizeof(m.bytes);i++)m.bytes[i]=(uint8_t)(i*37+i/255);
    struct lancelot_msdc c={.ctx=&m,.read=rd,.write=wr,.ticks=ticks,.timeout=100000,
        .critical_enter=enter,.critical_exit=leave,.idle_wait=wait_idle,.rca=1};
    if(adapter)c.read_fifo=burst;
#ifndef BEFORE_FIFO
    c.read_poll_ticks=50;c.read_sample=sample;
#endif
    uint8_t out[65552];memset(out,0xa5,sizeof(out));
    int status=lancelot_msdc_read(&c,1024,count,out+align,count*512);
    bool ok=mode==STREAM||mode==FIRST_DELAY||mode==MID_DELAY||mode==DONE_BEFORE_DRAIN;
#ifdef BEFORE_FIFO
    if(mode==DONE_BEFORE_DRAIN)ok=false;
    if(mode==LATE_CRC)ok=true; /* E2 returns using its pre-drain status. */
#endif
    if((status==LM_OK)!=ok)fprintf(stderr,"mode=%u count=%u irq=%u adapter=%u align=%u status=%d produced=%u consumed=%u now=%llu\n",mode,count,initial_irq,adapter,align,status,m.produced,m.consumed,(unsigned long long)m.now);
    assert((status==LM_OK)==ok);
    assert(!m.depth&&m.irq==initial_irq&&m.inten==0xa5a5ffff&&m.commands==1);
    assert(c.recoveries==(ok?0u:1u));
    if(ok)assert(!memcmp(out+align,m.bytes,count*512));
    for(unsigned i=0;i<align;i++)assert(out[i]==0xa5);
    for(unsigned i=align+count*512;i<sizeof(out);i++)assert(out[i]==0xa5);
    assert(initial_irq||!m.waits);
#ifdef BEFORE_FIFO
    if(mode==FIRST_DELAY||mode==MID_DELAY||mode==STUCK)assert(m.max_mask>=15600);
#else
    assert(m.max_mask<64 && (!adapter||m.max_burst<=128));
    assert(m.samples==1 && m.sample.received==m.consumed && m.sample.status==status);
    if(mode==FIRST_DELAY||mode==MID_DELAY||mode==STUCK) {
        assert(m.sample.empty_ticks>=15000&&m.sample.gap_max_ticks>=15000);
        assert(initial_irq ? m.waits>0 : m.waits==0);
        assert(m.sample.copy_ticks<m.total/2);
    }
    /* FIFO phase starts just after command readiness, not at command issue. */
    if(mode==FIRST_DELAY)assert(m.sample.first_ticks>=15550);
#endif
    if(mode==FIRST_DELAY&&count==16&&initial_irq&&adapter&&align==0)
        printf("delayed first data: IRQ max=%llu us, IRQ service=%u, empty waits=%u; model only\n",(unsigned long long)m.max_mask,m.irq_services,m.waits);
}
int main(void) {
    unsigned counts[]={1,8,16,128};
    for(unsigned i=0;i<4;i++)for(unsigned irq=0;irq<2;irq++)
    for(unsigned adapter=0;adapter<2;adapter++)for(unsigned align=0;align<2;align++)
    for(unsigned mode=STREAM;mode<=OVERFLOW_COUNT;mode++)run((enum mode)mode,counts[i],irq,adapter,align);
    puts("PASS: delayed first/middle data, backpressure, IRQ service, caller state, counted 512..65536-byte reads, completion-before-drain, CRC, timeout, short/overflow FIFO, exact bytes and guard bounds");
}
