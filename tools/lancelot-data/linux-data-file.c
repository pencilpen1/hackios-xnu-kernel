/* Freestanding Linux helper for Android staging: no block-device writes.
 * allocate initializes every byte of a new regular file; map obtains a
 * synchronized FIEMAP; patch validates the complete patch before writing.
 */
#include <stdint.h>
#include <stddef.h>
#if defined(__aarch64__)
#define N_OPEN 56
#define N_CLOSE 57
#define N_READ 63
#define N_WRITE 64
#define N_SEEK 62
#define N_PREAD 67
#define N_PWRITE 68
#define N_SYNC 82
#define N_TRUNCATE 46
#define N_STAT 80
#define N_STATFS 43
#define N_IOCTL 29
#define N_EXIT 93
#define MODE_OFFSET 16
static inline long sc(long n,long a,long b,long c,long d,long e,long f) {
 register long x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
 register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");return x0;
}
#else
#define N_OPEN 257
#define N_CLOSE 3
#define N_READ 0
#define N_WRITE 1
#define N_SEEK 8
#define N_PREAD 17
#define N_PWRITE 18
#define N_SYNC 74
#define N_TRUNCATE 77
#define N_STAT 5
#define N_STATFS 137
#define N_IOCTL 16
#define N_EXIT 60
#define MODE_OFFSET 24
static inline long sc(long n,long a,long b,long c,long d,long e,long f) {
 register long r10 __asm__("r10")=d,r8 __asm__("r8")=e,r9 __asm__("r9")=f;long result;
 __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return result;
}
#endif
static size_t length(const char*s){size_t n=0;while(s[n])n++;return n;}
static int equal(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void copy(void*out,const void*in,size_t n){uint8_t*d=out;const uint8_t*s=in;while(n--)*d++=*s++;}
static uint32_t u32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const uint8_t*p){return u32(p)|(uint64_t)u32(p+4)<<32;}
static void put32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static void output(int fd,const void*bytes,size_t n){const uint8_t*p=bytes;
 while(n){long r=sc(N_WRITE,fd,(long)p,(long)n,0,0,0);if(r<=0)sc(N_EXIT,90,0,0,0,0,0);p+=r;n-=(size_t)r;}}
static void number(uint64_t n){char b[32];unsigned i=32;do{b[--i]=(char)('0'+n%10);n/=10;}while(n);output(1,b+i,32-i);}
static void message(const char*s){output(1,s,length(s));}
static _Noreturn void die(const char*s){output(2,s,length(s));output(2,"\n",1);sc(N_EXIT,1,0,0,0,0,0);__builtin_unreachable();}
static uint64_t parse(const char*s){uint64_t n=0;if(!*s)die("empty number");while(*s){if(*s<'0'||*s>'9'||n>(UINT64_MAX-9)/10)die("invalid number");n=n*10+(unsigned)(*s++-'0');}return n;}
static long regular(const char*path,long flags,long mode,uint64_t*bytes){
 long fd=sc(N_OPEN,-100,(long)path,flags|0x20000|0x80000,mode,0,0);if(fd<0)die("cannot open exclusive/no-follow regular file");
 uint8_t st[144]={0};if(sc(N_STAT,fd,(long)st,0,0,0,0)<0||(u32(st+MODE_OFFSET)&0170000)!=0100000)die("target is not a regular file");
 *bytes=u64(st+48);return fd;
}
struct space {int64_t type,bsize;uint64_t blocks,bfree,bavail,files,ffree;int32_t fsid[2];int64_t namelen,frsize,flags,spare[4];};
static struct space space_at(const char*p){struct space s={0};if(sc(N_STATFS,(long)p,(long)&s,0,0,0,0)<0||s.type!=0xef53||s.bsize!=4096)die("expected ext4 with 4096-byte blocks");return s;}
static uint8_t buffer[1u<<20];
static void read_at(long fd,uint64_t at,size_t n,void*out){uint8_t*p=out;
 while(n){long r=sc(N_PREAD,fd,(long)p,(long)n,(long)at,0,0);if(r<=0)die("short input read");p+=r;at+=(uint64_t)r;n-=(size_t)r;}}
static void write_at(long fd,uint64_t at,size_t n,const void*in){const uint8_t*p=in;
 while(n){long r=sc(N_PWRITE,fd,(long)p,(long)n,(long)at,0,0);if(r<=0)die("file write failed; partial file preserved");p+=r;at+=(uint64_t)r;n-=(size_t)r;}}
static int allocate_file(const char*path,const char*max_string,const char*metadata_reserve){
 uint64_t cap=parse(max_string)&~UINT64_C(4095),bytes=0;if(cap<UINT64_C(67108864)||cap>UINT64_C(34359738368))die("allocation cap out of range");
 long fd=regular(path,2|64|128,0600,&bytes);if(bytes)die("new file was not empty");
 uint64_t reserved=metadata_reserve?parse(metadata_reserve):4096;
 if(reserved>65536)die("metadata reserve out of range");
 /* statfs includes ext4's metadata-only reserved cluster pool. Ordinary file
  * data cannot consume it, even as root. Keep that pool plus 256 KiB. */
 uint64_t synced=0,reported=0,keep_blocks=reserved+64;
 while(bytes<cap){struct space s=space_at(path);if(s.bfree<=keep_blocks)break;
  uint64_t available=(s.bfree-keep_blocks)*4096,n=cap-bytes;if(n>sizeof(buffer))n=sizeof(buffer);if(n>available)n=available;n&=~UINT64_C(4095);if(!n)break;
  write_at(fd,bytes,(size_t)n,buffer);bytes+=n;
  if(bytes-synced>=UINT64_C(67108864)||available<UINT64_C(2097152)){
   if(sc(N_SYNC,fd,0,0,0,0,0)<0)die("allocation fsync failed; partial file preserved");
   synced=bytes;}
  if(bytes-reported>=UINT64_C(268435456)){message("LHD_PROGRESS initialized_bytes=");number(bytes);message("\n");reported=bytes;}
 }
 if(sc(N_SYNC,fd,0,0,0,0,0)<0)die("final allocation fsync failed");
 sc(N_CLOSE,fd,0,0,0,0,0);
 if(bytes<UINT64_C(67108864))die("insufficient space for persistent data");
 struct space s=space_at(path);message("LHD_ALLOCATED file_bytes=");number(bytes);message(" free_blocks=");number(s.bfree);message(" available_blocks=");number(s.bavail);message(" block_size=4096\n");return 0;
}
struct extent {uint64_t logical,physical,bytes,reserved64[2];uint32_t flags,reserved[3];};
static struct {uint64_t start,length;uint32_t flags,mapped,count,reserved;struct extent extent[4096];} fiemap;
static uint8_t map_output[32+24*8192];
static int map_file(const char*path){
 uint64_t bytes;long fd=regular(path,0,0,&bytes);if(bytes<UINT64_C(67108864)||(bytes&4095))die("data file size/alignment invalid");
 uint64_t next=0;unsigned count=0;int last=0;
 do {
 fiemap.start=next;fiemap.length=UINT64_MAX-next;fiemap.flags=1;fiemap.count=4096;fiemap.mapped=0;
 if(sc(N_IOCTL,fd,0xc020660b,(long)&fiemap,0,0,0)<0||!fiemap.mapped||fiemap.mapped>4096)die("FIEMAP failed");
 for(unsigned i=0;i<fiemap.mapped;i++){struct extent*e=&fiemap.extent[i];
  if(e->logical!=next||!e->physical||!e->bytes||(e->logical&4095)||(e->physical&4095)||(e->bytes&4095)||
     (e->flags&~0x1001u)||e->bytes>bytes-next)die("hole/unwritten/shared/malformed extent rejected");
  if(count){uint8_t*p=map_output+32+24*(count-1);uint64_t previous=u64(p+8),blocks=u64(p+16);
   if(previous+blocks==e->physical/4096){put64(p+16,blocks+e->bytes/4096);next+=e->bytes;continue;}}
  if(count==8192)die("more than 8192 physical ranges");
  uint8_t*p=map_output+32+24*count++;put64(p,e->logical/4096);put64(p+8,e->physical/4096);put64(p+16,e->bytes/4096);next+=e->bytes;
 }
 last=(fiemap.extent[fiemap.mapped-1].flags&1)!=0;
 if(next==bytes&&!last)die("FIEMAP lacks final extent marker");
 } while(next<bytes&&!last);
 if(next!=bytes||!last)die("incomplete FIEMAP extent set");
 copy(map_output,"LHDATA1\0",8);put32(map_output+8,1);put32(map_output+12,4096);put64(map_output+16,bytes);put32(map_output+24,count);
 output(1,map_output,32+24*count);sc(N_CLOSE,fd,0,0,0,0,0);return 0;
}
static int trim_empty(const char *path,const char *size_string) {
 uint64_t bytes,new_bytes=parse(size_string);long fd=regular(path,2,0,&bytes);
 if(new_bytes<UINT64_C(67108864)||(new_bytes&4095)||new_bytes>=bytes||bytes-new_bytes>UINT64_C(1048576))die("empty-file trim range invalid");
 read_at(fd,0,4096,buffer);for(unsigned i=0;i<4096;i++)if(buffer[i])die("refusing to trim an already formatted/nonzero file");
 if(sc(N_TRUNCATE,fd,(long)new_bytes,0,0,0,0)<0||sc(N_SYNC,fd,0,0,0,0,0)<0)die("empty-file trim failed");
 sc(N_CLOSE,fd,0,0,0,0,0);message("LHD_TRIMMED old_bytes=");number(bytes);message(" file_bytes=");number(new_bytes);message("\n");return 0;
}
static int patch_file(const char*path,const char*patch){
 uint64_t bytes,pbytes;long fd=regular(path,2,0,&bytes),pf=regular(patch,0,0,&pbytes);uint8_t header[32],entry[16];
 if(pbytes<32)die("patch header missing");
 read_at(pf,0,32,header);
 if(!equal((const char*)header,"LHDPAT1")||u64(header+8)!=bytes||u64(header+24))die("patch header/target size mismatch");
 uint64_t count=u64(header+16),position=32,previous=0;if(!count||count>4096)die("patch extent count invalid");
 for(uint64_t i=0;i<count;i++){
  if(position>pbytes||pbytes-position<16)die("patch entries truncated");
  read_at(pf,position,16,entry);position+=16;
  uint64_t at=u64(entry),n=u64(entry+8);
  if(at<previous||(at&511)||!n||(n&511)||at>bytes||n>bytes-at||n>pbytes-position)die("patch range invalid");
  previous=at+n;position+=n;
 }
 if(position!=pbytes)die("unexpected patch tail");
 position=32;
 for(uint64_t i=0;i<count;i++){
  read_at(pf,position,16,entry);position+=16;uint64_t at=u64(entry),n=u64(entry+8);
  while(n){size_t piece=n>sizeof(buffer)?sizeof(buffer):(size_t)n;read_at(pf,position,piece,buffer);write_at(fd,at,piece,buffer);position+=piece;at+=piece;n-=piece;}
 }
 if(sc(N_SYNC,fd,0,0,0,0,0)<0)die("format patch fsync failed");
 sc(N_CLOSE,pf,0,0,0,0,0);sc(N_CLOSE,fd,0,0,0,0,0);
 message("LHD_PATCHED file_bytes=");number(bytes);message(" segments=");number(count);message("\n");return 0;
}
long linux_main(uintptr_t*stack){int argc=(int)stack[0];char**argv=(char**)&stack[1];
 if((argc==4||argc==5)&&equal(argv[1],"allocate"))return allocate_file(argv[2],argv[3],argc==5?argv[4]:NULL);
 if(argc==3&&equal(argv[1],"map"))return map_file(argv[2]);
 if(argc==4&&equal(argv[1],"patch"))return patch_file(argv[2],argv[3]);
 if(argc==4&&equal(argv[1],"trim-empty"))return trim_empty(argv[2],argv[3]);
 if(argc==3&&equal(argv[1],"space")){struct space s=space_at(argv[2]);message("LHD_SPACE block_size=4096 blocks=");number(s.blocks);message(" free_blocks=");number(s.bfree);message(" available_blocks=");number(s.bavail);message("\n");return 0;}
 die("usage: linux-data-file allocate PATH MAX_BYTES [META_RESERVED_BLOCKS] | map PATH | patch PATH PATCH | trim-empty PATH NEW_BYTES | space PATH");
}
#ifdef LHD_HOST_TEST
int main(int argc,char**argv){uintptr_t stack[16]={0};if(argc>14)die("too many arguments");stack[0]=(uintptr_t)argc;for(int i=0;i<argc;i++)stack[i+1]=(uintptr_t)argv[i];return (int)linux_main(stack);}
#elif defined(__aarch64__)
__asm__(".global _start\n_start:\nmov x0, sp\nbl linux_main\nmov x8, #93\nsvc #0\n");
#else
__asm__(".global _start\n_start:\nmov %rsp, %rdi\ncall linux_main\nmov %rax, %rdi\nmov $60, %rax\nsyscall\n");
#endif
