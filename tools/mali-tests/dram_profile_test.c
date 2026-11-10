#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_dram_profile.h"
struct fixture {
    uint32_t shu,pll,pcw,div,ckdiv,cona;
    unsigned calls,fail_at,change_at;
    enum lmd_bank change_bank;
};
static int read_register(void *cookie,enum lmd_bank bank,uint32_t off,uint32_t *value) {
    struct fixture *f=cookie;
    ++f->calls;assert(lmd_read_allowed(bank,off));
    if(f->calls==f->fail_at)return -5;
    uint32_t base=f->shu*0x500;
    if(bank==LMD_DRAMC)*value=f->shu<<1;
    else if(bank==LMD_EMI)*value=f->cona;
    else if(off==0x510)*value=f->pll<<31;
    else if(off==base+(f->pll?0xd94:0xd9c))*value=f->pcw;
    else if(off==base+(f->pll?0xda0:0xda8))*value=f->div;
    else {assert(off==base+0xd18);*value=f->ckdiv;}
    if(f->calls==f->change_at && bank==f->change_bank)
        *value^=bank==LMD_DRAMC?2u:off==0x510?0x80000000u:0x01000000u;
    return 0;
}
int main(void) {
    const unsigned pcw[]={69,123,46,123,59};
    const unsigned div[]={0,1,0,2,1};
    const unsigned want[]={3600,3200,2400,1600,1534};
    unsigned cases=0;
    for(unsigned sh=0;sh<3;sh++)for(unsigned pll=0;pll<2;pll++)
    for(unsigned channels=0;channels<2;channels++)for(unsigned rate=0;rate<5;rate++) {
        struct fixture f={.shu=sh,.pll=pll,.pcw=pcw[rate]<<24,.div=div[rate],.cona=channels<<8};
        struct lmd_io io={&f,read_register};struct lmd_clock result;
        assert(lmd_capture(&io,&result)==0&&result.valid&&result.mtps==want[rate]);
        assert(result.channels==(1u<<channels)&&result.shuffle==sh&&result.pll==pll);
        assert(f.calls==16);cases++;
    }
    for(unsigned fault=1;fault<=16;fault++) {
        struct fixture f={.pcw=69u<<24,.cona=1u<<8,.fail_at=fault};
        struct lmd_io io={&f,read_register};struct lmd_clock result;
        assert(lmd_capture(&io,&result)==-5&&!result.valid&&!result.mtps&&f.calls==fault);
    }
    for(unsigned mode=0;mode<3;mode++) {
        struct fixture f={.pcw=69u<<24,.cona=1u<<8,
            .change_at=mode==0?7:mode==1?8:11,.change_bank=mode==0?LMD_DRAMC:LMD_DDRPHY};
        struct lmd_io io={&f,read_register};struct lmd_clock result;
        assert(lmd_capture(&io,&result)==-11&&!result.valid&&!result.mtps);
    }
    for(unsigned invalid=0;invalid<3;invalid++) {
        struct fixture f={.pcw=69u<<24,.cona=1u<<8};
        if(invalid==0)f.shu=3;
        if(invalid==1)f.pcw=0;
        if(invalid==2)f.cona=3u<<8;
        struct lmd_io io={&f,read_register};struct lmd_clock result;
        assert(lmd_capture(&io,&result)==-95&&!result.valid&&!result.mtps);
    }
    struct fixture f={.pcw=123u<<24,.div=1,.ckdiv=1u<<27,.cona=1u<<8};
    struct lmd_io io={&f,read_register};struct lmd_clock result;
    assert(lmd_capture(&io,&result)==0&&result.mtps==1600);
    unsigned prior=f.calls;uint32_t value;
    assert(lmd_read(&io,LMD_EMI,4,&value)==-22&&f.calls==prior);
    assert(lmd_read(&io,LMD_DRAMC,0xe0,&value)==-22&&f.calls==prior);
    assert(lmd_read(&io,LMD_DDRPHY,0x1ca8,&value)==-22&&f.calls==prior);
    assert(lmd_read(&io,(enum lmd_bank)99,0,&value)==-22&&f.calls==prior);
    assert(lmd_capture(NULL,&result)==-22&&!result.valid);
    assert(lmd_capture(&io,NULL)==-22);
    printf("PASS: %u DDRPHY OPP/channel/PLL/shuffle combinations; both CKDIV paths; all 16 read failures; transition/unknown rejection; read-only allowlist\n",cases);
}
