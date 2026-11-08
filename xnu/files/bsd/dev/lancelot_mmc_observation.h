/* SPDX-License-Identifier: APSL-2.0
 * Lock-free observation of the operation already serialized by the MMC mutex.
 * No hardware commands, allocations, waits, or per-request printing.
 */
#ifndef LANCELOT_MMC_OBSERVATION_H
#define LANCELOT_MMC_OBSERVATION_H
enum {LMD_IO_IDLE=0,LMD_IO_SYSTEM_READ=1,LMD_IO_DATA_READ=2,LMD_IO_DATA_WRITE=3,LMD_IO_FLUSH=4};
enum {LMD_PHASE_IDLE=0,LMD_PHASE_FILE=1,LMD_PHASE_MASK_IRQ=2,LMD_PHASE_PIO=3,
    LMD_PHASE_RESTORE_IRQ=4,LMD_PHASE_IRQ_RESTORED=5,LMD_PHASE_FILE_DONE=6};
struct lmd_observer {
    _Atomic uint64_t epoch,owner,operation,offset,bytes,began,transport_bytes,command,phase;
};
struct lmd_observation {
    uint64_t epoch,owner,operation,offset,bytes,began,transport_bytes,command,phase;
    int consistent;
};
static inline void lmd_begin(struct lmd_observer *o,uint64_t owner,unsigned operation,
    uint64_t offset,uint64_t bytes,uint64_t ticks) {
    os_atomic_inc(&o->epoch,acq_rel);
    os_atomic_store(&o->owner,owner,release);os_atomic_store(&o->operation,operation,release);
    os_atomic_store(&o->offset,offset,release);os_atomic_store(&o->bytes,bytes,release);
    os_atomic_store(&o->began,ticks,release);os_atomic_store(&o->transport_bytes,0,relaxed);
    os_atomic_store(&o->command,0,relaxed);os_atomic_store(&o->phase,LMD_PHASE_FILE,relaxed);
    os_atomic_inc(&o->epoch,release);
}
static inline void lmd_end(struct lmd_observer *o) {
    os_atomic_inc(&o->epoch,acq_rel);os_atomic_store(&o->operation,LMD_IO_IDLE,release);
    os_atomic_store(&o->owner,0,release);os_atomic_store(&o->phase,LMD_PHASE_IDLE,relaxed);
    os_atomic_inc(&o->epoch,release);
}
static inline struct lmd_observation lmd_snapshot(struct lmd_observer *o) {
    struct lmd_observation v={0};uint64_t before=os_atomic_load(&o->epoch,acquire);
    v.epoch=before;v.owner=os_atomic_load(&o->owner,acquire);v.operation=os_atomic_load(&o->operation,acquire);
    v.offset=os_atomic_load(&o->offset,acquire);v.bytes=os_atomic_load(&o->bytes,acquire);
    v.began=os_atomic_load(&o->began,acquire);v.transport_bytes=os_atomic_load(&o->transport_bytes,relaxed);
    v.command=os_atomic_load(&o->command,relaxed);v.phase=os_atomic_load(&o->phase,relaxed);
    v.consistent=!(before&1)&&before==os_atomic_load(&o->epoch,acquire);return v;
}
#endif
