#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "present_policy.h"
int main(void){
 uint8_t in[LMP_SWAP_BYTES],out[LMP_SWAP_BYTES+16];memset(in,0,sizeof(in));memset(out,0xa5,sizeof(out));
 lmp_put(in,0x50,912);lmp_put(in,0x54,77);lmp_put(in,0x104,1);
 assert(lmp_rewrite(out,in,sizeof(in),1125,2436,88,1080,2340));
 assert(lmp_word(out,0x50)==912&&lmp_word(out,0x54)==88&&lmp_word(out,0x6c)==1080&&lmp_word(out,0x70)==2340);
 for(size_t i=0;i<sizeof(in);i++)if(!(i>=0x54&&i<0x58)&&!(i>=0x6c&&i<0x74))assert(in[i]==out[i]);
 for(size_t i=sizeof(in);i<sizeof(out);i++)assert(out[i]==0xa5);
 lmp_put(in,0x6c,1125);lmp_put(in,0x70,2436);lmp_put(in,0xcc,1125);lmp_put(in,0xd0,2436);
 assert(lmp_full_frame(in,sizeof(in),1125,2436));
 size_t fields[]={0x58,0x5c,0x60,0x64,0x68,0xc4,0xc8};
 for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++){lmp_put(in,fields[i],1);assert(!lmp_full_frame(in,sizeof(in),1125,2436));lmp_put(in,fields[i],0);}
 for(size_t n=0;n<sizeof(in);n++)assert(!lmp_full_frame(in,n,1125,2436));
 lmp_put(in,0x6c,0xffffffff);assert(!lmp_full_frame(in,sizeof(in),1125,2436));
 puts("PASS swap ABI: full-frame native GPU output preserves virtual layout and request identity; partial/multilayer/invalid requests unchanged");
}
