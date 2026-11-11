
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define UNUSED __attribute__((unused))
#include MACROS
struct MALI_ATTRIBUTE_BUFFER {uint32_t zero;};
struct packed_attribute {uint32_t opaque[4];};
typedef struct packed_attribute MALI_ATTRIBUTE_BUFFER_PACKED_T;
#define MALI_ATTRIBUTE_BUFFER_header 0
static unsigned char *base;
static size_t offset;
static void MALI_ATTRIBUTE_BUFFER_pack(MALI_ATTRIBUTE_BUFFER_PACKED_T *p,const struct MALI_ATTRIBUTE_BUFFER *v){
 (void)v;offset=(unsigned char *)p-base;
 printf("descriptor store offset=%zu expected=16 logical_allocation=32 sizeof_packed=%zu\n",offset,sizeof(*p));fflush(stdout);
 memset(p,0,sizeof(*p));
}
int main(int argc,char **argv){
 (void)argv;
 base=malloc(32);assert(base);memset(base,0xa5,32);
 void *cpu=base;
 if(argc==1){pan_cast_and_pack(cpu+16,ATTRIBUTE_BUFFER,cfg);}
 else {pan_cast_and_pack_nodefaults(cpu+16,ATTRIBUTE_BUFFER,cfg);}
 assert(offset==16);
 for(unsigned i=0;i<16;i++)assert(base[i]==0xa5);
 for(unsigned i=16;i<32;i++)assert(base[i]==0);
 free(base);return 0;
}
