/* SPDX-License-Identifier: APSL-2.0
 * Persistent HFS+ file on the existing Ubuntu ext4 filesystem. No outer
 * filesystem metadata is written by XNU. Every sector is resolved from a
 * fully initialized extent of one SHA-bound fixed file before issuing CMD24.
 */
#include "lancelot_data_guard.h"
#include "lancelot_mmc_io_limits.h"
static struct ext4_inode data_image;
static bool data_ready,data_failed;
static uint64_t data_bytes,data_writes,data_write_bytes,data_flushes,data_write_us,data_guard_groups,data_guard_nodes;
static struct lhd_map data_map;
static uint8_t *data_map_raw;
static size_t data_map_size;
#define LANCELOT_DATA_VERIFY_BYTES 4096u
#define LANCELOT_DATA_SNAPSHOT_MAX LANCELOT_MMC_REQUEST_BYTES
static uint32_t data_block_size=4096;
static uint32_t data_strategy_diagnostics;
static uint8_t data_readback[LANCELOT_DATA_VERIFY_BYTES] __attribute__((aligned(64)));
/* Serializes a complete data write/flush while the shared controller can be
 * released between verified chunks for system-image and data reads. */
static LCK_MTX_DECLARE(lancelot_data_request_lock,&lancelot_mmc_group);
#define LANCELOT_DATA_FILE "/hackios/20H392/ios-data.hfs"
#define LANCELOT_DATA_MAP "/hackios/20H392/ios-data.map"
SYSCTL_QUAD(_kern_redwood,OID_AUTO,lancelot_data_bytes,CTLFLAG_RD|CTLFLAG_LOCKED,&data_bytes,"validated persistent iOS data device bytes");
SYSCTL_QUAD(_kern_redwood,OID_AUTO,lancelot_data_writes,CTLFLAG_RD|CTLFLAG_LOCKED,&data_writes,"readback-verified data sector writes");
SYSCTL_QUAD(_kern_redwood,OID_AUTO,lancelot_data_write_bytes,CTLFLAG_RD|CTLFLAG_LOCKED,&data_write_bytes,"readback-verified data bytes");
SYSCTL_QUAD(_kern_redwood,OID_AUTO,lancelot_data_flushes,CTLFLAG_RD|CTLFLAG_LOCKED,&data_flushes,"completed eMMC cache flushes");
SYSCTL_QUAD(_kern_redwood,OID_AUTO,lancelot_data_write_us,CTLFLAG_RD|CTLFLAG_LOCKED,&data_write_us,"data write and readback wall microseconds");

void lancelot_data_startup_diagnostics(void);
extern uint32_t lancelot_mmc_mutex_state(const void *);
uint32_t lancelot_data_current_mutex_state(void);
uint32_t lancelot_data_current_mutex_state(void) {
    return lancelot_mmc_mutex_state(&lancelot_mmc_lock);
}
void
lancelot_data_startup_diagnostics(void)
{
    lancelot_mmc_dump_active_io();
    /* This observer never waits behind a possibly stalled storage request. */
    if(!lck_mtx_try_lock(&lancelot_mmc_lock)) {
        /* Pinned ARM64 XNU's generic mutex state uses a 28-bit compact thread
         * ID. Observe it without decoding/dereferencing a thread pointer. */
        uint32_t state=lancelot_mmc_mutex_state(&lancelot_mmc_lock);
        printf("lancelot-data-startup: MMC_mutex_busy=1 counters_not_sampled=1 lock_state=%#x "
            "owner_compact_tid=%u one_shot=1 owner_may_change=1\n",state,state&0x0fffffffu);
        return;
    }
    uint64_t writes=data_writes,bytes=data_write_bytes,flushes=data_flushes,write_us=data_write_us;
    uint64_t reads=sector_reads,errors=io_errors,commands=controller.commands_sent;
    bool available=data_ready,broken=data_failed,system_broken=failed;
    lck_mtx_unlock(&lancelot_mmc_lock);
    printf("lancelot-data-startup: MMC_mutex_busy=0 ready=%d data_failed=%d system_failed=%d "
        "writes=%llu write_bytes=%llu flushes=%llu write_us=%llu read_sectors=%llu "
        "read_errors=%llu commands=%llu one_shot=1\n",
        available,broken,system_broken,writes,bytes,flushes,write_us,reads,errors,commands);
}

static int
data_file_read(uint64_t offset,size_t size,void *out)
{
    lck_mtx_lock(&lancelot_mmc_lock);
    mmc_observe_begin(LMD_IO_DATA_READ,offset,size);
    int result=data_ready&&!data_failed&&!failed&&offset<=data_bytes&&size<=data_bytes-offset&&
        ext4_read_file(&filesystem,&data_image,offset,size,out)?0:EIO;
    os_atomic_store(&mmc_observer.phase,LMD_PHASE_FILE_DONE,relaxed);
    mmc_observe_end();lck_mtx_unlock(&lancelot_mmc_lock);
    mmc_report_native_failure();return result;
}
/* The fixed inode/extent map is immutable. Each chunk stays in one proved
 * backing block. Native writes retain the exact one-sector CMD24 gate; a
 * complete contiguous chunk is verified with one counted read. */
static uint32_t data_snapshot_hash(const uint8_t *bytes,size_t count) {
    uint32_t hash=2166136261u;
    for(size_t i=0;i<count;i++)hash=(hash^bytes[i])*16777619u;
    return hash;
}
static int
data_file_write(uint64_t offset,size_t size,const void *buffer,size_t *transferred)
{
    if(transferred)*transferred=0;
    if(!buffer || (offset&511) || (size&511) ||
       offset>data_bytes || size>data_bytes-offset || size>LANCELOT_DATA_SNAPSHOT_MAX)return EINVAL;
    if(!size)return 0;
    /* A mapped HFS/UPL page may be changed through another CPU alias while
     * PIO or readback runs. Verify the exact bytes submitted, not a later
     * version of the caller's memory. Allocation/copy can fault and must
     * precede both storage locks. The bound matches the Android-derived
     * 512 KiB request limit; native writes and readback remain segmented. */
    size_t snapshot_size=size;
    uint8_t *snapshot=kalloc_data(snapshot_size,Z_WAITOK);
    if(!snapshot)return ENOMEM;
    memcpy(snapshot,buffer,snapshot_size);
    lck_mtx_lock(&lancelot_data_request_lock);
    const uint8_t *in=snapshot;int result=0;
    uint32_t command=0,stage=0,interrupt=0,response=0;
    uint32_t failure_flags=0,mismatch_byte=UINT32_MAX,expected_hash=0,observed_hash=0;
    while(size && !result) {
        lck_mtx_lock(&lancelot_mmc_lock);
        if(!data_ready || data_failed || failed) {
            lck_mtx_unlock(&lancelot_mmc_lock);result=ENXIO;break;
        }
        uint64_t began=mmc_ticks(NULL),physical=0,allowed=0,sector=0;
        size_t chunk=512,verified=0;unsigned written=0;
        if(filesystem.block_size<512 || (filesystem.block_size&511))result=EIO;
        if(!result) {
            uint64_t logical=offset/filesystem.block_size;
            size_t within=(size_t)(offset%filesystem.block_size);
            if(!(offset&(LANCELOT_DATA_VERIFY_BYTES-1)) && size>=LANCELOT_DATA_VERIFY_BYTES &&
               filesystem.block_size-within>=LANCELOT_DATA_VERIFY_BYTES &&
               max_read_blocks>=LANCELOT_DATA_VERIFY_BYTES/512)
                chunk=LANCELOT_DATA_VERIFY_BYTES;
            if(logical>UINT32_MAX || !lhd_map_block(&data_map,logical,&allowed) ||
               !e4_map(&filesystem,&data_image,(uint32_t)logical,&physical) ||
               !physical || physical!=allowed || physical>UINT64_MAX/filesystem.block_size)result=EIO;
            if(!result) {
                uint64_t relative=physical*filesystem.block_size;
                if(relative>UINT64_MAX-within)result=EIO;
                else {
                    relative+=within;
                    if(offer.partition_bytes<chunk || relative>offer.partition_bytes-chunk ||
                       offer.partition_offset>UINT64_MAX-relative || ((offer.partition_offset+relative)&511))result=EIO;
                    else {
                        sector=(offer.partition_offset+relative)>>9;
                        if(sector>UINT32_MAX || chunk/512-1>UINT32_MAX-sector)result=EIO;
                    }
                }
            }
        }
        mmc_observe_begin(LMD_IO_DATA_WRITE,offset,chunk);
        if(result)failure_flags|=1u;
        if(!result && controller.queued_write) {
            /* The complete group lies in the same sealed initialized extent.
             * CQE transfers it by DMA, then the existing readback proves it.
             * An ambiguous failed task reports no unverified prefix. */
            controller.media_write_first=sector;
            controller.media_write_end=sector+chunk/512;
            controller.media_write_enabled=true;
            int error=controller.queued_write(controller.ctx,(uint32_t)sector,(uint32_t)(chunk/512),in);
            controller.media_write_enabled=false;
            if(error){result=EIO;failure_flags|=2u;}
            else written=(unsigned)(chunk/512);
        } else if(!result)for(;written<chunk/512;written++) {
            /* No erase, partition switch or multi-block write is admitted. */
            controller.media_write_first=sector+written;
            controller.media_write_end=sector+written+1;
            controller.media_write_enabled=true;
            int error=lancelot_msdc_write_sector(&controller,(uint32_t)(sector+written),in+written*512);
            controller.media_write_enabled=false;
            if(error){result=EIO;failure_flags|=2u;break;}
        }
        controller.media_write_enabled=false;
        /* On a recovered write failure, only an independently verified prefix
         * can be reported. A failed recovery is never followed by more I/O. */
        if(written && (!result || controller.recovery_status==LM_OK)) {
            size_t check_bytes=(size_t)written*512;
            if(lancelot_msdc_read(&controller,(uint32_t)sector,written,data_readback,check_bytes)!=LM_OK){result=EIO;failure_flags|=4u;}
            else for(unsigned i=0;i<written;i++) {
                if(memcmp(data_readback+i*512,in+i*512,512)){
                    result=EIO;failure_flags|=8u;
                    for(unsigned j=0;j<512;j++)if(data_readback[i*512+j]!=in[i*512+j]){mismatch_byte=i*512+j;break;}
                    expected_hash=data_snapshot_hash(in+i*512,512);
                    observed_hash=data_snapshot_hash(data_readback+i*512,512);
                    break;
                }
                verified+=512;
            }
        }
        data_writes+=verified/512;data_write_bytes+=verified;
        if(transferred)*transferred+=verified;
        mmc_observe_transfer((uint64_t)verified*2);
        offset+=verified;in+=verified;size-=verified;
        if(result)data_failed=true;
        command=controller.last_command;stage=controller.last_stage;
        interrupt=controller.last_int;response=controller.last_response;
        data_write_us+=elapsed_us(began,mmc_ticks(NULL));
        mmc_observe_end();lck_mtx_unlock(&lancelot_mmc_lock);
    }
    lck_mtx_unlock(&lancelot_data_request_lock);
    kfree_data(snapshot,snapshot_size);
    if(result && result!=ENXIO)
        printf("lancelot-data: write failed offset=%llu cmd=%u stage=%u irq=%#x response=%#x failure_flags=%#x mismatch_byte=%u expected_hash=%x observed_hash=%x snapshot_bytes=%lu; data writes stopped\n",
            offset,command,stage,interrupt,response,failure_flags,mismatch_byte,expected_hash,observed_hash,snapshot_size);
    mmc_report_native_failure();return result;
}
static int
data_flush(void)
{
    lck_mtx_lock(&lancelot_data_request_lock);
    lck_mtx_lock(&lancelot_mmc_lock);
    mmc_observe_begin(LMD_IO_FLUSH,0,0);
    int result=EIO;
    if(data_ready&&!data_failed&&!failed) {
        controller.media_write_first=controller.media_write_end=0;
        controller.media_write_enabled=true;
        result=lancelot_msdc_flush(&controller)==LM_OK?0:EIO;
        controller.media_write_enabled=false;
        if(!result)++data_flushes;else data_failed=true;
    }
    mmc_observe_end();lck_mtx_unlock(&lancelot_mmc_lock);
    lck_mtx_unlock(&lancelot_data_request_lock);return result;
}
static int data_open(dev_t dev,__unused int flags,__unused int type,__unused proc_t p) {
    return minor(dev)==1&&data_ready&&!data_failed&&!failed?0:ENXIO;
}
static int data_close(__unused dev_t dev,__unused int flags,__unused int type,__unused proc_t p){return 0;}
static void
data_strategy(buf_t bp)
{
    uint32_t count=buf_count(bp);int error=0;buf_setresid(bp,count);
    caddr_t mapped=NULL;bool did_map=false;
    lck_mtx_lock(&lancelot_mmc_lock);uint32_t block_size=data_block_size;lck_mtx_unlock(&lancelot_mmc_lock);
    if(!data_ready||data_failed||failed||minor(buf_device(bp))!=1)error=ENXIO;
    else if(buf_blkno(bp)<0||(uint64_t)buf_blkno(bp)>UINT64_MAX/block_size)error=EINVAL;
    else {
        uint64_t offset=(uint64_t)buf_blkno(bp)*block_size;size_t transferred=0;
        if(offset>data_bytes||count>data_bytes-offset)error=EINVAL;
        else if(count) {
            /* HFS cluster I/O carries a UPL instead of b_datap. Map outside
             * the MMC mutex, and release it before completing the buffer. */
            error=buf_map(bp,&mapped);did_map=!error;
            if(!error&&!mapped)error=EIO;
            if(!error&&(buf_flags(bp)&B_READ))error=data_file_read(offset,count,mapped);
            else if(!error) {
                error=data_file_write(offset,count,mapped,&transferred);
                buf_setresid(bp,count-(uint32_t)transferred);
                if(!error&&(buf_flags(bp)&B_FUA))error=data_flush();
            }
        }
        if(!error)buf_setresid(bp,0);
    }
    if(did_map) {int unmap_error=buf_unmap(bp);if(!error)error=unmap_error;}
    if(error&&os_atomic_inc_orig(&data_strategy_diagnostics,relaxed)<8)
        printf("lancelot-data-strategy: error=%d block=%lld block_size=%u bytes=%u flags=%#x mapped=%d resid=%u data_failed=%d system_failed=%d\n",
            error,(long long)buf_blkno(bp),block_size,count,(unsigned)buf_flags(bp),
            did_map,buf_resid(bp),data_failed,failed);
    if(error)buf_seterror(bp,error);buf_biodone(bp);
}
static int
data_ioctl(dev_t dev,u_long cmd,caddr_t value,__unused int flag,__unused proc_t p)
{
    if(minor(dev)!=1||!data_ready||data_failed||failed)return ENXIO;
    switch(cmd) {
    case DKIOCGETBLOCKSIZE:*(uint32_t*)value=data_block_size;return 0;
    case DKIOCSETBLOCKSIZE:{uint32_t size=*(uint32_t*)value;if(size<512||size>4096||(size&(size-1)))return EINVAL;
        lck_mtx_lock(&lancelot_mmc_lock);data_block_size=size;lck_mtx_unlock(&lancelot_mmc_lock);return 0;}
    case DKIOCGETPHYSICALBLOCKSIZE:*(uint32_t*)value=4096;return 0;
    case DKIOCGETBLOCKCOUNT:*(uint64_t*)value=data_bytes/data_block_size;return 0;
    case DKIOCISWRITABLE:*(uint32_t*)value=1;return 0;
    /* Journaling must issue full cache flushes; no hardware FUA or barrier-only
     * shortcut is advertised by this synchronous PIO device. */
    case DKIOCGETFEATURES:*(uint32_t*)value=0;return 0;
    case DKIOCSYNCHRONIZE:return data_flush();
    default:return lancelot_mmc_io_limits(cmd,value,data_block_size);
    }
}
static int data_size(dev_t dev){return minor(dev)==1&&data_ready&&data_bytes/512<=INT32_MAX?(int)(data_bytes/512):-1;}
static int
data_char_io(dev_t dev,struct uio *uio,bool write)
{
    if(minor(dev)!=1||!data_ready||data_failed||failed)return ENXIO;
    if(uio_offset(uio)<0 || (write&&((uio_offset(uio)&511)||(uio_resid(uio)&511) ||
       (uint64_t)uio_offset(uio)>data_bytes || (uint64_t)uio_resid(uio)>data_bytes-(uint64_t)uio_offset(uio))))return EINVAL;
    if(!uio_resid(uio) || (uint64_t)uio_offset(uio)>=data_bytes)return 0;
    size_t bounce_bytes=(size_t)MIN((uint64_t)16384,MIN((uint64_t)uio_resid(uio),data_bytes-(uint64_t)uio_offset(uio)));
    uint8_t *bounce=kalloc_data(bounce_bytes,Z_WAITOK);if(!bounce)return ENOMEM;
    struct uio *copy=write?uio_duplicate(uio):NULL;
    if(write&&!copy){kfree_data(bounce,bounce_bytes);return ENOMEM;}
    int error=0;
    while(uio_resid(uio)>0 && (uint64_t)uio_offset(uio)<data_bytes && !error) {
        uint64_t offset=(uint64_t)uio_offset(uio);size_t count=(size_t)MIN((uint64_t)bounce_bytes,MIN((uint64_t)uio_resid(uio),data_bytes-offset));
        if(write){size_t transferred=0;error=uiomove((caddr_t)bounce,(int)count,copy);
            if(!error)error=data_file_write(offset,count,bounce,&transferred);
            if(transferred)uio_update(uio,(user_ssize_t)transferred);}
        else {error=data_file_read(offset,count,bounce);if(!error)error=uiomove((caddr_t)bounce,(int)count,uio);}
    }
    if(copy)uio_free(copy);kfree_data(bounce,bounce_bytes);return error;
}
static int data_cread(dev_t d,struct uio*u,__unused int flags){return data_char_io(d,u,false);}
static int data_cwrite(dev_t d,struct uio*u,__unused int flags){return data_char_io(d,u,true);}
static const struct bdevsw data_bdev={.d_open=data_open,.d_close=data_close,.d_strategy=data_strategy,
    .d_ioctl=data_ioctl,.d_dump=eno_dump,.d_psize=data_size,.d_type=D_DISK};
static const struct cdevsw data_cdev={.d_open=data_open,.d_close=data_close,.d_read=data_cread,.d_write=data_cwrite,
    .d_ioctl=data_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=eno_select,.d_mmap=eno_mmap,
    .d_strategy=eno_strat,.d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=D_DISK};
static void
lancelot_data_init(void)
{
    uint32_t requested=0;(void)PE_parse_boot_argn("lancelot_data",&requested,sizeof(requested));
    if(requested!=1)return;
    char pinned_hex[65]={0};uint8_t pinned[32],actual[32],vh[512],alternate[512];
    if(!PE_parse_boot_argn("lancelot_data_map",pinned_hex,sizeof(pinned_hex))||strlen(pinned_hex)!=64) {
        printf("lancelot-data: missing data-map SHA binding; no writable device\n");return;
    }
    for(unsigned i=0;i<32;i++) {
        unsigned n=0;for(unsigned j=0;j<2;j++) {char c=pinned_hex[2*i+j];
            if(c>='0'&&c<='9')n=n*16+(unsigned)(c-'0');
            else if(c>='a'&&c<='f')n=n*16+(unsigned)(c-'a'+10);
            else {printf("lancelot-data: malformed data-map SHA binding\n");return;}}
        pinned[i]=(uint8_t)n;
    }
    struct lhd_walk *walk=kalloc_data(sizeof(*walk),Z_WAITOK|Z_ZERO);
    struct ext4_inode map_inode;
    if(!walk)return;
    unsigned admission_stage=1;
    lck_mtx_lock(&lancelot_mmc_lock);
    bool valid=ready&&!failed&&ext4_lookup(&filesystem,LANCELOT_DATA_FILE,&data_image)&&
        data_image.size>=UINT64_C(67108864)&&!(data_image.size&4095)&&
        ext4_lookup(&filesystem,LANCELOT_DATA_MAP,&map_inode)&&map_inode.size>=LHD_MAP_HEADER&&map_inode.size<=LHD_MAX_MAP_BYTES;
    if(valid) {
        admission_stage=2;
        data_map_size=(size_t)map_inode.size;data_map_raw=kalloc_data(data_map_size,Z_WAITOK);
        valid=data_map_raw&&ext4_read_file(&filesystem,&map_inode,0,data_map_size,data_map_raw);
    }
    if(valid){admission_stage=3;sha256(data_map_raw,data_map_size,actual);valid=!memcmp(actual,pinned,sizeof(actual));}
    if(valid){admission_stage=4;valid=lhd_map_open(&data_map,data_map_raw,data_map_size,filesystem.block_size,filesystem.blocks,data_image.size);}
    if(valid){admission_stage=5;valid=lhd_metadata_safe(&filesystem,&data_map,&data_guard_groups);}
    if(valid){admission_stage=6;valid=lhd_initialized_tree(&filesystem,&data_image,&data_map,walk,&data_guard_nodes);}
    if(valid){admission_stage=7;valid=ext4_read_file(&filesystem,&data_image,1024,512,vh)&&
        ext4_read_file(&filesystem,&data_image,data_image.size-1024,512,alternate)&&lhd_hfs_geometry(vh,alternate,data_image.size);}
    data_bytes=valid?data_image.size:0;
    lck_mtx_unlock(&lancelot_mmc_lock);
    mmc_report_native_failure();
    kfree_data(walk,sizeof(*walk));
    if(!valid){if(data_map_raw)kfree_data(data_map_raw,data_map_size);data_map_raw=NULL;memset(&data_map,0,sizeof(data_map));
        printf("lancelot-data: admission failed stage=%u groups=%llu nodes=%llu map_bytes=%lu; no writable device\n",
            admission_stage,data_guard_groups,data_guard_nodes,(unsigned long)data_map_size);return;}
    int bm=bdevsw_add(-1,&data_bdev),cm=bm<0?-1:cdevsw_add_with_bdev(-1,&data_cdev,bm);
    if(cm<0 || !devfs_make_node(makedev(bm,1),DEVFS_BLOCK,UID_ROOT,GID_OPERATOR,0600,"lancelot_data") ||
       !devfs_make_node(makedev(cm,1),DEVFS_CHAR,UID_ROOT,GID_OPERATOR,0600,"rlancelot_data"))return;
    data_ready=true;
    printf("lancelot-data-io: request_bytes=%u snapshot_bytes=%u segments=%u segment_bytes=%u active_commands=1 source=Android_mmcblk0\n",
        LANCELOT_MMC_REQUEST_BYTES,LANCELOT_DATA_SNAPSHOT_MAX,LANCELOT_MMC_SEGMENTS,LANCELOT_MMC_SEGMENT_BYTES);
    printf("lancelot-data: HFSX data-file=%s bytes=%llu ranges=%u extent_nodes=%llu metadata_groups=%llu map_sha256=%s write_scope=sealed_initialized_file_extents readback=verified_4K_chunks logical_block=4096 native_sector=512 system_readonly=1\n",
        LANCELOT_DATA_FILE,data_bytes,data_map.count,data_guard_nodes,data_guard_groups,pinned_hex);
}
