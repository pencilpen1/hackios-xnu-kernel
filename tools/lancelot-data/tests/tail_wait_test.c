/* Actual transport with delayed hardware completion. Time is a deterministic
 * register model in microseconds, not a claim about device performance. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_msdc.h"
enum mode { NORMAL, DELAYED_TAIL, LATE_CRC, TAIL_TIMEOUT, PREPARE_BUSY, FIFO_STALL, SHORT_READ, COMMAND_TIMEOUT };
struct model {
    enum mode mode;
    uint32_t cfg, interrupts, argument, command, response, phase, count, pending_count;
    unsigned position, total, irq, initial_irq, depth, entries, data_commands, polls, waits, masked_waits;
    uint64_t now, masked_at, max_mask, payload_done;
    uint8_t payload[4096], disk[4096];
};
static struct model *active;
static bool is_read(unsigned command) {return command==8 || command==17 || command==18;}
static uint32_t read_reg(void *ctx, uint32_t reg) {
    struct model *m=ctx;
    switch(reg) {
    case LM_CFG:return 9;
    case LM_INTEN:return m->interrupts;
    case LM_STATUS:return m->mode==PREPARE_BUSY && m->now<32000 ? 3 : 0;
    case LM_PS:return LM_DAT0;
    case LM_FIFO:
        if(m->phase!=2 || !is_read(m->command))return 0;
        if(m->mode==FIFO_STALL && m->position>=128)return 0;
        if(m->mode==SHORT_READ && m->position>=128)return 0;
        return m->position<m->total ? (m->total-m->position<128?m->total-m->position:128) : 0;
    case LM_RESPONSE:return m->response;
    case LM_INT:
        m->polls++;
        if(m->phase==1)return m->mode==COMMAND_TIMEOUT && (is_read(m->command)||m->command==24)?0:LM_CMD_READY;
        if(m->phase!=2)return 0;
        if(m->mode==SHORT_READ && is_read(m->command) && m->position>=128)return LM_DATA_DONE;
        if(m->position<m->total)return 0;
        if(m->mode==TAIL_TIMEOUT || m->mode==FIFO_STALL)return 0;
        if((m->mode==DELAYED_TAIL || m->mode==LATE_CRC) && m->now-m->payload_done<32000)return 0;
        return m->mode==LATE_CRC ? (1u<<15) : LM_DATA_DONE;
    case LM_RX: {
        assert(m->depth==1 && !m->irq && is_read(m->command) && m->position+4<=m->total);
        uint32_t value;memcpy(&value,m->payload+m->position,4);m->position+=4;m->now++;
        if(m->position==m->total)m->payload_done=m->now;
        return value;
    }
    default:assert(!"unexpected register read");return 0;
    }
}
static void write_reg(void *ctx, uint32_t reg, uint32_t value) {
    struct model *m=ctx;
    switch(reg) {
    case LM_INTEN:m->interrupts=value;return;
    case LM_FIFO:assert(value==(1u<<31));return;
    case LM_INT:
        if(value==LM_CMD_READY && m->phase==1)m->phase=(is_read(m->command)||m->command==24)?2:0;
        return;
    case LM_COUNT:m->count=value;return;
    case LM_ARG:m->argument=value;return;
    case LM_CMD:
        m->command=value&63;m->phase=1;m->response=0x900;
        if(m->command==23){m->pending_count=m->argument;return;}
        if(is_read(m->command) || m->command==24) {
#ifdef BEFORE_TAIL
            assert(m->depth==1 && !m->irq);
#else
            assert(m->command==24 ? (m->depth==1 && !m->irq) : (!m->depth && m->irq==m->initial_irq));
#endif
            if(m->command==18)assert(m->pending_count==m->count);
            if(m->command==24)assert(m->count==1);
            m->position=0;m->total=m->count*512;assert(m->total<=4096);m->data_commands++;
            return;
        }
        assert(m->command==12 || m->command==13 || m->command==6);
        return;
    case LM_TX:
        assert(m->depth==1 && !m->irq && m->command==24 && m->position+4<=m->total);
        memcpy(m->disk+m->position,&value,4);m->position+=4;m->now++;
        if(m->position==m->total)m->payload_done=m->now;
        return;
    default:assert(!"unexpected register write");
    }
}
static uint64_t ticks(void *ctx){struct model *m=ctx;assert(m->now<1000000);return ++m->now;}
static uint64_t enter(void *ctx){struct model *m=ctx;assert(!m->depth);m->depth=1;m->entries++;m->masked_at=m->now;unsigned prior=m->irq;m->irq=0;return prior;}
static void leave(void *ctx,uint64_t state){struct model *m=ctx;assert(m->depth==1&&!m->irq);m->depth=0;if(m->now-m->masked_at>m->max_mask)m->max_mask=m->now-m->masked_at;m->irq=(unsigned)state;}
static void idle_wait(void *ctx){struct model *m=ctx;if(m->irq){m->waits++;m->now+=100;}else m->masked_waits++;}
#ifndef BEFORE_TAIL
#define __unused __attribute__((unused))
#define os_atomic_add(p,v,o) __atomic_fetch_add(p,v,__ATOMIC_RELAXED)
#define os_atomic_load(p,o) __atomic_load_n(p,__ATOMIC_RELAXED)
#define os_atomic_store(p,v,o) __atomic_store_n(p,v,__ATOMIC_RELAXED)
static unsigned mmc_current_phase;
static uint64_t mmc_phase_began,mmc_phase_total[LM_PHASE_COUNT],mmc_phase_max[LM_PHASE_COUNT];
static uint64_t mmc_ticks(void *ctx){(void)ctx;return active->now;}
#include "actual-mmc-phase.inc"
#endif
static void run_case(enum mode mode, bool write, unsigned count, unsigned initial_irq) {
    struct model m={.mode=mode,.interrupts=0xa5a5ffff,.irq=initial_irq,.initial_irq=initial_irq};active=&m;
    for(unsigned i=0;i<4096;i++)m.payload[i]=(uint8_t)(i*37+13);
    struct lancelot_msdc c={.ctx=&m,.read=read_reg,.write=write_reg,.ticks=ticks,.timeout=50000,
        .critical_enter=enter,.critical_exit=leave,.idle_wait=idle_wait,.rca=1,
        .media_write_enabled=write,.media_write_first=128,.media_write_end=129};
#ifndef BEFORE_TAIL
    mmc_current_phase=0;mmc_phase_began=0;
    memset(mmc_phase_total,0,sizeof(mmc_phase_total));memset(mmc_phase_max,0,sizeof(mmc_phase_max));
    c.phase=mmc_phase;
    c.read_poll_ticks=50;
#endif
    uint8_t output[4112];memset(output,0xcd,sizeof(output));
    int result=write?lancelot_msdc_write_sector(&c,128,m.payload):lancelot_msdc_read(&c,128,count,output+8,count*512);
    bool success=mode==NORMAL || mode==DELAYED_TAIL || mode==PREPARE_BUSY;
    if((result==LM_OK)!=success)fprintf(stderr,"mode=%u write=%u count=%u irq=%u result=%d\n",mode,write,count,initial_irq,result);
    assert((result==LM_OK)==success);
    assert(m.irq==initial_irq&&!m.depth&&m.interrupts==0xa5a5ffff&&m.data_commands==1);
    if(success)assert(!memcmp(m.payload,write?m.disk:output+8,count*512));
    if(!write)for(unsigned i=0;i<8;i++)assert(output[i]==0xcd&&output[8+count*512+i]==0xcd);
    if(!success)assert(c.recoveries==1&&c.recovery_status==LM_OK);
    if(mode==DELAYED_TAIL || mode==LATE_CRC || mode==TAIL_TIMEOUT || mode==PREPARE_BUSY) {
#ifdef BEFORE_TAIL
        assert(m.max_mask>=32000 && m.waits==0);
#else
        assert(m.max_mask<2000);
        if(initial_irq)assert(m.waits>=300 && m.polls<1000);
        else assert(!m.waits);
#endif
    }
    if(mode==FIFO_STALL || mode==COMMAND_TIMEOUT) {
#ifdef BEFORE_TAIL
        assert(!m.waits&&m.max_mask>=49000);
#else
        if(write)assert(!m.waits&&m.max_mask>=49000);
        else {assert(m.max_mask<100);assert(initial_irq ? m.waits>0 : m.waits==0);}
#endif
    }
#ifndef BEFORE_TAIL
    assert(mmc_current_phase==LM_PHASE_IDLE);
    /* A few modeled ticks pass between the final word and phase boundary. */
    if(mode==DELAYED_TAIL)assert(mmc_phase_total[LM_PHASE_TAIL]>=31900 && mmc_phase_total[LM_PHASE_FIFO]<2000);
    if(mode==PREPARE_BUSY)assert(mmc_phase_total[LM_PHASE_PREPARE]>=32000);
#endif
    if(mode==DELAYED_TAIL && initial_irq)printf("%s count=%u: modeled IRQ mask=%llu us, completion polls=%u, waits=%u\n",
        write?"write":"read",count,(unsigned long long)m.max_mask,m.polls,m.waits);
}
int main(void) {
    for(unsigned irq=0;irq<2;irq++)for(unsigned mode=NORMAL;mode<=COMMAND_TIMEOUT;mode++) {
        run_case((enum mode)mode,false,1,irq);run_case((enum mode)mode,false,8,irq);
        if(mode!=FIFO_STALL&&mode!=SHORT_READ)run_case((enum mode)mode,true,1,irq);
    }
    puts("PASS: exact payload, delayed/late-error completion, timeout, partial FIFO, prepare busy, caller IRQ preservation and recovery");
}
