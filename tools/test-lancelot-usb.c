/* Exercise the production protocol/PIO core under ASan/UBSan with a host model. */
#include <assert.h>
#include <stdio.h>
#include "../xnu/files/bsd/dev/lancelot_musb.h"
struct model {
	uint8_t index, reg[256], input[64], output[20000];
	uint16_t tx[2], rx, count, intrtx, intrrx;
	unsigned in_at, out_size, packet_start, packets, ep0_bytes;
};
static uint8_t r8(void *ctx, unsigned reg)
{
	struct model *m=ctx;
	if(reg==LU_FIFO0||reg==LU_FIFO1){assert(m->in_at<64);return m->input[m->in_at++];}
	if(reg==LU_COUNT)return (uint8_t)m->count;
	return m->reg[reg];
}
static uint16_t r16(void *ctx,unsigned reg)
{
	struct model *m=ctx;
	if(reg==LU_TXCSR)return m->tx[m->index];
	if(reg==LU_RXCSR)return m->rx;
	if(reg==LU_COUNT)return m->count;
	if(reg==LU_INTRTX)return m->intrtx;
	if(reg==LU_INTRRX)return m->intrrx;
	return 0;
}
static void w8(void *ctx,unsigned reg,uint8_t val)
{
	struct model *m=ctx;
	if(reg==LU_INDEX){assert(val<2);m->index=val;return;}
	if(reg==LU_FIFO0){m->ep0_bytes++;return;}
	if(reg==LU_FIFO1){assert(m->out_size<sizeof(m->output));m->output[m->out_size++]=val;return;}
	if(reg==LU_INTRUSB)m->reg[reg]&=(uint8_t)~val;
	else m->reg[reg]=val;
}
static void w16(void *ctx,unsigned reg,uint16_t val)
{
	struct model *m=ctx;
	if(reg==LU_INTRTX){m->intrtx&=(uint16_t)~val;return;}
	if(reg==LU_INTRRX){m->intrrx&=(uint16_t)~val;return;}
	if(reg==LU_RXCSR){m->rx=val&1;return;}
	if(reg!=LU_TXCSR)return;
	if(m->index){
		m->tx[1]=val&1;
		if(val&1){assert(m->out_size-m->packet_start<=64);m->packet_start=m->out_size;m->packets++;}
	} else {
		if(val&0x40)m->tx[0]&=(uint16_t)~1;
		if(val&0x80)m->tx[0]&=(uint16_t)~0x10;
		if(val&2)m->tx[0]|=2;
		if(val&8)m->tx[0]|=8;
		if(val==0||val==0x100)m->tx[0]=0;
	}
}
static size_t snapshot(uint64_t cursor,uint64_t *start,uint8_t *out,size_t capacity)
{
	if(cursor>20000)cursor=0;
	*start=cursor;
	size_t n=20000-cursor;if(n>capacity)n=capacity;
	for(size_t i=0;i<n;i++)out[i]=(uint8_t)(i+cursor);
	return n;
}
static void init(struct lancelot_musb *u,struct model *m)
{
	memset(m,0,sizeof(*m));memset(u,0,sizeof(*u));u->ctx=m;
	u->r8=r8;u->r16=r16;u->w8=w8;u->w16=w16;u->snapshot=snapshot;u->boot=123;
	lu_reset(u);u->address=7;u->configuration=1;
}
static void command(struct lancelot_musb *u,struct model *m,const char *cmd)
{
	m->out_size=m->packet_start=m->packets=m->in_at=0;m->tx[1]=0;
	m->count=(uint16_t)strlen(cmd);assert(m->count<=64);memcpy(m->input,cmd,m->count);
	m->rx=1;lu_poll(u);
}
static void drain(struct lancelot_musb *u,struct model *m)
{
	for(unsigned i=0;u->reply!=LU_NONE&&i<1000;i++){m->tx[1]=0;lu_poll(u);}
	assert(u->reply==LU_NONE);
}
static uint64_t le(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(i*8);return v;}
int main(void)
{
	struct lancelot_musb u;struct model m;
	init(&u,&m);
	/* SET_ADDRESS must wait for status ACK, including coalesced interrupts. */
	u.configuration=0;
	uint8_t setup[8]={0,5,9,0,0,0,0,0};lu_setup(&u,setup);
	lu_control_poll(&u,true);assert(u.address==7);
	m.tx[0]=0;lu_control_poll(&u,true);assert(u.address==9&&m.reg[LU_ADDR]==9);
	/* SETUP FIFO is released before the descriptor data is written. */
	setup[0]=0x80;setup[1]=6;setup[2]=0;setup[3]=1;setup[6]=64;
	m.tx[0]=1;lu_setup(&u,setup);assert(!m.ep0_bytes&&u.state==LU_SETUP_ACK);
	lu_control_poll(&u,false);assert(m.ep0_bytes==18&&u.state==LU_STATUS_OUT);
	/* FS-only device correctly rejects a device qualifier. */
	setup[3]=6;lu_setup(&u,setup);assert(u.stalls==1);
	init(&u,&m);
	command(&u,&m,"oem klog 0000000000000000");assert(m.out_size==4&&!memcmp(m.output,"OKAY",4));
	command(&u,&m,"upload");assert(m.out_size==12&&!memcmp(m.output,"DATA00004028",12));
	/* TXPKTRDY backpressure does not spin or mutate an immutable snapshot. */
	for(unsigned i=0;i<100;i++)lu_poll(&u);
	assert(m.out_size==12&&u.offset==0);
	drain(&u,&m);assert(m.out_size==12+40+16384+4);
	const uint8_t *h=m.output+12;assert(!memcmp(h,"KLOG2\0\0\0",8));
	assert(le(h+8,8)==123&&le(h+16,8)==0&&le(h+24,8)==16384&&le(h+36,4)==16384);
	assert(le(h+32,4)==lu_crc32(h+40,16384));
	for(unsigned i=0;i<16384;i++)assert(h[40+i]==(uint8_t)i);
	assert(!memcmp(h+40+16384,"OKAY",4));
	command(&u,&m,"oem klog 0000000000004e20");command(&u,&m,"upload");drain(&u,&m);
	assert(m.out_size==12+40+4&&le(m.output+12+36,4)==0);
	/* Invalid cursors cannot inject or truncate a command. */
	command(&u,&m,"oem klog 000000000000000g");assert(!memcmp(m.output,"FAIL",4));
	command(&u,&m,"flash:boot");assert(!memcmp(m.output,"FAIL",4));
	command(&u,&m,"oem log 0000000000000000");assert(!memcmp(m.output,"INFOLLOG1 ",10));drain(&u,&m);
	assert(m.packets==173); /* header + ceil(4096/24) + OKAY */
	/* Reset discards the transfer, preserving the boot id and source log. */
	command(&u,&m,"upload");m.reg[LU_INTRUSB]=4;lu_poll(&u);
	assert(!u.configuration&&u.reply==LU_NONE&&u.boot==123&&m.reg[LU_INTRUSB]==0);
	/* Oversized OUT packet is flushed without touching the command buffer. */
	u.configuration=1;m.count=65;m.rx=1;m.tx[1]=0;m.in_at=0;lu_poll(&u);assert(!m.in_at&&!m.rx);
	puts("MUSB: EP0 sequencing, FS descriptors, immutable KLOG2/CRC, empty snapshot, backpressure, reset, malformed/oversize commands passed");
}
