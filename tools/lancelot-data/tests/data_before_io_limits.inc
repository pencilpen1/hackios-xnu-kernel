/* E1 data write and ioctl bodies; names isolated for before/after regression. */
static int
limits_before_data_file_write(uint64_t offset,size_t size,const void *buffer,size_t *transferred)
{
    if(transferred)*transferred=0;
    if(!buffer || (offset&511) || (size&511) ||
       offset>data_bytes || size>data_bytes-offset || size>(128u*1024u))return EINVAL;
    if(!size)return 0;
    /* A mapped HFS/UPL page may be changed through another CPU alias while
     * PIO or readback runs. Verify the exact bytes submitted, not a later
     * version of the caller's memory. Allocation/copy can fault and must
     * precede both storage locks. The device advertises 64 KiB requests. */
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
        if(!result)for(;written<chunk/512;written++) {
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
limits_before_data_ioctl(dev_t dev,u_long cmd,caddr_t value,__unused int flag,__unused proc_t p)
{
    if(minor(dev)!=1||!data_ready||data_failed||failed)return ENXIO;
    switch(cmd) {
    case DKIOCGETBLOCKSIZE:*(uint32_t*)value=data_block_size;return 0;
    case DKIOCSETBLOCKSIZE:{uint32_t size=*(uint32_t*)value;if(size<512||size>4096||(size&(size-1)))return EINVAL;
        lck_mtx_lock(&lancelot_mmc_lock);data_block_size=size;lck_mtx_unlock(&lancelot_mmc_lock);return 0;}
    case DKIOCGETPHYSICALBLOCKSIZE:*(uint32_t*)value=4096;return 0;
    case DKIOCGETBLOCKCOUNT:*(uint64_t*)value=data_bytes/data_block_size;return 0;
    case DKIOCISWRITABLE:*(uint32_t*)value=1;return 0;
    case DKIOCGETMAXBLOCKCOUNTREAD:*(uint64_t*)value=65536/data_block_size;return 0;
    case DKIOCGETMAXBLOCKCOUNTWRITE:*(uint64_t*)value=65536/data_block_size;return 0;
    /* Journaling must issue full cache flushes; no hardware FUA or barrier-only
     * shortcut is advertised by this synchronous PIO device. */
    case DKIOCGETFEATURES:*(uint32_t*)value=0;return 0;
    case DKIOCSYNCHRONIZE:return data_flush();
    default:return ENOTTY;
    }
}
