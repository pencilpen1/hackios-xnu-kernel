/* SPDX-License-Identifier: APSL-2.0
 * Per-CPU observation of callbacks that already run with interrupts masked.
 * The writer neither logs nor waits. A reader takes one advisory snapshot.
 */
#ifndef LANCELOT_SGI_OBSERVATION_H
#define LANCELOT_SGI_OBSERVATION_H

#define LSG_STACK_CAPACITY 8u
enum {
    LSG_IDLE=0, LSG_HANDLER=1, LSG_DEBUG=2, LSG_CLOCK=3, LSG_KPERF=4,
    LSG_XCALL=5, LSG_IMMEDIATE=6, LSG_AST=7, LSG_SIMPLE=8, LSG_SEND=9,
    LSG_OVERFLOW=255
};
struct lsg_frame {
    _Atomic uint32_t stage, signal;
    _Atomic uint64_t callback, argument, began;
};
struct lsg_observer {
    _Atomic uint64_t epoch;
    _Atomic uint32_t depth;
    struct lsg_frame frames[LSG_STACK_CAPACITY];
};
struct lsg_sample {
    uint64_t epoch, callback, argument, began;
    uint32_t depth, stage, signal;
    int consistent;
};
/* Only this CPU writes its slot, with IRQ/FIQ masked. Nested xcall draining
 * pushes another frame and restores the suspended callback when it returns. */
static inline void
lsg_push(struct lsg_observer *o, uint32_t stage, uint32_t signal,
    uint64_t callback, uint64_t argument, uint64_t ticks)
{
    os_atomic_inc(&o->epoch, acq_rel);
    uint32_t depth=os_atomic_load(&o->depth, relaxed);
    if(depth<LSG_STACK_CAPACITY) {
        struct lsg_frame *f=&o->frames[depth];
        os_atomic_store(&f->stage, stage, relaxed);
        os_atomic_store(&f->signal, signal, relaxed);
        os_atomic_store(&f->callback, callback, relaxed);
        os_atomic_store(&f->argument, argument, relaxed);
        os_atomic_store(&f->began, ticks, relaxed);
    }
    if(depth!=UINT32_MAX)os_atomic_store(&o->depth, depth+1, relaxed);
    os_atomic_inc(&o->epoch, release);
}
static inline void
lsg_pop(struct lsg_observer *o)
{
    os_atomic_inc(&o->epoch, acq_rel);
    uint32_t depth=os_atomic_load(&o->depth, relaxed);
    if(depth)os_atomic_store(&o->depth, depth-1, relaxed);
    os_atomic_inc(&o->epoch, release);
}
static inline struct lsg_sample
lsg_snapshot(struct lsg_observer *o)
{
    struct lsg_sample v={0};
    uint64_t before=os_atomic_load(&o->epoch, acquire);
    v.epoch=before;v.depth=os_atomic_load(&o->depth, relaxed);
    if(v.depth&&v.depth<=LSG_STACK_CAPACITY) {
        struct lsg_frame *f=&o->frames[v.depth-1];
        v.stage=os_atomic_load(&f->stage, relaxed);
        v.signal=os_atomic_load(&f->signal, relaxed);
        v.callback=os_atomic_load(&f->callback, relaxed);
        v.argument=os_atomic_load(&f->argument, relaxed);
        v.began=os_atomic_load(&f->began, relaxed);
    }else if(v.depth) v.stage=LSG_OVERFLOW;
    os_atomic_thread_fence(acquire);
    v.consistent=!(before&1)&&before==os_atomic_load(&o->epoch, acquire)&&
        v.depth<=LSG_STACK_CAPACITY;
    return v;
}

#if defined(GENERIC_GIC_V3_IRQ)
extern _Atomic uint32_t lancelot_sgi_observation_enabled;
void lancelot_sgi_enter(unsigned, uint32_t, uint32_t, uintptr_t, uintptr_t);
void lancelot_sgi_leave(unsigned);
bool lancelot_sgi_read(unsigned, struct lsg_sample *);
#define LSG_ENTER(cpu, stage, signal, callback, argument) do { \
    if(os_atomic_load(&lancelot_sgi_observation_enabled, relaxed)) \
        lancelot_sgi_enter((cpu)->cpu_number, (stage), (signal), \
            (uintptr_t)VM_KERNEL_STRIP_UPTR(callback), (uintptr_t)(argument)); \
} while(0)
#define LSG_LEAVE(cpu) do { \
    if(os_atomic_load(&lancelot_sgi_observation_enabled, relaxed)) \
        lancelot_sgi_leave((cpu)->cpu_number); \
} while(0)
#else
#define LSG_ENTER(cpu, stage, signal, callback, argument) do {} while(0)
#define LSG_LEAVE(cpu) do {} while(0)
#endif
#endif
