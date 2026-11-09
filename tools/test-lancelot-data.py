"""Validate real grouped writes, 4K geometry, reader access and flush ordering."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,shutil,subprocess,sys
P=Path(__file__).resolve().parent;R=P.parent
if os.name=='nt':
    path='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/' + Path(__file__).name
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',path]))
E=R/'out/lancelot/tests';E.mkdir(parents=True,exist_ok=True)
O=E/('data-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=['lancelot_data_disk.inc','lancelot_data_guard.h','lancelot_ext4.h','lancelot_sha256.h','lancelot_msdc.h','lancelot_mmc_observation.h']
sources={}
for name in names:
    source=R/'xnu/files/bsd/dev'/name;shutil.copy2(source,O/name);sources[source.relative_to(R).as_posix()]=sha(source)
fixture=R/'tools/lancelot-data/tests/backend_test.c'
sources[fixture.relative_to(R).as_posix()]=sha(fixture)
baseline=R/'tools/lancelot-data/tests/data_write_before_snapshot.inc'
sources[baseline.relative_to(R).as_posix()]=sha(baseline)
shutil.copy2(baseline,O/baseline.name)
sources[Path(__file__).relative_to(R).as_posix()]=sha(Path(__file__))
text=fixture.read_text(encoding='utf-8')
anchor='#define lck_mtx_lock pthread_mutex_lock\n#define lck_mtx_unlock pthread_mutex_unlock\n#define lck_mtx_try_lock(p) (pthread_mutex_trylock(p)==0)'
assert text.count(anchor)==1
text=text.replace(anchor,r'''
static _Thread_local unsigned owned_controller;
static _Thread_local unsigned owned_request;
static pthread_mutex_t lancelot_data_request_lock;
static bool allocation_failure;
static void (*lock_observer)(pthread_mutex_t *),(*unlock_observer)(pthread_mutex_t *);
static int model_lock(pthread_mutex_t *lock){
 if(lock_observer)lock_observer(lock);
 int error=pthread_mutex_lock(lock);assert(!error);
 if(lock==&lancelot_mmc_lock)owned_controller++;
 if(lock==&lancelot_data_request_lock)owned_request++;
 return error;
}
static int model_unlock(pthread_mutex_t *lock){
 if(lock==&lancelot_mmc_lock){assert(owned_controller==1);owned_controller--;}
 if(lock==&lancelot_data_request_lock){assert(owned_request==1);owned_request--;}
 int error=pthread_mutex_unlock(lock);assert(!error);
 if(unlock_observer)unlock_observer(lock);
 return error;
}
static bool model_try_lock(pthread_mutex_t *lock){
 int error=pthread_mutex_trylock(lock);assert(!error||error==EBUSY);
 if(!error&&lock==&lancelot_mmc_lock)owned_controller++;
 return !error;
}
#define lck_mtx_lock model_lock
#define lck_mtx_unlock model_unlock
#define lck_mtx_try_lock model_try_lock
''')
allocator='static void*kalloc_data(size_t n,__unused int flags){return calloc(1,n);}'
assert text.count(allocator)==1
text=text.replace(allocator,'static void*kalloc_data(size_t n,__unused int flags){assert(!owned_controller&&!owned_request);return allocation_failure?NULL:calloc(1,n);}')
anchor=' int unlocked=pthread_mutex_trylock(&lancelot_mmc_lock);assert(!unlocked);\n pthread_mutex_unlock(&lancelot_mmc_lock);'
# The reporter checks ownership by its caller, including when another thread
# legitimately owns the shared controller during concurrent testing.
report_start=text.index('static void mmc_report_native_failure(void){')
report_end=text.index('\n}',report_start)+2
text=text[:report_start]+'static void mmc_report_native_failure(void){assert(!owned_controller);}' +text[report_end:]
anchor='static bool fail_flush;'
assert text.count(anchor)==1
text=text.replace(anchor,anchor+'\nstatic size_t corrupt_offset;\nstatic void (*flush_observer)(void);')
text=text.replace('if(readbacks==corrupt_read)out[0]^=1;return LM_OK;',
                  'if(readbacks==corrupt_read){assert(corrupt_offset<capacity);out[corrupt_offset]^=1;}return LM_OK;')
text=text.replace(' flushes++;return fail_flush?LM_IO:LM_OK;',
                  ' if(flush_observer)flush_observer();flushes++;return fail_flush?LM_IO:LM_OK;')
extension=r'''
static pthread_mutex_t schedule_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_changed=PTHREAD_COND_INITIALIZER;
static bool paused_writer,resume_writer,flush_waiting;
static _Thread_local bool flush_thread_tag;
static uint8_t *large_input;
static size_t large_size,large_done;
static int writer_error,flush_error;
static void before_driver_lock(pthread_mutex_t *lock){
 if(lock!=&lancelot_data_request_lock||!flush_thread_tag)return;
 assert(!pthread_mutex_lock(&schedule_lock));flush_waiting=true;
 assert(!pthread_cond_broadcast(&schedule_changed));assert(!pthread_mutex_unlock(&schedule_lock));
}
static void after_driver_unlock(pthread_mutex_t *lock){
 if(lock!=&lancelot_mmc_lock||flush_thread_tag)return;
 assert(!pthread_mutex_lock(&schedule_lock));
 if(!paused_writer){
  paused_writer=true;assert(!pthread_cond_broadcast(&schedule_changed));
  while(!resume_writer)assert(!pthread_cond_wait(&schedule_changed,&schedule_lock));
 }
 assert(!pthread_mutex_unlock(&schedule_lock));
}
static void verify_flush_order(void){assert(large_done==large_size);}
static void *write_large(void *unused){(void)unused;writer_error=data_file_write(0,large_size,large_input,&large_done);return NULL;}
static void *flush_large(void *unused){(void)unused;flush_thread_tag=true;flush_error=data_flush();return NULL;}
static void test_groups(void){
 uint8_t input[8192],output[8192];for(unsigned i=0;i<sizeof(input);i++)input[i]=(uint8_t)(i*31+9);
 size_t done=0;reset_io();corrupt_offset=0;
 assert(!data_file_write(0,sizeof(input),input,&done)&&done==sizeof(input)&&writes==16&&readbacks==2);
 assert(!data_file_read(0,sizeof(output),output)&&!memcmp(input,output,sizeof(input)));
 reset_io();max_read_blocks=1;
 assert(!data_file_write(0,sizeof(input),input,&done)&&done==sizeof(input)&&writes==16&&readbacks==16);
 reset_io();fail_write=10;
 assert(data_file_write(0,sizeof(input),input,&done)==EIO&&done==4608&&writes==10&&readbacks==2&&!controller.media_write_enabled);
 reset_io();fail_write=10;controller.recovery_status=LM_IO;
 assert(data_file_write(0,sizeof(input),input,&done)==EIO&&done==4096&&writes==10&&readbacks==1);
 reset_io();corrupt_read=2;corrupt_offset=1024;
 assert(data_file_write(0,sizeof(input),input,&done)==EIO&&done==5120&&writes==16&&readbacks==2);
 corrupt_offset=0;reset_io();fail_read=1;
 assert(data_file_write(0,sizeof(input),input,&done)==EIO&&!done&&writes==8&&readbacks==1);
 reset_io();unsigned before=writes;
 assert(data_file_write(data_bytes-4096,8192,input,&done)==EINVAL&&!done&&writes==before);
}
static void test_geometry(void){
 reset_io();data_block_size=4096;uint32_t size=0;uint64_t count=0;
 assert(!data_ioctl(1,DKIOCGETBLOCKSIZE,(caddr_t)&size,0,0)&&size==4096);
 assert(!data_ioctl(1,DKIOCGETPHYSICALBLOCKSIZE,(caddr_t)&size,0,0)&&size==4096);
 assert(!data_ioctl(1,DKIOCGETBLOCKCOUNT,(caddr_t)&count,0,0)&&count*4096==data_bytes);
 /* HFS's initial header/geometry probe temporarily requests 512-byte units. */
 size=512;assert(!data_ioctl(1,DKIOCSETBLOCKSIZE,(caddr_t)&size,0,0));
 assert(!data_ioctl(1,DKIOCGETBLOCKCOUNT,(caddr_t)&count,0,0)&&count*512==data_bytes);
 size=4096;assert(!data_ioctl(1,DKIOCSETBLOCKSIZE,(caddr_t)&size,0,0));
 uint8_t input[4096],output[4096];for(unsigned i=0;i<sizeof(input);i++)input[i]=(uint8_t)(i+77);
 struct test_buf b={.count=4096,.device=1,.blkno=7,.upl_data=input};data_strategy(&b);
 assert(!b.error&&!b.resid&&b.done==1&&writes==8&&readbacks==1);
 assert(!data_file_read(7*4096,sizeof(output),output)&&!memcmp(input,output,sizeof(input)));
 uint8_t *physical=disk+4096*4096+7*4096;
 assert(!memcmp(physical,input,sizeof(input)));
 /* Alternate volume header lives 1024 bytes before the virtual disk's end. */
 uint8_t header[512];memset(header,0x78,sizeof(header));size_t done=0;
 assert(!data_file_write(data_bytes-1024,sizeof(header),header,&done)&&done==512);
 b=(struct test_buf){.count=4096,.device=1,.blkno=(int64_t)(data_bytes/4096-1),.upl_data=output,.flags=B_READ};
 data_strategy(&b);assert(!b.error&&!memcmp(output+3072,header,sizeof(header)));
 size=4*1024*1024;assert(data_ioctl(1,DKIOCSETBLOCKSIZE,(caddr_t)&size,0,0)==EINVAL);
}
static void test_fragmented(void){
 reset_io();data_image=(struct ext4_inode){.mode=0100600,.flags=0x80000,.size=data_bytes};
 extent(data_image.extents,0,4096,1);put16(data_image.extents+2,2);
 put32(data_image.extents+24,1);put16(data_image.extents+28,16383);put16(data_image.extents+30,0);put32(data_image.extents+32,8192);
 put32(map_bytes+24,2);put64(map_bytes+48,1);put64(map_bytes+56,1);put64(map_bytes+64,8192);put64(map_bytes+72,16383);
 assert(lhd_map_open(&data_map,map_bytes,80,4096,filesystem.blocks,data_bytes));
 uint8_t input[8192];memset(input,0x6a,sizeof(input));size_t done=0;
 assert(!data_file_write(0,sizeof(input),input,&done)&&done==8192&&writes==16&&readbacks==2);
 assert(!memcmp(disk+4096*4096,input,4096)&&!memcmp(disk+8192*4096,input+4096,4096));
 data_image=(struct ext4_inode){.mode=0100600,.flags=0x80000,.size=data_bytes};extent(data_image.extents,0,4096,16384);
 put32(map_bytes+24,1);put64(map_bytes+48,16384);assert(lhd_map_open(&data_map,map_bytes,56,4096,filesystem.blocks,data_bytes));
}
static void test_concurrent(void){
 reset_io();large_size=131072;large_done=0;large_input=malloc(large_size);assert(large_input);
 for(size_t i=0;i<large_size;i++)large_input[i]=(uint8_t)(i*7+33);
 paused_writer=resume_writer=flush_waiting=false;writer_error=flush_error=-1;
 lock_observer=before_driver_lock;unlock_observer=after_driver_unlock;flush_observer=verify_flush_order;
 pthread_t writer,flusher;assert(!pthread_create(&writer,NULL,write_large,NULL));
 assert(!pthread_mutex_lock(&schedule_lock));
 while(!paused_writer)assert(!pthread_cond_wait(&schedule_changed,&schedule_lock));
 assert(!pthread_mutex_unlock(&schedule_lock));
 assert(writes==8&&readbacks==1&&large_done==4096);
 assert(!pthread_create(&flusher,NULL,flush_large,NULL));
 assert(!pthread_mutex_lock(&schedule_lock));
 while(!flush_waiting)assert(!pthread_cond_wait(&schedule_changed,&schedule_lock));
 assert(!pthread_mutex_unlock(&schedule_lock));
 /* The writer is paused outside the controller lock but still owns the
  * request-order lock. A foreground read completes; flush cannot overtake. */
 unlock_observer=NULL;uint8_t output[512];assert(!data_file_read(0,sizeof(output),output));
 assert(!memcmp(output,large_input,sizeof(output))&&!flushes);
 assert(!pthread_mutex_lock(&schedule_lock));resume_writer=true;
 assert(!pthread_cond_broadcast(&schedule_changed));assert(!pthread_mutex_unlock(&schedule_lock));
 assert(!pthread_join(writer,NULL)&&!pthread_join(flusher,NULL));
 lock_observer=NULL;flush_observer=NULL;
 assert(!writer_error&&!flush_error&&large_done==large_size&&writes==256&&readbacks==32&&flushes==1);
 printf("128 KiB production request: CMD24 sectors=%u verification reads=%u (legacy=256); foreground read completed before writer finished; flush stayed ordered\n",writes,readbacks);
 free(large_input);large_input=NULL;
}
#include "data_write_before_snapshot.inc"
static uint8_t *mutable_caller;
static void change_written_source(unsigned completed){if(completed==2)mutable_caller[512]^=1;}
static void test_snapshot(void){
 uint8_t input[16384],expected[16384],output[16384];
 for(unsigned i=0;i<sizeof(input);i++)expected[i]=(uint8_t)(i*19+7);
 size_t done=0;
 reset_io();memcpy(input,expected,sizeof(input));mutable_caller=input;after_sector_write=change_written_source;
 assert(legacy_data_file_write(0,sizeof(input),input,&done)==EIO&&done==512&&writes==8&&readbacks==1&&data_failed);
 assert(controller.last_command==18&&controller.last_stage==4&&controller.last_int==0x1000&&controller.last_response==0x900);
 reset_io();memcpy(input,expected,sizeof(input));mutable_caller=input;after_sector_write=change_written_source;
 assert(!data_file_write(0,sizeof(input),input,&done)&&done==sizeof(input)&&!data_failed);
 assert(input[512]!=expected[512]);after_sector_write=NULL;
 assert(!data_file_read(0,sizeof(output),output)&&!memcmp(output,expected,sizeof(output)));
 reset_io();corrupt_read=1;corrupt_offset=512;
 assert(data_file_write(0,sizeof(input),expected,&done)==EIO&&done==512&&data_failed);
 reset_io();allocation_failure=true;
 assert(data_file_write(0,4096,expected,&done)==ENOMEM&&!done&&!writes&&!data_failed);
 allocation_failure=false;
 assert(data_file_write(0,LANCELOT_DATA_SNAPSHOT_MAX+512,expected,&done)==EINVAL&&!done&&!writes);
 puts("PASS R16 failure signature reproduced under mutable-source change: completed CMD18 plus 512 verified bytes; stable snapshot succeeds; genuine corruption still stops I/O; allocation outside both locks");
}
static void optimization_tests(void){test_groups();test_geometry();test_fragmented();test_concurrent();test_snapshot();}
'''
anchor='int main(int argc,char**argv){';assert text.count(anchor)==1
text=text.replace(anchor,extension+'\n'+anchor,1)
anchor='setup();test_guard();test_io();';assert text.count(anchor)==1
text=text.replace(anchor,anchor+'optimization_tests();',1)
unit=O/'test.c';unit.write_text(text,encoding='utf-8')
runs=[];passed=True
for cc in ['gcc','clang']:
    exe=O/cc
    for label,cmd in [(cc+'-build',[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation','-Wno-format','-fsanitize=address,undefined','-pthread','-I'+str(O),unit,'-o',exe]),(cc+'-run',[exe])]:
        p=subprocess.run(list(map(str,cmd)),capture_output=True,timeout=90)
        log=O/(label+'.log');log.write_bytes(p.stdout+p.stderr)
        runs.append({'name':label,'exit':p.returncode,'log':log.name,'sha256':sha(log)})
        print(label,p.returncode,(p.stdout+p.stderr).decode(errors='replace')[-2300:],flush=True)
        if p.returncode:passed=False;break
    if not passed:break
assert all(sha(R/n)==h for n,h in sources.items())
with (O/'result.json').open('x',encoding='utf-8') as f:
    json.dump({'passed':passed and len(runs)==4,'sources':sources,'runs':runs,'actual_data_backend':True,
        'guard_and_ext4_mapper_exercised':True,'controller_registers_and_kernel_objects_modeled':True,
        'logical_block_bytes':4096,'verification_chunk_bytes':4096,
        'verification_calls_for_128KiB':32,'legacy_verification_calls_for_128KiB':256,
        'foreground_reader_and_flush_order_verified':passed,
        'old_mutable_source_writeback_failure_reproduced':passed,
        'immutable_snapshot_roundtrip_verified':passed,
        'genuine_readback_corruption_stops_io':passed,
        'snapshot_allocation_outside_storage_locks':passed,
        'allocation_failure_does_not_stop_io':passed,
        'snapshot_max_bytes':131072,
        'device_access':False,'GPU_complete':False},f,indent=2)
print('4K IO TESTS',O,flush=True);raise SystemExit(0 if passed and len(runs)==4 else 1)
