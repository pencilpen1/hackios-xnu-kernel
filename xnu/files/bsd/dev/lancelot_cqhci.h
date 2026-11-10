/* CQHCI 5.x transport for MT6768. Controller ownership is external; CPU IRQs
 * stay enabled. The fixed DMA arena is retained permanently after an error
 * so even an unresponsive engine cannot DMA into subsequently reused memory. */
#ifndef LANCELOT_CQHCI_H
#define LANCELOT_CQHCI_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define LC_BASE 0x800u
#define LC_CFG 0x08u
#define LC_CTL 0x0cu
#define LC_IS 0x10u
#define LC_ISTE 0x14u
#define LC_ISGE 0x18u
#define LC_TDBR 0x28u
#define LC_TCN 0x2cu
#define LC_MAX_BYTES 524288u
#define LC_ARENA_BYTES (4096u+LC_MAX_BYTES)
#define LC_SLOTS 8u
struct lancelot_cqhci {
    void *ctx;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    uint64_t (*ticks)(void *);
    void (*wait)(void *);
    /* 0: clean before DMA read, 1: clean/invalidate before DMA write,
     * 2: invalidate after DMA write. All ranges stay in the private arena. */
    void (*sync)(void *,size_t,size_t,unsigned);
    uint8_t *arena;
    uint64_t physical,read_timeout,write_timeout,poll_ticks;
    bool enabled,failed;
    uint32_t last_is,last_terri,last_host_int,completed,active_mask;
    uint64_t requests,reads,writes,flushes,bytes,waits,total_ticks,max_ticks;
};
static inline void lc_put32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static inline uint32_t lc_rd(struct lancelot_cqhci *q,unsigned r){return q->read(q->ctx,LC_BASE+r);}
static inline void lc_wr(struct lancelot_cqhci *q,unsigned r,uint32_t v){q->write(q->ctx,LC_BASE+r,v);}
static inline void lc_zero(uint8_t *p,size_t n){for(size_t i=0;i<n;i++)p[i]=0;}
static inline void lc_copy(uint8_t *d,const uint8_t *s,size_t n){for(size_t i=0;i<n;i++)d[i]=s[i];}
static int lc_poison(struct lancelot_cqhci *q,int error) {
    q->failed=true;
    q->last_is=lc_rd(q,LC_IS);q->last_terri=lc_rd(q,0x54);
    q->last_host_int=q->read(q->ctx,0x0c);
    lc_wr(q,LC_ISGE,0);lc_wr(q,LC_CTL,1); /* halt, do not clear live descriptors */
    uint64_t start=q->ticks(q->ctx);
    while(!(lc_rd(q,LC_IS)&1) && !(lc_rd(q,LC_CTL)&1)) {
        if(q->ticks(q->ctx)-start>=q->read_timeout)break;
        if(q->wait)q->wait(q->ctx);
    }
    return error;
}
static int lc_enable(struct lancelot_cqhci *q,uint32_t rca,unsigned depth) {
    if(!q->arena || !q->physical || (q->physical&1023) || q->physical>=(1ULL<<36) ||
       LC_ARENA_BYTES>(1ULL<<36)-q->physical || depth<LC_SLOTS || depth>32 ||
       !q->read_timeout || !q->write_timeout || !q->sync || !rca || rca>65535)return -3;
    if((lc_rd(q,0)&0xfff)!=0x510 || (lc_rd(q,LC_CFG)&1) || lc_rd(q,LC_TDBR))return -3;
    lc_zero(q->arena,4096);
    for(unsigned i=0;i<LC_SLOTS;i++) {
        uint8_t *link=q->arena+32*i+16;
        uint64_t trans=q->physical+1024+16*i;
        lc_put32(link,1u|(6u<<3));lc_put32(link+4,(uint32_t)trans);lc_put32(link+8,(uint32_t)(trans>>32));
    }
    q->sync(q->ctx,0,4096,0);
    lc_wr(q,LC_CFG,0x1100); /* 128-bit task descriptors, direct commands */
    lc_wr(q,0x20,(uint32_t)q->physical);lc_wr(q,0x24,(uint32_t)(q->physical>>32));
    lc_wr(q,0x40,0x40);lc_wr(q,0x44,rca);
    lc_wr(q,0x50,0xfff9a080u); /* report card R1 errors, including switch rejection */
    lc_wr(q,0x1c,0);lc_wr(q,LC_ISGE,0);lc_wr(q,LC_ISTE,0);
    lc_wr(q,LC_TCN,0xffffffff);lc_wr(q,LC_IS,0xf);
    lc_wr(q,LC_CFG,0x1101);lc_wr(q,LC_CTL,0);
    /* Status generation remains enabled for polling; no unhandled CQE IRQ. */
    lc_wr(q,LC_ISTE,0xf);
    q->enabled=(lc_rd(q,LC_CFG)&0x1101)==0x1101 && !(lc_rd(q,LC_CTL)&1);
    return q->enabled?0:lc_poison(q,-2);
}
static int lc_wait(struct lancelot_cqhci *q,uint32_t mask,uint64_t timeout) {
    uint64_t start=q->ticks(q->ctx),last=start;
    q->completed=0;q->active_mask=mask;
    for(;;) {
        uint32_t status=lc_rd(q,LC_IS),host=q->read(q->ctx,0x0c);
        if((status&4) || (host&((3u<<9)|(3u<<14)|(7u<<17))))return lc_poison(q,-2);
        uint32_t done=lc_rd(q,LC_TCN);
        if(done&~mask)return lc_poison(q,-2);
        if(done){q->completed|=done;lc_wr(q,LC_TCN,done);}
        if((q->completed&mask)==mask && !(lc_rd(q,LC_TDBR)&mask)) {
            if((lc_rd(q,LC_IS)&4) || (q->read(q->ctx,0x0c)&((3u<<9)|(3u<<14)|(7u<<17))))return lc_poison(q,-2);
            lc_wr(q,LC_IS,2);
            uint64_t elapsed=q->ticks(q->ctx)-start;
            q->total_ticks+=elapsed;if(elapsed>q->max_ticks)q->max_ticks=elapsed;
            q->active_mask=0;return 0;
        }
        uint64_t now=q->ticks(q->ctx);
        if(now-start>=timeout)return lc_poison(q,-1);
        if(q->wait && now-last>=q->poll_ticks){q->waits++;q->wait(q->ctx);last=q->ticks(q->ctx);}
    }
}
static bool lc_idle(struct lancelot_cqhci *q) {
    return !lc_rd(q,LC_TDBR) && !lc_rd(q,LC_TCN) && !(lc_rd(q,LC_CTL)&1) &&
        !(lc_rd(q,LC_IS)&4) && !(q->read(q->ctx,0x0c)&((3u<<9)|(3u<<14)|(7u<<17)));
}
static int lc_transfer(struct lancelot_cqhci *q,uint32_t sector,uint32_t blocks,
    uint8_t *data,bool write) {
    if(!q->enabled||q->failed)return -2;
    if(!data || !blocks || blocks>1024 || (uint64_t)sector+blocks>(1ULL<<32))return -3;
    if(!lc_idle(q))return lc_poison(q,-2);
    size_t bytes=(size_t)blocks*512;
    if(write)lc_copy(q->arena+4096,data,bytes);
    q->sync(q->ctx,4096,bytes,write?0:1);
    unsigned slots=(blocks+127)/128;
    for(unsigned i=0;i<slots;i++) {
        unsigned count=blocks-i*128;if(count>128)count=128;
        uint8_t *task=q->arena+32*i,*trans=q->arena+1024+16*i;
        lc_zero(task,16);lc_zero(trans,16);
        lc_put32(task,0x2fu|(!write?1u<<12:0)|(count<<16));
        lc_put32(task+4,sector+i*128);
        uint64_t address=q->physical+4096+(uint64_t)i*65536;
        lc_put32(trans,0x23u|((count*512u&0xffff)<<16));
        lc_put32(trans+4,(uint32_t)address);lc_put32(trans+8,(uint32_t)(address>>32));
    }
    q->sync(q->ctx,0,4096,0);
    uint32_t mask=(1u<<slots)-1;
    lc_wr(q,LC_IS,0xf);lc_wr(q,LC_TDBR,mask);q->requests++;
    int result=lc_wait(q,mask,write?q->write_timeout:q->read_timeout);
    if(result)return result;
    if(!write){q->sync(q->ctx,4096,bytes,2);lc_copy(data,q->arena+4096,bytes);q->reads++;}
    else q->writes++;
    q->bytes+=bytes;return 0;
}
static int lc_flush(struct lancelot_cqhci *q) {
    if(!q->enabled||q->failed)return -2;
    if(!lc_idle(q))return lc_poison(q,-2);
    uint8_t *task=q->arena+31*32;lc_zero(task,32);
    lc_put32(task,0x2fu|(1u<<14)|(6u<<16)|(3u<<23));
    lc_put32(task+4,0x03200100u); /* FLUSH_CACHE, no partition change */
    q->sync(q->ctx,0,4096,0);
    lc_wr(q,LC_IS,0xf);lc_wr(q,LC_TDBR,1u<<31);q->requests++;
    int result=lc_wait(q,1u<<31,q->write_timeout);
    if(!result && (lc_rd(q,0x48)&0xfff9a080u))result=lc_poison(q,-2);
    if(!result)q->flushes++;
    return result;
}
#endif
