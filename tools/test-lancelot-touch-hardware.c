/* Host models, not device evidence: compile actual SPI, platform and NVT code. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "lancelot_spi.h"
#include "lancelot_touch_platform.h"
#include "lancelot_nt36672_hw.c"  /* include actual implementation to fuzz private header parser */

struct spi_model {
	uint32_t regs[16], fifo[8], rx[8], status;
	unsigned tx_words, rx_at, output_bytes, acts, resumes, writes;
	unsigned enters, exits;
	uint8_t output[258];
	bool timeout, finish, both, critical;
	uint64_t tick;
	uint64_t event_ready;
};
static uint64_t st(void *ctx) { return ++((struct spi_model *)ctx)->tick; }
static uint64_t se(void *ctx) { struct spi_model *m=ctx;assert(!m->critical);m->critical=true;m->enters++;return 0x65; }
static void sx(void *ctx,uint64_t state) { struct spi_model *m=ctx;assert(m->critical && state==0x65);m->critical=false;m->exits++; }
static void sd(void *ctx,uint32_t us) { struct spi_model *m=ctx;assert(m->critical && us && us<3000);m->tick+=us; }
static uint32_t sr(void *ctx, uint32_t offset)
{
	struct spi_model *m = ctx;
	assert(m->critical);
	if (offset == LS_STATUS) { assert(!m->status || m->tick>=m->event_ready);uint32_t v = m->status; m->status = 0; return v; }
	if (offset == LS_STATUS1) return m->timeout ? 0 : 1;
	if (offset == LS_RX) { assert(m->rx_at < 8); return m->rx[m->rx_at++]; }
	assert(offset < sizeof(m->regs) && !(offset & 3)); return m->regs[offset / 4];
}
static void sw(void *ctx, uint32_t offset, uint32_t value)
{
	struct spi_model *m = ctx; m->writes++;
	assert(m->critical);
	assert(offset == LS_CMD || offset == LS_TX || offset == LS_CFG0 || offset == LS_CFG1 || offset == LS_CFG2 || offset == LS_PAD);
	if (offset == LS_TX) { assert(m->tx_words < 8); m->fifo[m->tx_words++] = value; return; }
	if(offset==LS_CMD && (value&(LS_ACT|LS_RESUME)))
		assert((m->regs[LS_CMD/4]&(LS_MSB|LS_IE|LS_PAUSE))==(LS_MSB|LS_IE|LS_PAUSE));
	m->regs[offset / 4] = value;
	if (offset != LS_CMD) return;
	assert(!(value & ((1u << 10) | (1u << 11) | (1u << 8) | (1u << 9)))); /* no DMA/mode1..3 */
	if (value & LS_RESET) { m->tx_words = 0; m->rx_at = 0; m->status = 0; return; }
	if (!(value & (LS_ACT | LS_RESUME))) return;
	if (value & LS_ACT) { assert(!m->output_bytes); m->acts++; } else { assert(m->output_bytes); m->resumes++; }
	assert(value & LS_PAUSE);
	unsigned n = ((m->regs[LS_CFG1 / 4] >> 16) & 1023) + 1;
	assert(n <= 32 && m->tx_words == (n + 3) / 4 && m->output_bytes + n <= sizeof(m->output));
	for (unsigned i = 0; i < n; i++) m->output[m->output_bytes++] = (uint8_t)(m->fifo[i / 4] >> (8 * (i % 4)));
	for (unsigned i = 0; i < 8; i++) m->rx[i] = m->fifo[i] ^ 0xa5a5a5a5u;
	m->tx_words = m->rx_at = 0;
	m->status = m->timeout ? 0 : m->finish ? 1 : m->both ? 3 : 2;
	m->event_ready=m->tick+(n*8000000u+1857142-1)/1857142+2; /* configured 26MHz/14 */
}
static void test_spi(void)
{
	for (unsigned status = 2; status <= 3; status++)
	for (unsigned alignment = 0; alignment < 4; alignment++) for (unsigned n = 1; n <= 258; n++) {
		struct spi_model m = {.both=status==3};
		struct lancelot_spi s = {.ctx=&m,.read=sr,.write=sw,.ticks=st,.delay_us=sd,.timeout=10000,.critical_enter=se,.critical_exit=sx};
		uint8_t *a = malloc(n + alignment), *b = malloc(n + alignment); assert(a && b);
		for (unsigned i = 0; i < n; i++) a[alignment+i] = (uint8_t)(i * 37 + n);
		assert(lancelot_spi_configure(&s, 26000000, 2000000) == 0);
		assert(lancelot_spi_transfer(&s, a+alignment, b+alignment, n) == 0);
		assert(m.output_bytes == n && m.acts == 1 && m.resumes == (n - 1) / 32);
		assert(!memcmp(m.output,a+alignment,n));
		for (unsigned i=0;i<n;i++) assert(b[alignment+i] == (uint8_t)(a[alignment+i] ^ 0xa5));
		assert(m.regs[LS_CMD/4] == LS_MSB && s.messages == 1 && !s.failures);
		assert(m.enters==1 && m.exits==1 && !m.critical);
		free(a);free(b);
	}
	struct spi_model m = {0}; struct lancelot_spi s = {.ctx=&m,.read=sr,.write=sw,.ticks=st,.delay_us=sd,.timeout=10,.critical_enter=se,.critical_exit=sx};
	uint8_t buf[1] = {0};
	assert(lancelot_spi_configure(&s, 0, 2000000) == LS_ARGUMENT && m.writes == 0);
	assert(lancelot_spi_configure(&s, 109200000, 2000000) == 0 && m.writes == 0);
	assert((s.cfg0 >> 16) == 25 && s.cfg2 == (27u | (27u << 16))); /* 26 cycles >=230ns */
	assert(lancelot_spi_configure(&s, 26000000, 2000000) == 0);
	assert((s.cfg0 >> 16) == 5 && s.cfg2 == (6u | (6u << 16)));
	assert(lancelot_spi_transfer(&s, buf, buf, 0) == LS_ARGUMENT && !m.writes);
	assert(lancelot_spi_transfer(&s, buf, buf, 259) == LS_ARGUMENT && !m.writes);
	m.timeout=true;assert(lancelot_spi_transfer(&s,buf,buf,1)==LS_TIMEOUT && m.regs[LS_CMD/4]==LS_MSB);
	assert(!m.critical && m.enters==m.exits);
	memset(&m,0,sizeof(m));m.finish=true;assert(lancelot_spi_transfer(&s,buf,NULL,1)==LS_STATUS_ERROR);
	assert(!m.critical && m.enters==m.exits);
	s.critical_exit=NULL;m.writes=0;assert(lancelot_spi_transfer(&s,buf,NULL,1)==LS_ARGUMENT && !m.writes);
	puts("SPI0 FIFO: lengths1..258/alignment0..3, pause/CS, words, noDMA, timeout/reset passed");
}

struct platform_model { uint32_t regs[LP_BANKS][1024]; uint64_t tick; unsigned writes; bool stuck; };
static uint64_t pt(void *ctx) { return ++((struct platform_model *)ctx)->tick; }
static uint32_t pr(void *ctx, unsigned bank, uint32_t offset) {
	struct platform_model *m=ctx; assert(bank<LP_BANKS && offset<4096 && !(offset&3));return m->regs[bank][offset/4];
}
static void pw(void *ctx, unsigned bank, uint32_t offset, uint32_t value)
{
	struct platform_model *m=ctx;m->writes++;
	assert(bank!=LP_TOP && bank!=LP_PLL); /* shared parent and PLL must NEVER be written */
	if (m->stuck) return;
	if(bank==LP_INFRA) {
		assert((offset==0x88 || offset==0x8c) && value==2);
		if(offset==0x88)m->regs[bank][0x94/4]|=value;else m->regs[bank][0x94/4]&=~value;return;
	}
	if(bank==LP_GPIO && (offset==0x30c || offset==0x34c)) {
		unsigned allowed=offset==0x30c ? 0xf0 : 0xffff;assert(!(value&~allowed));
		uint32_t *r=&m->regs[bank][(offset-12)/4];
		for(unsigned i=0;i<8;i++) {
			if(value&(8u<<(i*4))) *r=(*r&~(15u<<(i*4)))|(value&(7u<<(i*4)));
		}
		return;
	}
	assert((bank==LP_RM && (offset==0x64 || offset==0x68)) ||
	       (bank==LP_GPIO && (offset==4 || offset==8 || offset==0x14 || offset==0x18 || offset==0x114 || offset==0x118)));
	uint32_t base=offset&~15u;uint32_t *r=&m->regs[bank][base/4];
	if((offset&15)==4)*r|=value;else *r&=~value;
}
static void test_platform(void)
{
	struct platform_model m={0};m.regs[LP_TOP][0x60/4]=0x01000101;m.regs[LP_INFRA][0x94/4]=0xff55aafa;
	m.regs[LP_PLL][0x258/4]=1;m.regs[LP_PLL][0x25c/4]=0x81150000;m.regs[LP_PLL][0x264/4]=1;
	m.regs[LP_GPIO][0x340/4]=0x12000000;m.regs[LP_GPIO][0x300/4]=0x123;m.regs[LP_GPIO][0x00]=0xabcd;
	m.regs[LP_GPIO][0x10/4]=0xff00;m.regs[LP_GPIO][0x110/4]=0xa5a5;m.regs[LP_RM][0x60/4]=0x12345;
	uint32_t before[LP_BANKS][1024];memcpy(before,m.regs,sizeof(before));
	struct lancelot_touch_resources p={.ctx=&m,.read=pr,.write=pw,.ticks=pt,.timeout=20};
	assert(lancelot_touch_prepare_resources(&p)==0);
	assert(p.source_hz==109200000); /* postdiv=2, live mainpll=1092MHz, parent=/10 */
	assert((m.regs[LP_GPIO][0x340/4]&0xffff)==0x1111);
	assert((m.regs[LP_INFRA][0x94/4]&~2u)==(before[LP_INFRA][0x94/4]&~2u)); /* eMMC gate preserved */
	assert(lancelot_touch_restore_resources(&p)==0 && !memcmp(before,m.regs,sizeof(before)));
	m.writes=0;m.regs[LP_TOP][0x60/4]|=0x80000000;p.changed=false;
	assert(lancelot_touch_prepare_resources(&p)==-2 && m.writes==0);
	m.regs[LP_TOP][0x60/4]&=~0x80000000u;
	m.regs[LP_PLL][0x264/4]=3;assert(lancelot_touch_prepare_resources(&p)==-2 && !m.writes);
	m.regs[LP_PLL][0x264/4]=1;m.regs[LP_PLL][0x258/4]=0;
	assert(lancelot_touch_prepare_resources(&p)==-2 && !m.writes);
	m.regs[LP_PLL][0x258/4]=1;m.regs[LP_PLL][0x25c/4]=0;
	assert(lancelot_touch_prepare_resources(&p)==-2 && !m.writes);
	m.regs[LP_PLL][0x25c/4]=0x81150000;
	for(unsigned parent=0;parent<4;parent++) {
		m.regs[LP_TOP][0x60/4]=parent<<24;
		assert(lancelot_touch_prepare_resources(&p)==0);
		assert(p.source_hz==(parent==0 ? 26000000 : parent==1 ? 109200000 : parent==2 ? 78000000 : 91000000));
		assert(lancelot_touch_restore_resources(&p)==0);
	}
	m.regs[LP_INFRA][0x94/4]=0;m.stuck=true;
	assert(lancelot_touch_prepare_resources(&p)==-4 && p.changed);
	puts("MT6768 resources: live PLL rate, active sibling buses, zero shared clock/PLL writes, rollback/timeouts passed");
}

struct nvt_model { uint8_t ram[0x40000]; uint32_t page; unsigned calls,writes,resets,delay,fail_call,fail_count; bool bad_crc,bad_result,boot_timeout,ready; };
static int nt(void *ctx,const uint8_t *tx,uint8_t *rx,uint32_t n)
{
	struct nvt_model *m=ctx;m->calls++;
	if(m->fail_call && m->calls>=m->fail_call && m->calls-m->fail_call<(m->fail_count ? m->fail_count : 1))return -1;
	assert(n && n<=258);
	if(tx[0]==0xff && n==3 && !rx) {m->page=(uint32_t)tx[1]<<15|(uint32_t)tx[2]<<7;return 0;}
	uint32_t addr=m->page+(tx[0]&127);
	if(rx) {
		assert(!(tx[0]&0x80) && n>=3 && addr+n-2<=sizeof(m->ram));rx[0]=0xaa;rx[1]=0xbb;memcpy(rx+2,m->ram+addr,n-2);return 0;
	}
	assert(tx[0]&0x80);assert(addr+n-1<=sizeof(m->ram));m->writes++;
	memcpy(m->ram+addr,tx+1,n-1);
	if(addr==LN_RESET) {assert(n==2 && tx[1]==0x69);m->resets++;}
	if(addr==0x3f10d && tx[1]==1) {
		m->ready=true;
		m->ram[LN_EVENT+0x60]=m->boot_timeout ? 0 : 0xa3;
		m->ram[0x3f133]=m->bad_crc ? 5 : 4;
		memcpy(m->ram+0x3f120,m->ram+0x3f100,8);
		if(m->bad_result)m->ram[0x3f120]^=1;
		uint8_t *info=m->ram+LN_EVENT+0x78;info[0]=8;info[1]=0xf7;info[4]=0x04;info[5]=0x38;info[6]=0x09;info[7]=0x24;
	}
	return 0;
}
static void nd(void *ctx,uint32_t us) {
	struct nvt_model *m=ctx;assert(us<=10000);m->delay+=us;
	/* Real firmware reuses R_DLM for Header CRC after BLD succeeds. */
	if(m->ready && us>=5000) {const uint8_t header_crc[4]={0x76,0x70,0x25,0x98};memcpy(m->ram+0x3f124,header_crc,4);}
}
static void ninit(struct nvt_model *m) {memset(m,0,sizeof(*m));const uint8_t trim[6]={10,0,0,0x72,0x66,3};memcpy(m->ram+LN_TRIM,trim,6);}
static void test_nvt(const char *fwpath)
{
	struct nvt_model *m=calloc(1,sizeof(*m));assert(m);ninit(m);
	struct ln_device d={.ctx=m,.transfer=nt,.delay_us=nd};
	assert(ln_probe(&d)==0 && d.identified && !m->writes);
	uint8_t packet[66];for(unsigned i=0;i<65;i++)m->ram[LN_EVENT+i]=(uint8_t)(i+7);
	assert(ln_read_report(&d,packet)==LN_ARGUMENT); /* identity alone is not a running firmware */
	assert(ln_read(&d,LN_EVENT,packet,65)==0 && !memcmp(packet,m->ram+LN_EVENT,65));
	unsigned calls=m->calls;assert(ln_read(&d,0x3ffff,packet,2)==LN_ARGUMENT && m->calls==calls);
	m->ram[LN_TRIM]=11;assert(ln_probe(&d)==LN_ID && !d.identified && !m->resets);
	assert(ln_boot_firmware(&d,NULL,0)==LN_ID && !m->writes);
	if(!fwpath) {puts("NVT: framing/identity fail-closed passed; private firmware tests SKIPPED");free(m);return;}
	FILE *f=fopen(fwpath,"rb");assert(f);uint8_t *fw=malloc(LN_FW_BYTES+1);assert(fw);
	assert(fread(fw,1,LN_FW_BYTES+1,f)==LN_FW_BYTES);fclose(f);
	struct ln_firmware plan;assert(ln_validate_firmware(fw,LN_FW_BYTES,&plan)==0 && plan.count==15);
	ninit(m);assert(ln_probe(&d)==0);
	fw[LN_FW_BYTES-1]^=1;calls=m->calls;assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==LN_FIRMWARE && m->calls==calls && !m->writes);fw[LN_FW_BYTES-1]^=1;
	assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==0 && m->resets==1 && d.version==8 && d.running);
	assert(d.dlm_crc==plan.part[1].crc && ln_le32(m->ram+0x3f124)==0x98257076); /* snapshot before register reuse */
	assert(ln_read_report(&d,packet)==0 && packet[0]==0 && !memcmp(packet+1,m->ram+LN_EVENT,65));
	for(unsigned i=0;i<plan.count;i++)if(plan.part[i].last)assert(!memcmp(m->ram+plan.part[i].sram,fw+plan.part[i].bin,plan.part[i].last+1));
	ninit(m);m->bad_crc=true;assert(ln_probe(&d)==0);assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==LN_CRC && !d.running);
	ninit(m);m->bad_result=true;assert(ln_probe(&d)==0);assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==LN_CRC);
	ninit(m);m->boot_timeout=true;assert(ln_probe(&d)==0);assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==LN_BOOT && m->delay<=520000);
	ninit(m);assert(ln_probe(&d)==0);m->fail_call=m->calls+4;m->fail_count=5;assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)!=0);
	ninit(m);assert(ln_probe(&d)==0);m->fail_call=m->calls+100;m->fail_count=5;
	assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==LN_IO && m->resets==1 && !m->ready);
	ninit(m);m->fail_call=1;assert(ln_probe(&d)==LN_OK && m->calls==3 && m->delay==100);
	ninit(m);m->fail_call=1;m->fail_count=5;assert(ln_probe(&d)==LN_IO && m->calls==5 && m->delay==400 && !d.identified);
	ninit(m);assert(ln_probe(&d)==0);m->fail_call=m->calls+100;
	assert(ln_boot_firmware(&d,fw,LN_FW_BYTES)==LN_OK && d.running); /* one transient RAM write */
	/* Exercise hostile header bounds even though final SHA pin rejects them. */
	for(unsigned n=0;n<4096;n++) {
		uint8_t old[4];memcpy(old,fw,4);uint32_t end=n*0x100001u;memcpy(fw,&end,4);
		(void)ln_fw_header(fw,LN_FW_BYTES,&plan);memcpy(fw,old,4);
	}
	for(unsigned i=0;i<3;i++) {
		unsigned at=i==0 ? 4 : 8;
		uint32_t bad=i==0 ? 0x3f000 : i==1 ? UINT32_MAX : LN_FW_USED;
		uint8_t old[4];memcpy(old,fw+at,4);memcpy(fw+at,&bad,4);
		assert(ln_fw_header(fw,LN_FW_BYTES,&plan)==LN_FIRMWARE);memcpy(fw+at,old,4);
	}
	assert(ln_validate_firmware(fw,LN_FW_BYTES-1,&plan)==LN_FIRMWARE);
	free(fw);free(m);
	puts("NT36672A: SPI dummy/page framing, exact firmware pin, 15 inclusive RAM regions, CRC/status, bounded boot and failures passed");
}
int main(int argc,char **argv) {test_spi();test_platform();test_nvt(argc>1 ? argv[1] : NULL);return 0;}
