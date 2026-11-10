#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lancelot_cqhci.h"
enum { NORMAL, DELAYED, TIMEOUT, CRC_ERROR, HOST_ERROR, LATE_ERROR, BAD_DCMD, STALE_ERROR };
struct model {
    uint32_t reg[0x900/4],pending,seen,mode,submitted,completed,flushes,halted;
    unsigned syncs,invalidates,waits;
    uint64_t now,issued,physical;
    uint8_t *cpu,*device,*disk;
};
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p){return get32(p)|(uint64_t)get32(p+4)<<32;}
static void progress(struct model *m) {
    if(!m->pending||m->halted)return;
    if(m->mode==TIMEOUT || (m->mode==DELAYED&&m->now-m->issued<15600))return;
    if(m->mode==CRC_ERROR){m->reg[(LC_BASE+LC_IS)/4]|=4;m->reg[(LC_BASE+0x54)/4]=0x80010000;return;}
    if(m->mode==HOST_ERROR){m->reg[0x0c/4]|=1u<<15;return;}
    unsigned slot=31;
    while(!(m->pending&(1u<<slot)))slot--;
    uint8_t *task=m->device+32*slot;
    uint32_t flags=get32(task);assert((flags&0x3f)==0x2f);
    if(slot==31) {
        assert((flags&(1u<<14)) && ((flags>>16)&63)==6 && ((flags>>23)&3)==3);
        assert(get64(task+4)==0x03200100 && !get32(task+16));
        m->flushes++;m->reg[(LC_BASE+0x48)/4]=m->mode==BAD_DCMD?0x80:0x900;
    } else {
        uint32_t blocks=flags>>16,sector=get32(task+4);
        assert(blocks && blocks<=128 && sector>=1024 && sector+blocks<=2048);
        const uint8_t *link=task+16;assert(get32(link)==0x31);
        uint64_t at=get64(link+4);assert(at==m->physical+1024+16*slot);
        const uint8_t *trans=m->device+(at-m->physical);
        uint32_t attr=get32(trans),bytes=attr>>16;assert((attr&0xffff)==0x23);
        if(!bytes)bytes=65536;
        assert(bytes==blocks*512);
        uint64_t phys=get64(trans+4);assert(phys==m->physical+4096+(uint64_t)slot*65536);
        size_t offset=(size_t)(phys-m->physical);assert(offset+bytes<=LC_ARENA_BYTES);
        if(flags&(1u<<12))memcpy(m->device+offset,m->disk+(sector-1024)*512,bytes);
        else memcpy(m->disk+(sector-1024)*512,m->device+offset,bytes);
    }
    m->pending&=~(1u<<slot);m->completed++;
    m->reg[(LC_BASE+LC_TDBR)/4]=m->pending;
    m->reg[(LC_BASE+LC_TCN)/4]|=1u<<slot;
    m->reg[(LC_BASE+LC_IS)/4]|=2;
    if(!m->pending&&m->mode==LATE_ERROR)m->reg[(LC_BASE+LC_IS)/4]|=4;
}
static uint32_t rd(void *ctx,uint32_t reg){struct model*m=ctx;assert(reg<0x900&&!(reg&3));m->now++;if(reg==LC_BASE+LC_IS)progress(m);return m->reg[reg/4];}
static void wr(void *ctx,uint32_t reg,uint32_t value) {
    struct model*m=ctx;assert(reg<0x900&&!(reg&3));m->now++;
    if(reg==LC_BASE+LC_IS || reg==LC_BASE+LC_TCN){m->reg[reg/4]&=~value;return;}
    if(reg==LC_BASE+LC_TDBR) {
        assert(!m->pending && m->reg[(LC_BASE+LC_CFG)/4]==0x1101);
        assert(m->reg[(LC_BASE+LC_ISGE)/4]==0 && m->reg[(LC_BASE+LC_ISTE)/4]==0xf);
        assert(value && !(value&~0x800000ff));m->pending=value;m->submitted++;m->issued=m->now;m->seen|=value;
    }
    if(reg==LC_BASE+LC_CTL && value==1){m->halted++;m->reg[(LC_BASE+LC_IS)/4]|=1;}
    m->reg[reg/4]=value;
}
static uint64_t ticks(void *ctx){struct model*m=ctx;return ++m->now;}
static void wait_cpu(void *ctx){struct model*m=ctx;m->waits++;m->now+=100;}
static void sync_dma(void *ctx,size_t offset,size_t bytes,unsigned direction) {
    struct model*m=ctx;assert(offset<=LC_ARENA_BYTES&&bytes<=LC_ARENA_BYTES-offset&&direction<=2);
    m->syncs++;
    if(direction<2)memcpy(m->device+offset,m->cpu+offset,bytes);
    else {memcpy(m->cpu+offset,m->device+offset,bytes);m->invalidates++;}
}
static void run(unsigned mode,unsigned blocks,bool write) {
    struct model m={.mode=mode,.physical=0x180000000ULL};
    m.cpu=calloc(1,LC_ARENA_BYTES);m.device=calloc(1,LC_ARENA_BYTES);m.disk=malloc(LC_MAX_BYTES);
    uint8_t *data=malloc(LC_MAX_BYTES+16);assert(m.cpu&&m.device&&m.disk&&data);
    for(unsigned i=0;i<LC_MAX_BYTES;i++)m.disk[i]=(uint8_t)(i*29+i/513);
    memset(data,0xa7,LC_MAX_BYTES+16);
    if(write)for(unsigned i=0;i<blocks*512;i++)data[8+i]=(uint8_t)(i*17+i/511);
    m.reg[LC_BASE/4]=0x510;
    struct lancelot_cqhci q={.ctx=&m,.read=rd,.write=wr,.ticks=ticks,.wait=wait_cpu,.sync=sync_dma,
        .arena=m.cpu,.physical=m.physical,.read_timeout=20000,.write_timeout=30000,.poll_ticks=50};
    assert(!lc_enable(&q,1,32));
    if(mode==STALE_ERROR)m.reg[(LC_BASE+LC_IS)/4]=4;
    int result=mode==BAD_DCMD?lc_flush(&q):lc_transfer(&q,1024,blocks,data+8,write);
    bool ok=mode==NORMAL||mode==DELAYED;
    assert((result==0)==ok);
    if(ok) {
        assert(!memcmp(data+8,m.disk,blocks*512));
        assert(m.completed==(blocks+127)/128 && !q.active_mask);
        assert(q.bytes==blocks*512 && q.requests==1 && q.failed==false);
        if(!write)assert(m.invalidates==1);
        assert(!lc_flush(&q)&&m.flushes==1&&q.flushes==1);
    } else {
        assert(q.failed&&m.halted && (q.last_is||q.last_host_int||mode==TIMEOUT||mode==BAD_DCMD));
        unsigned count=m.submitted;
        assert(lc_transfer(&q,1024,1,data+8,false)!=0&&lc_flush(&q)!=0&&m.submitted==count);
        if(!write)for(unsigned i=0;i<blocks*512;i++)assert(data[8+i]==0xa7);
        if(mode==STALE_ERROR)assert(!m.submitted);
    }
    for(unsigned i=0;i<8;i++)assert(data[i]==0xa7&&data[8+LC_MAX_BYTES+i]==0xa7);
    if(mode==DELAYED)assert(m.waits>100);
    unsigned before=m.submitted;
    assert(lc_transfer(&q,1024,1025,data,false)!=0 && lc_transfer(&q,UINT32_MAX,2,data,false)!=0);
    assert(m.submitted==before);
    free(m.cpu);free(m.device);free(m.disk);free(data);
}
int main(void) {
    unsigned counts[]={1,8,128,129,256,1024};
    for(unsigned i=0;i<6;i++)for(unsigned mode=NORMAL;mode<=STALE_ERROR;mode++)for(unsigned wr=0;wr<2;wr++)run(mode,counts[i],wr);
    puts("PASS actual CQHCI: 36-bit DMA, 128-bit tasks, 64 KiB zero-length encoding, eight out-of-order tags, 512 KiB read/write, cache ownership, DCMD flush, error/timeout quarantine and caller bounds");
}
