#include "present_cache_hook.h"
#include <assert.h>
#include <stdio.h>
struct map_case { struct lp_vm_region region[18];unsigned calls;uintptr_t target; };
static int query(uintptr_t target,uint32_t depth,struct lp_vm_region *out,void *context) {
    struct map_case *c=context;assert(target==c->target && depth<18);c->calls++;
    *out=c->region[depth];return !out->error;
}
static void mapping_tests(void) {
    struct map_case c={.target=0x1898766a4ull};struct lp_vm_region out;
    /* The old BASIC query sees R, while the executable leaf lives below it. */
    c.region[0]=(struct lp_vm_region){0x180000000ull,0x40000000ull,0,1,1,0};
    c.region[1]=(struct lp_vm_region){0x189870000ull,0x10000,1,5,0,0};
    assert(!(c.region[0].protection&4));
    assert(lp_executable_page(c.target,16384,4,query,&c,&out));
    assert(c.calls==2 && out.depth==1 && out.protection==5);
    assert(!lp_executable_page(c.target,16384,6,query,&c,&out));
    c.region[1].protection=7;assert(lp_executable_page(c.target,16384,6,query,&c,&out));
    c.region[1].protection=1;assert(!lp_executable_page(c.target,16384,4,query,&c,&out));
    c.region[1].protection=5;c.region[1].start=c.target+4;
    assert(!lp_executable_page(c.target,16384,4,query,&c,&out)); /* query skipped a hole */
    c.region[1].start=c.target-4;c.region[1].bytes=8;
    assert(!lp_executable_page(c.target,16384,4,query,&c,&out)); /* not a full page */
    c.region[1].error=5;assert(!lp_executable_page(c.target,4096,4,query,&c,&out)&&out.error==5);
    for(unsigned i=0;i<18;i++)c.region[i]=(struct lp_vm_region){0x180000000ull,0x40000000ull,i,1,1,0};
    c.calls=0;assert(!lp_executable_page(c.target,4096,4,query,&c,&out)&&c.calls==16);
    c.calls=0;assert(!lp_executable_page(c.target,6000,4,query,&c,&out)&&!c.calls);
    uint32_t source[1024],copy[1024];for(unsigned i=0;i<1024;i++)source[i]=i*17u;
    const size_t offset=0x6a4;uint32_t prior=source[offset/4];
    assert(lp_copy_code_page(copy,source,sizeof(source),offset,prior,0xfeed1234));
    assert(source[offset/4]==prior && copy[offset/4]==0xfeed1234);
    assert(lp_code_page_unchanged(source,copy,sizeof(source),offset,prior));
    for(unsigned i=0;i<1024;i++)if(i!=offset/4)assert(source[i]==copy[i]);
    source[900]^=1;assert(!lp_code_page_unchanged(source,copy,sizeof(source),offset,prior));
    source[900]^=1;source[offset/4]^=1;
    assert(!lp_code_page_unchanged(source,copy,sizeof(source),offset,prior));
    assert(!lp_copy_code_page(copy,source,sizeof(source),offset,prior,0));
    assert(!lp_copy_code_page(source,source,sizeof(source),offset,prior,0));
    assert(!lp_copy_code_page(copy,source,sizeof(source),sizeof(source),0,0));
}
static uintptr_t decode(uintptr_t pc,const uint32_t code[3]) {
    int64_t pages=((code[0]>>29)&3)|(((code[0]>>5)&0x7ffff)<<2);
    if(pages&(1ll<<20))pages-=1ll<<21;
    return (uintptr_t)((int64_t)(pc&~(uintptr_t)4095)+pages*4096+((code[1]>>10)&4095));
}
int main(void) {
    mapping_tests();
    uint32_t original[3]={0xf0ef4dd0,0x911a9210,0xd61f0200};
    {
        /* G7 shared-cache slide: the low-loaded injection may be beyond ADRP. */
        uintptr_t slide=0x542f4000,stub=0x1afb974d4ull+slide,target=0x18e5526a4ull+slide;
        uintptr_t far=0x1000096a4ull,nearby=0x2800006a4ull;
        uint32_t patched[3]={original[0],original[1],original[2]};
        assert(!lp_stub_plan(stub,original,target,far,&patched[0]));
        assert(lp_stub_plan(stub,original,target,nearby,&patched[0]));
        assert(decode(stub,patched)==nearby);
        uint8_t bridge[4096]={0};assert(lp_bridge_code(bridge,sizeof(bridge),far));
        uint32_t first,second;uint64_t literal;
        memcpy(&first,bridge+LP_STUB_LOW,4);memcpy(&second,bridge+LP_STUB_LOW+4,4);
        assert((first&31)==16 && ((first>>5)&0x7ffff)*4==12 && second==0xd61f0200);
        memcpy(&literal,bridge+LP_STUB_LOW+12,8);assert(literal==far);
        assert(!lp_bridge_code(bridge,LP_STUB_LOW+19,far));
    }
    for(unsigned i=0;i<32;i++) {
        uintptr_t slide=(uintptr_t)i*0x400000;
        uintptr_t stub=0x1afb974d4ull+slide,target=0x18e5526a4ull+slide,replacement=0x1034506a4ull+slide;
        uint32_t patched[3]={original[0],original[1],original[2]},back=0;
        assert(lp_stub_plan(stub,original,target,replacement,&patched[0]));
        assert(decode(stub,original)==target && decode(stub,patched)==replacement);
        assert(lp_stub_plan(stub,patched,replacement,target,&back)&&back==original[0]);
        assert(!lp_stub_plan(stub,original,target+4,replacement,&back));
        assert(!lp_stub_plan(stub,original,target,replacement+4,&back));
        assert(!lp_stub_plan(stub,original,target,0x9000006a4ull,&back));
        assert(patched[1]==original[1]&&patched[2]==original[2]);
    }
    uint32_t entry[12]={0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd,0xaa0003f3,
        0xb9401400,0x91006262,0x528000a1,0x52808d83,0xd2800004,0xd2800005,0x9425cfde};
    assert(lp_swap_entry(entry));assert(lp_call_target(0x1af22355cull,entry[11])==0x1afb974d4ull);
    for(unsigned i=0;i<11;i++){entry[i]^=1;assert(!lp_swap_entry(entry));entry[i]^=1;}
    entry[11]=0;assert(!lp_swap_entry(entry));
    puts("PASS actual 20H392 entry/stub, ASLR slides, old/new target atomicity, changed signature/target/range rejected");
}
