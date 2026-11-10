/* Actual ext4 admission + data_disk.inc, with kernel objects/card modeled. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define __unused __attribute__((unused))
#define lancelot_msdc_write_sector uncalled_native_write
#define lancelot_msdc_read uncalled_native_read
#define lancelot_msdc_flush uncalled_native_flush
#include "lancelot_msdc.h"
#undef lancelot_msdc_write_sector
#undef lancelot_msdc_read
#undef lancelot_msdc_flush
#include "lancelot_ext4.h"
#include "lancelot_sha256.h"
static pthread_mutex_t lancelot_mmc_lock=PTHREAD_MUTEX_INITIALIZER;
uint32_t lancelot_mmc_mutex_state(const void *p){assert(p==&lancelot_mmc_lock);return 0x4000002au;}
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
#define lck_mtx_try_lock(p) (pthread_mutex_trylock(p)==0)
#define LCK_MTX_DECLARE(name,group) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
#define SYSCTL_QUAD(...) /* kernel registration modeled */
#define os_atomic_inc_orig(p,order) __atomic_fetch_add((p),1,__ATOMIC_RELAXED)
#define LMD_ORDER_acq_rel memory_order_acq_rel
#define LMD_ORDER_release memory_order_release
#define LMD_ORDER_acquire memory_order_acquire
#define LMD_ORDER_relaxed memory_order_relaxed
#define os_atomic_inc(p,order) atomic_fetch_add_explicit((p),1,LMD_ORDER_##order)
#define os_atomic_add(p,v,order) atomic_fetch_add_explicit((p),(v),LMD_ORDER_##order)
#define os_atomic_store(p,v,order) atomic_store_explicit((p),(v),LMD_ORDER_##order)
#define os_atomic_load(p,order) atomic_load_explicit((p),LMD_ORDER_##order)
#include "lancelot_mmc_observation.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define Z_WAITOK 1
#define Z_ZERO 2
static void*kalloc_data(size_t n,__unused int flags){return calloc(1,n);}
static void kfree_data(void*p,__unused size_t n){free(p);}
static uint8_t *disk;
static uint64_t disk_bytes,ticks;
static struct ext4_fs filesystem;
static struct ext4_inode ios;
static struct lancelot_msdc controller;
static struct {uint64_t partition_offset,partition_bytes;} offer;
static bool ready,failed;
static uint64_t sector_reads,io_errors;
static uint64_t max_read_blocks=128;
static unsigned writes,readbacks,flushes,fail_write,fail_read,corrupt_read;
static bool fail_flush;
static void (*after_sector_write)(unsigned);
static uint64_t mmc_ticks(__unused void*ctx){return ++ticks;}
static uint64_t elapsed_us(uint64_t a,uint64_t b){return b-a;}
static struct lmd_observer mmc_observer;
static void mmc_observe_begin(unsigned operation,uint64_t offset,uint64_t bytes){
 assert(lmd_snapshot(&mmc_observer).operation==LMD_IO_IDLE);
 lmd_begin(&mmc_observer,42,operation,offset,bytes,mmc_ticks(NULL));
}
static void mmc_observe_end(void){lmd_end(&mmc_observer);}
static void mmc_observe_transfer(uint64_t bytes){os_atomic_add(&mmc_observer.transport_bytes,bytes,relaxed);}
void lancelot_mmc_dump_active_io(void){struct lmd_observation v=lmd_snapshot(&mmc_observer);assert(v.consistent);}
static void mmc_report_native_failure(void){
 int unlocked=pthread_mutex_trylock(&lancelot_mmc_lock);assert(!unlocked);
 pthread_mutex_unlock(&lancelot_mmc_lock);
}
static int read_disk(__unused void*ctx,uint64_t offset,size_t size,void*out){
 if(offset>disk_bytes||size>disk_bytes-offset)return 0;memcpy(out,disk+offset,size);return 1;
}
static int lancelot_msdc_write_sector(struct lancelot_msdc*m,uint32_t sector,const uint8_t *bytes){
 assert(m==&controller&&m->media_write_enabled&&m->media_write_first==sector&&m->media_write_end==(uint64_t)sector+1);
 assert((uint64_t)sector*512>=offer.partition_offset);writes++;if(writes==fail_write)return LM_IO;
 uint64_t offset=(uint64_t)sector*512-offer.partition_offset;assert(offset<=disk_bytes-512);
 memcpy(disk+offset,bytes,512);if(after_sector_write)after_sector_write(writes);return LM_OK;
}
static int lancelot_msdc_read(struct lancelot_msdc*m,uint32_t sector,uint32_t count,uint8_t*out,size_t capacity){
 assert(m==&controller&&!m->media_write_enabled&&count>0&&count<=8&&capacity==(size_t)count*512);readbacks++;
 if(readbacks==fail_read)return LM_IO;
 uint64_t offset=(uint64_t)sector*512-offer.partition_offset;assert(offset<=disk_bytes-(uint64_t)count*512);memcpy(out,disk+offset,(size_t)count*512);
 m->last_command=count>1?18:17;m->last_stage=4;m->last_int=LM_DATA_DONE;m->last_response=0x900;
 if(readbacks==corrupt_read)out[0]^=1;return LM_OK;
}
static int lancelot_msdc_flush(struct lancelot_msdc*m){
 assert(m==&controller&&m->media_write_enabled&&!m->media_write_first&&!m->media_write_end);
 flushes++;return fail_flush?LM_IO:LM_OK;
}
typedef void*proc_t;
#define minor(d) ((d)&255u)
#define makedev(a,b) (((a)<<8)|(b))
#define UID_ROOT 0
#define GID_OPERATOR 5
#define DEVFS_BLOCK 0
#define DEVFS_CHAR 1
#define D_DISK 1
#define B_READ 1u
#define B_FUA 2u
struct test_buf{uint32_t count,resid,flags;dev_t device;int64_t blkno;void*data,*upl_data;
 int error,done,map_error,unmap_error,maps,unmaps;bool mapped;};
typedef struct test_buf*buf_t;
#define buf_count(b) ((b)->count)
#define buf_setresid(b,n) ((b)->resid=(n))
#define buf_device(b) ((b)->device)
#define buf_blkno(b) ((b)->blkno)
#define buf_flags(b) ((b)->flags)
#define buf_dataptr(b) ((uintptr_t)(b)->data)
#define buf_resid(b) ((b)->resid)
#define buf_seterror(b,e) ((b)->error=(e))
static int buf_map(buf_t b,caddr_t*out){
 int unlocked=pthread_mutex_trylock(&lancelot_mmc_lock);assert(!unlocked);
 pthread_mutex_unlock(&lancelot_mmc_lock);b->maps++;
 if(b->map_error)return b->map_error;
 *out=b->data?b->data:b->upl_data;b->mapped=true;return 0;
}
static int buf_unmap(buf_t b){assert(b->mapped);b->unmaps++;b->mapped=false;return b->unmap_error;}
static void buf_biodone(buf_t b){assert(!b->mapped);b->done++;}
enum {DKIOCGETBLOCKSIZE=1,DKIOCSETBLOCKSIZE,DKIOCGETPHYSICALBLOCKSIZE,DKIOCGETBLOCKCOUNT,DKIOCISWRITABLE,
 DKIOCGETMAXBLOCKCOUNTREAD,DKIOCGETMAXBLOCKCOUNTWRITE,DKIOCGETFEATURES,DKIOCSYNCHRONIZE,
 DKIOCGETMAXBYTECOUNTREAD,DKIOCGETMAXBYTECOUNTWRITE,DKIOCGETMAXSEGMENTCOUNTREAD,DKIOCGETMAXSEGMENTCOUNTWRITE,
 DKIOCGETMAXSEGMENTBYTECOUNTREAD,DKIOCGETMAXSEGMENTBYTECOUNTWRITE,DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT,
 DKIOCGETCOMMANDPOOLSIZE,DKIOCISSOLIDSTATE,DKIOCISVIRTUAL};
typedef ssize_t user_ssize_t;
struct uio{int64_t offset;ssize_t resid;uint8_t*cursor;bool output,fault;};
#define uio_offset(u) ((u)->offset)
#define uio_resid(u) ((u)->resid)
static struct uio*uio_duplicate(struct uio*u){struct uio*c=malloc(sizeof(*c));if(c)*c=*u;return c;}
static void uio_free(struct uio*u){free(u);}
static void uio_update(struct uio*u,user_ssize_t n){assert(n>=0&&n<=u->resid);u->offset+=n;u->resid-=n;u->cursor+=n;}
static int uiomove(caddr_t bytes,int n,struct uio*u){
 assert(n>=0&&n<=u->resid);if(u->fault)return EFAULT;
 if(u->output)memcpy(u->cursor,bytes,(size_t)n);else memcpy(bytes,u->cursor,(size_t)n);
 uio_update(u,n);return 0;
}
struct bdevsw{int(*d_open)(dev_t,int,int,proc_t);int(*d_close)(dev_t,int,int,proc_t);void(*d_strategy)(buf_t);
 int(*d_ioctl)(dev_t,u_long,caddr_t,int,proc_t);void*d_dump;int(*d_psize)(dev_t);int d_type;};
struct cdevsw{int(*d_open)(dev_t,int,int,proc_t);int(*d_close)(dev_t,int,int,proc_t);
 int(*d_read)(dev_t,struct uio*,int);int(*d_write)(dev_t,struct uio*,int);int(*d_ioctl)(dev_t,u_long,caddr_t,int,proc_t);
 void*d_stop,*d_reset,*d_ttys,*d_select,*d_mmap,*d_strategy,*d_reserved_1,*d_reserved_2;int d_type;};
#define eno_dump NULL
#define eno_stop NULL
#define eno_reset NULL
#define eno_select NULL
#define eno_mmap NULL
#define eno_strat NULL
#define eno_getc NULL
#define eno_putc NULL
static int bdevsw_add(__unused int n,__unused const struct bdevsw*s){return 2;}
static int cdevsw_add_with_bdev(__unused int n,__unused const struct cdevsw*s,__unused int b){return 3;}
static void*devfs_make_node(__unused dev_t d,__unused int kind,__unused int uid,__unused int gid,__unused int mode,__unused const char*name){return (void*)1;}
static int PE_parse_boot_argn(__unused const char*n,__unused void*p,__unused size_t s){return 0;}
#include "lancelot_data_disk.inc"
static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t*p,uint32_t v){put16(p,(uint16_t)v);put16(p+2,(uint16_t)(v>>16));}
static void put64(uint8_t*p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static void be32(uint8_t*p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void be64(uint8_t*p,uint64_t v){be32(p,(uint32_t)(v>>32));be32(p+4,(uint32_t)v);}
static uint8_t map_bytes[LHD_MAP_HEADER+2*LHD_MAP_ENTRY];
static void extent(uint8_t*p,unsigned depth,uint64_t physical,unsigned count){
 memset(p,0,60);put16(p,0xf30a);put16(p+2,1);put16(p+4,4);put16(p+6,(uint16_t)depth);
 if(depth){put32(p+16,(uint32_t)physical);put16(p+20,(uint16_t)(physical>>32));}
 else{put16(p+16,(uint16_t)count);put16(p+18,(uint16_t)(physical>>32));put32(p+20,(uint32_t)physical);}
}
static void reset_io(void){
 failed=data_failed=false;data_ready=true;ready=true;writes=readbacks=flushes=0;fail_write=fail_read=corrupt_read=0;fail_flush=false;after_sector_write=NULL;
 controller.media_write_enabled=false;controller.media_write_first=controller.media_write_end=0;data_block_size=512;
 controller.recovery_status=LM_OK;max_read_blocks=128;
}
static void setup(void){
 disk_bytes=UINT64_C(134217728);disk=calloc(1,(size_t)disk_bytes);assert(disk);
 offer.partition_offset=UINT64_C(8192);offer.partition_bytes=disk_bytes;
 filesystem=(struct ext4_fs){.read=read_disk,.bytes=disk_bytes,.blocks=disk_bytes/4096,.block_size=4096,
  .blocks_per_group=32768,.inodes_per_group=128,.inode_size=256,.desc_size=32,.descriptors=4096};
 put32(disk+4096,2);put32(disk+4100,3);put32(disk+4104,4);put32(disk+1124,1);
 data_image=(struct ext4_inode){.mode=0100600,.flags=0x80000,.size=UINT64_C(67108864)};
 extent(data_image.extents,0,4096,16384);data_bytes=data_image.size;
 memcpy(map_bytes,"LHDATA1\0",8);put32(map_bytes+8,1);put32(map_bytes+12,4096);put64(map_bytes+16,data_bytes);
 put32(map_bytes+24,1);put64(map_bytes+32,0);put64(map_bytes+40,4096);put64(map_bytes+48,16384);
 assert(lhd_map_open(&data_map,map_bytes,56,4096,filesystem.blocks,data_bytes));reset_io();
}
static void test_guard(void){
 struct lhd_walk*w=calloc(1,sizeof(*w));uint64_t nodes=0,groups=0;assert(w);
 assert(lhd_metadata_safe(&filesystem,&data_map,&groups)&&groups==1);
 assert(lhd_initialized_tree(&filesystem,&data_image,&data_map,w,&nodes)&&nodes==0);
 /* Real phones can have more than one FIEMAP page of physical ranges. */
 uint8_t *many=calloc(1,LHD_MAP_HEADER+6000*LHD_MAP_ENTRY);assert(many);
 memcpy(many,map_bytes,LHD_MAP_HEADER);put32(many+24,6000);
 for(unsigned i=0;i<6000;i++){uint8_t*p=many+32+24*i;put64(p,i);put64(p+8,1+2*i);put64(p+16,i==5999?16384-5999:1);}
 struct lhd_map large;uint64_t mapped;
 assert(lhd_map_open(&large,many,32+6000*24,4096,filesystem.blocks,data_bytes));
 assert(lhd_map_block(&large,16383,&mapped)&&mapped==22383);free(many);
 extent(disk+30*4096,0,4096,16384);extent(data_image.extents,1,30,0);
 assert(lhd_initialized_tree(&filesystem,&data_image,&data_map,w,&nodes)&&nodes==1);
 extent(data_image.extents,1,4096,0);assert(!lhd_initialized_tree(&filesystem,&data_image,&data_map,w,0));
 extent(data_image.extents,0,4096,16384);put16(data_image.extents+16,0xc000);
 assert(!lhd_initialized_tree(&filesystem,&data_image,&data_map,w,0));
 extent(data_image.extents,0,4096,16384);put32(data_image.extents+12,1);assert(!lhd_initialized_tree(&filesystem,&data_image,&data_map,w,0));
 extent(data_image.extents,0,4096,16384);put32(disk+4096,5000);assert(!lhd_metadata_safe(&filesystem,&data_map,0));put32(disk+4096,2);
 uint8_t saved[80];memcpy(saved,map_bytes,sizeof(saved));struct lhd_map invalid;
 put32(map_bytes+24,2);put64(map_bytes+48,8192);put64(map_bytes+56,8192);put64(map_bytes+64,4096);put64(map_bytes+72,8192);
 assert(!lhd_map_open(&invalid,map_bytes,80,4096,filesystem.blocks,data_bytes));memcpy(map_bytes,saved,sizeof(saved));
 uint8_t vh[512]={0},alt[512];vh[0]='H';vh[1]='X';vh[3]=5;be32(vh+4,1u<<13);be32(vh+12,13);
 be32(vh+40,4096);be32(vh+44,16384);be32(vh+48,15000);be32(vh+52,100);
 for(unsigned i=0;i<3;i++){uint8_t*p=vh+112+80*i;be64(p,i==0?2048:(i==1?8192:16384));be32(p+12,i==0?1:(i==1?2:4));be32(p+16,14+4*i);be32(p+20,i==0?1:(i==1?2:4));}
 memcpy(alt,vh,512);assert(lhd_hfs_geometry(vh,alt,data_bytes));
 be32(vh+48,20000);assert(!lhd_hfs_geometry(vh,alt,data_bytes));be32(vh+48,15000);
 be32(vh+40,512);assert(!lhd_hfs_geometry(vh,alt,data_bytes));be32(vh+40,4096);
 be32(vh+112+16,16384);assert(!lhd_hfs_geometry(vh,alt,data_bytes));free(w);
 puts("PASS actual admission: full extent tree, unwritten/hole/metadata/overlap rejects and HFSX geometry");
}
static void test_io(void){
 uint8_t bytes[4096],out[4096];for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(uint8_t)(i*13+7);size_t done=777;
 assert(!data_file_write(3584,sizeof(bytes),bytes,&done)&&done==sizeof(bytes)&&writes==8&&readbacks==8&&!controller.media_write_enabled);
 assert(!data_file_read(3584,sizeof(out),out)&&!memcmp(bytes,out,sizeof(out)));
 unsigned before=writes;assert(data_file_write(1,512,bytes,&done)==EINVAL&&!done&&writes==before);
 assert(data_file_write(data_bytes-512,1024,bytes,&done)==EINVAL&&!done&&writes==before);
 reset_io();put32(data_image.extents+20,4000);assert(data_file_write(0,512,bytes,&done)==EIO&&!done&&!writes&&data_failed);
 extent(data_image.extents,0,4096,16384);reset_io();fail_write=2;
 assert(data_file_write(0,1536,bytes,&done)==EIO&&done==512&&writes==2&&readbacks==1&&data_failed&&!controller.media_write_enabled);
 before=writes;assert(data_file_write(4096,512,bytes,&done)==ENXIO&&!done&&writes==before);
 reset_io();corrupt_read=1;assert(data_file_write(0,512,bytes,&done)==EIO&&!done&&data_failed);
 reset_io();struct test_buf b={.count=1536,.device=1,.data=bytes};fail_write=2;data_strategy(&b);
 assert(b.done==1&&b.error==EIO&&b.resid==1024);
 reset_io();b=(struct test_buf){.count=512,.device=1,.data=bytes,.flags=B_FUA};data_strategy(&b);
 assert(b.done==1&&!b.error&&!b.resid&&flushes==1);
 reset_io();b=(struct test_buf){.count=4096,.device=1,.upl_data=bytes};data_strategy(&b);
 assert(b.done==1&&!b.error&&!b.resid&&b.maps==1&&b.unmaps==1&&writes==8);
 memset(out,0,sizeof(out));b=(struct test_buf){.count=4096,.device=1,.upl_data=out,.flags=B_READ};data_strategy(&b);
 assert(b.done==1&&!b.error&&!b.resid&&b.maps==1&&b.unmaps==1&&!memcmp(bytes,out,sizeof(out)));
 reset_io();b=(struct test_buf){.count=512,.device=1,.upl_data=bytes,.map_error=ENOMEM};data_strategy(&b);
 assert(b.done==1&&b.error==ENOMEM&&b.resid==512&&b.maps==1&&!b.unmaps&&!writes);
 reset_io();b=(struct test_buf){.count=512,.device=1};data_strategy(&b);
 assert(b.done==1&&b.error==EIO&&b.resid==512&&b.maps==1&&b.unmaps==1&&!writes);
 reset_io();b=(struct test_buf){.count=512,.device=1,.upl_data=bytes,.unmap_error=EINVAL};data_strategy(&b);
 assert(b.done==1&&b.error==EINVAL&&b.maps==1&&b.unmaps==1&&writes==1);
 reset_io();struct uio u={.offset=0,.resid=1536,.cursor=bytes};fail_write=2;
 assert(data_cwrite(1,&u,0)==EIO&&u.offset==512&&u.resid==1024&&u.cursor==bytes+512);
 reset_io();u=(struct uio){.offset=0,.resid=512,.cursor=bytes,.fault=true};assert(data_cwrite(1,&u,0)==EFAULT&&u.offset==0&&u.resid==512&&!writes);
 reset_io();uint32_t features=99;assert(!data_ioctl(1,DKIOCGETFEATURES,(caddr_t)&features,0,0)&&features==0);
 assert(data_ioctl(0,DKIOCGETFEATURES,(caddr_t)&features,0,0)==ENXIO);
 assert(data_size(1)==(int)(data_bytes/512));assert(!data_flush()&&flushes==1&&!controller.media_write_enabled);
 fail_flush=true;assert(data_flush()==EIO&&data_failed&&!controller.media_write_enabled);
#ifndef LANCELOT_OLD_NO_DIAG
 lancelot_data_startup_diagnostics();
 pthread_mutex_lock(&lancelot_mmc_lock);lancelot_data_startup_diagnostics();pthread_mutex_unlock(&lancelot_mmc_lock);
#endif
 puts("PASS actual write/strategy/raw paths: sealed LBA, readback, UPL mapping outside MMC lock, unmapped completion, map failure, partial residuals, stopped writer, FUA/full flush and UIO copy failure");
}
#ifndef LANCELOT_OLD_NO_DIAG
static struct lmd_observer concurrent_observer;
static void *observation_writer(void *arg){
 (void)arg;
 for(uint64_t i=1;i<20000;i++){lmd_begin(&concurrent_observer,42,LMD_IO_SYSTEM_READ,i,i+4096,i+100);
  os_atomic_store(&concurrent_observer.transport_bytes,i,relaxed);lmd_end(&concurrent_observer);}
 return NULL;
}
static void test_observation(void){
 assert(lmd_snapshot(&mmc_observer).operation==LMD_IO_IDLE);
 pthread_t writer;assert(!pthread_create(&writer,NULL,observation_writer,NULL));
 for(unsigned i=0;i<50000;i++){
  struct lmd_observation v=lmd_snapshot(&concurrent_observer);
  if(v.consistent&&v.operation){assert(v.owner==42&&v.operation==LMD_IO_SYSTEM_READ);
   assert(v.bytes==v.offset+4096&&v.began==v.offset+100);}
 }
 assert(!pthread_join(writer,NULL));assert(lmd_snapshot(&concurrent_observer).operation==LMD_IO_IDLE);
 puts("PASS actual atomic I/O observation: concurrent stable owner/request snapshots and balanced read/write/flush paths");
}
#endif
int main(int argc,char**argv){
 setup();test_guard();test_io();
#ifndef LANCELOT_OLD_NO_DIAG
 test_observation();
#endif
 if(argc==2){FILE*f=fopen(argv[1],"rb");uint8_t vh[512],alt[512];assert(f);
  assert(!fseeko(f,0,SEEK_END));off_t size=ftello(f);assert(size>0);
  assert(!fseeko(f,1024,SEEK_SET)&&fread(vh,1,512,f)==512);
  assert(!fseeko(f,size-1024,SEEK_SET)&&fread(alt,1,512,f)==512);
  assert(lhd_hfs_geometry(vh,alt,(uint64_t)size));fclose(f);puts("PASS actual host mkfs image geometry");}
 free(disk);return 0;
}
