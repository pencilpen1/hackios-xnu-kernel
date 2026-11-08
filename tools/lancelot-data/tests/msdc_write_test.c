/* Controlled card/FIFO model for the actual production bounded CMD24 path. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "lancelot_msdc.h"
enum { OK, BUSY, FIFO_STUCK, CMD_TIMEOUT, CMD_ERROR, EARLY_DONE, DATA_ERROR, DATA_TIMEOUT, NOT_READY, FLUSH_ERROR };
struct model {unsigned mode,phase,command,sent,depth;uint32_t arg,irq,intmask,response;uint64_t ticks;uint8_t disk[512];unsigned writes,flushes,bursts;};
static uint32_t read_reg(void*ctx,uint32_t reg){
 struct model*m=ctx;
 switch(reg){
 case LM_CFG:return 9;
 case LM_STATUS:return m->mode==BUSY?1:0;
 case LM_INTEN:return m->intmask;
 case LM_PS:return LM_DAT0;
 case LM_FIFO:if(m->mode==FIFO_STUCK)return 1u<<31;return 0;
 case LM_RESPONSE:return m->response;
 case LM_INT:
  if(m->phase==1){if(m->command==24&&m->mode==CMD_TIMEOUT)return 0;if(m->command==24&&m->mode==CMD_ERROR)return 1u<<10;return LM_CMD_READY;}
  if(m->phase==2){if(m->mode==DATA_ERROR)return 1u<<16;if(m->mode==EARLY_DONE)return LM_DATA_DONE;
   if(m->mode!=DATA_TIMEOUT&&m->sent==512)return LM_DATA_DONE;}
  return 0;
 default:abort();
 }
}
static void write_reg(void*ctx,uint32_t reg,uint32_t value){
 struct model*m=ctx;
 switch(reg){
 case LM_INTEN:m->intmask=value;break;
 case LM_FIFO:assert(value==(1u<<31));break;
 case LM_INT:if(value==LM_CMD_READY&&m->phase==1)m->phase=m->command==24?2:0;break;
 case LM_ARG:m->arg=value;break;
 case LM_COUNT:assert(value==1);break;
 case LM_CMD:
  m->command=value&63;m->phase=1;m->response=0x900;
  if(m->command==24){assert(value==LM_WRITE_SINGLE&&m->arg==0x12345);m->sent=0;m->writes++;}
  else if(m->command==13){assert(m->arg==(1u<<16));if(m->mode==NOT_READY)m->response=7u<<9;}
  else if(m->command==12)assert(value==LM_STOP);
  else if(m->command==6){assert(m->arg==((3u<<24)|(32u<<16)|(1u<<8)));m->flushes++;if(m->mode==FLUSH_ERROR)m->response|=0x80;}
  else assert(false);
  break;
 case LM_TX:
  assert(m->phase==2&&m->sent<512&&m->depth==1);
  for(unsigned i=0;i<4;i++)m->disk[m->sent++]=(uint8_t)(value>>(8*i));
  break;
 default:abort();
 }
}
static uint64_t tick(void*c){struct model*m=c;assert(m->ticks<100000);return ++m->ticks;}
static void write_fifo(void *ctx,const uint8_t *data,unsigned words){
 struct model*m=ctx;assert(words&&words<=32);m->bursts++;
 for(unsigned i=0;i<words;i++){
  uint32_t value=(uint32_t)data[4*i]|(uint32_t)data[4*i+1]<<8|
   (uint32_t)data[4*i+2]<<16|(uint32_t)data[4*i+3]<<24;
  write_reg(ctx,LM_TX,value);
 }
}
static uint64_t enter(void*c){struct model*m=c;assert(!m->depth++);uint64_t s=m->irq;m->irq=0;return s;}
static void leave(void*c,uint64_t s){struct model*m=c;assert(m->depth--==1&&!m->irq);m->irq=(uint32_t)s;}
static struct lancelot_msdc setup(struct model*m,unsigned mode){
 memset(m,0,sizeof(*m));m->mode=mode;m->irq=1;m->intmask=0xa5a5ffff;
 return(struct lancelot_msdc){.ctx=m,.read=read_reg,.write=write_reg,.ticks=tick,.timeout=2048,
  .critical_enter=enter,.critical_exit=leave,.rca=1,.media_write_enabled=true,.media_write_first=0x12345,.media_write_end=0x12346};
}
int main(void){
 uint8_t bytes[528];for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(uint8_t)(i*37+5);
 for(unsigned burst=0;burst<2;burst++)for(unsigned align=0;align<8;align++){
  struct model m;struct lancelot_msdc c=setup(&m,OK);
  if(burst)c.write_fifo=write_fifo;
  assert(lancelot_msdc_write_sector(&c,0x12345,bytes+align)==0);
  assert(m.writes==1&&m.sent==512&&!memcmp(bytes+align,m.disk,512)&&!m.depth&&m.irq==1&&m.intmask==0xa5a5ffff);
  assert(m.bursts==(burst?4u:0u));
  assert(lancelot_msdc_flush(&c)==0&&m.flushes==1&&!m.depth&&m.irq==1);
 }
 for(unsigned burst=0;burst<2;burst++)for(unsigned mode=BUSY;mode<=NOT_READY;mode++){
  struct model m;struct lancelot_msdc c=setup(&m,mode);
  if(burst)c.write_fifo=write_fifo;
  assert(lancelot_msdc_write_sector(&c,0x12345,bytes)!=0&&!m.depth&&m.irq==1&&m.intmask==0xa5a5ffff);
 }
 struct model m;struct lancelot_msdc c=setup(&m,FLUSH_ERROR);assert(lancelot_msdc_flush(&c)!=0&&!m.depth&&m.irq==1);
 c=setup(&m,OK);c.media_write_enabled=false;
 assert(lancelot_msdc_write_sector(&c,0x12345,bytes)==LM_CONFIG&&lancelot_msdc_flush(&c)==LM_CONFIG&&!m.writes&&!m.flushes);
 c.media_write_enabled=true;
 assert(lancelot_msdc_write_sector(&c,0x12344,bytes)==LM_CONFIG);
 assert(lancelot_msdc_write_sector(&c,0x12346,bytes)==LM_CONFIG&&!m.writes);
 assert(lm_command(&c,6u|(7u<<7),(3u<<24)|(179u<<16))==LM_CONFIG); /* no eMMC partition switch */
 assert(lm_command(&c,25u|(1u<<7),0x12345)==LM_CONFIG);
 assert(lm_command(&c,35u|(1u<<7),0)==LM_CONFIG); /* no erase */
 puts("PASS production CMD24: exact FIFO payload/alignment, each-sector scope, IRQ restoration, all timeout/error stages, CMD13 readiness and cache flush; model only");
}
