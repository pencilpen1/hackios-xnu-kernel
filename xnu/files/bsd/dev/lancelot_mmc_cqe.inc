/* Native XNU adapter for the independently testable HS400/CQHCI transport. */
extern uint64_t kvtophys(vm_offset_t);
static struct lancelot_cqhci mmc_queue;
static vm_offset_t mmc_dma_arena;
static void mmc_dma_sync(__unused void *ctx,size_t offset,size_t bytes,unsigned direction) {
    if(offset>LC_ARENA_BYTES || bytes>LC_ARENA_BYTES-offset || direction>2)
        panic("lancelot CQE DMA range");
    uintptr_t start=(mmc_dma_arena+offset)&~(uintptr_t)63;
    uintptr_t end=(mmc_dma_arena+offset+bytes+63)&~(uintptr_t)63;
    for(uintptr_t p=start;p<end;p+=64) {
        if(direction==0)__asm__ volatile("dc cvac, %0"::"r"(p):"memory");
        else if(direction==1)__asm__ volatile("dc civac, %0"::"r"(p):"memory");
        else __asm__ volatile("dc ivac, %0"::"r"(p):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
}
static int mmc_queue_result(int result) {
    controller.last_int=mmc_queue.last_host_int;
    controller.last_response=result?mmc_queue.last_terri:0x900;
    controller.recovery_status=result?LM_IO:LM_OK;
    os_atomic_store(&mmc_cqe_requests,mmc_queue.requests,relaxed);
    os_atomic_store(&mmc_cqe_bytes,mmc_queue.bytes,relaxed);
    os_atomic_store(&mmc_cqe_waits,mmc_queue.waits,relaxed);
    os_atomic_store(&mmc_cqe_total,mmc_queue.total_ticks,relaxed);
    os_atomic_store(&mmc_cqe_max,mmc_queue.max_ticks,relaxed);
    os_atomic_store(&mmc_cqe_failed,mmc_queue.failed,relaxed);
    if(result)os_atomic_store(&mmc_observer.phase,LMD_PHASE_IRQ_RESTORED,relaxed);
    return result;
}
static int mmc_queue_read(__unused void *ctx,uint32_t sector,uint32_t count,uint8_t *out,size_t capacity) {
    if(capacity<(size_t)count*512)return LM_CONFIG;
    controller.last_command=46;controller.last_stage=6;
    os_atomic_store(&mmc_observer.command,46,relaxed);
    return mmc_queue_result(lc_transfer(&mmc_queue,sector,count,out,false));
}
static int mmc_queue_write(__unused void *ctx,uint32_t sector,uint32_t count,const uint8_t *in) {
    if(!count || !controller.media_write_enabled || sector<controller.media_write_first ||
       (uint64_t)sector+count>controller.media_write_end)return LM_CONFIG;
    controller.last_command=47;controller.last_stage=6;
    os_atomic_store(&mmc_observer.command,47,relaxed);
    return mmc_queue_result(lc_transfer(&mmc_queue,sector,count,(uint8_t *)(uintptr_t)in,true));
}
static int mmc_queue_flush(__unused void *ctx) {
    if(!controller.media_write_enabled)return LM_CONFIG;
    controller.last_command=6;controller.last_stage=6;
    os_atomic_store(&mmc_observer.command,6,relaxed);
    return mmc_queue_result(lc_flush(&mmc_queue));
}
static uint32_t mmc_timeout_field(uint64_t ns,uint64_t hz,uint32_t max) {
    uint64_t tick_ns=1000000000/hz;
    uint64_t cycles=(ns+tick_ns-1)/tick_ns;
    uint64_t value=((cycles+1048575)/1048576)*2; /* HS400 DDR */
    value=value>1?value-1:0;
    return (uint32_t)(value>max?max:value);
}
static bool mmc_enable_hs400_cqe(void) {
    uint8_t *probe=kalloc_data(LC_MAX_BYTES,Z_WAITOK);
    if(!probe)return false;
    uint8_t expected[32],actual[32];
    bool ok=false;
    uint32_t sector=(uint32_t)(offer.partition_offset>>9);
    for(unsigned i=0;i<1024;i+=16)
        if(lancelot_msdc_read(&controller,sector+i,16,probe+i*512,8192)!=LM_OK)goto done;
    sha256(probe,LC_MAX_BYTES,expected);
    struct lancelot_hs400 hs;
    int status=lancelot_hs400_negotiate(&controller,offer.source_clock_hz,counter_frequency,mmc_verify_super,NULL,&hs);
    printf("lancelot-mmc-hs400: status=%d stage=%u hs200_map=%x edge=%u tap=%u hs400_map=%x tap=%u clock=%llu depth=%u\n",
        status,hs.stage,hs.hs200_map,hs.hs200_edge,hs.hs200_tap,hs.hs400_map,hs.hs400_tap,hs.clock_hz,hs.depth);
    if(status!=LM_OK)goto done;
    active_clock_hz=hs.clock_hz;
    if(kmem_alloc_contig(kernel_map,&mmc_dma_arena,round_page(LC_ARENA_BYTES),PAGE_MASK,0,0,
        KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)goto done;
    mmc_queue=(struct lancelot_cqhci){.read=mmc_read_register,.write=mmc_write_register,
        .ticks=mmc_ticks,.wait=mmc_idle_wait,.sync=mmc_dma_sync,.arena=(uint8_t *)mmc_dma_arena,
        .physical=kvtophys(mmc_dma_arena),.read_timeout=counter_frequency/2,
        .write_timeout=counter_frequency*30,.poll_ticks=counter_frequency/20000};
    /* Check physically contiguous DMA ownership before enabling card CMDQ. */
    for(size_t off=0;off<LC_ARENA_BYTES;off+=PAGE_SIZE)
        if(kvtophys(mmc_dma_arena+off)!=mmc_queue.physical+off)goto done;
    if(!mmc_queue.physical || mmc_queue.physical+LC_ARENA_BYTES>(1ULL<<36))goto done;
    controller.bus_mode_change_enabled=true;
    status=lh_switch(&controller,15,1);
    controller.bus_mode_change_enabled=false;
    if(status!=LM_OK || lm_card_ready(&controller)!=LM_OK)goto done;
    lh_field(&controller,LM_CFG,8,0); /* CQE ADMA owns data, not the CPU FIFO */
    lh_field(&controller,0xb8,1u<<1,1u<<1); /* MT6768 36-bit DMA address support */
    lh_field(&controller,0xb4,1u<<7,1u<<7);
    lh_field(&controller,LM_BUS_CFG,0xffu<<24,mmc_timeout_field(100000000,active_clock_hz,255)<<24);
    lh_field(&controller,LM_BUS_CFG,0x1fffu<<2,mmc_timeout_field(20000000000ULL,active_clock_hz,8191)<<2);
    controller.write(NULL,LM_INTEN,0);controller.write(NULL,LM_INT,0xffffffff);
    status=lc_enable(&mmc_queue,controller.rca,hs.depth);
    if(status!=LM_OK)goto done;
    controller.queued_read=mmc_queue_read;controller.queued_write=mmc_queue_write;controller.queued_flush=mmc_queue_flush;
    bzero(probe,LC_MAX_BYTES);
    status=lancelot_msdc_read(&controller,sector,1024,probe,LC_MAX_BYTES);
    if(status!=LM_OK)goto done;
    sha256(probe,LC_MAX_BYTES,actual);
    if(memcmp(expected,actual,32)) { (void)lc_poison(&mmc_queue,LM_IO);goto done; }
    max_read_blocks=1024;
    printf("lancelot-mmc: HS400 clock=%llu width=8 CQE=1 device_depth=%u batch_slots=8 max_request=524288 dma_compare=PASS\n",
        active_clock_hz,hs.depth);
    ok=true;
done:
    if(!ok)printf("lancelot-mmc: HS400/CQE initialization failed; data volume remains closed is=%x terri=%x host=%x\n",
        mmc_queue.last_is,mmc_queue.last_terri,mmc_queue.last_host_int);
    kfree_data(probe,LC_MAX_BYTES);
    return ok;
}
