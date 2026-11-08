/* SPDX-License-Identifier: APSL-2.0
 * MT6768 Panfrost queue, following hakios-mt6768 RedwoodMali's kernel worker.
 * The Lancelot board adapter owns clocks, rails, private ASes and recovery.
 */
#ifndef LANCELOT_MALI_QUEUE_H
#define LANCELOT_MALI_QUEUE_H
#include "lancelot_mali_core.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LMQ_QUEUE_DEPTH 64u
struct lmq_job {
    struct lm_vm *vm;
    struct lm_range *ranges;
    uint32_t count, requirements;
    int32_t owner_pid;
    uint64_t jc, fs_jc, deadline_ns, token, queued_ns; /* fs_jc!=0: one VTC/FS request */
    void *owner;
    int (*ready)(struct lmq_job *); /* earlier failed producer cancels later work */
    /* Called exactly once after acceptance, outside the queue/hardware lock.
     * All ranges, backing and owner remain retained through this callback. */
    void (*complete)(struct lmq_job *, const struct lm_job_result *, int);
};
int lancelot_mali_queue_start(void);
int lancelot_mali_enqueue(struct lmq_job *, uint64_t *token);
int lancelot_mali_queue_drain(uint64_t timeout_ns);
int lancelot_hakios_backend(void);
int lancelot_mali_queued_backend(void);

#ifdef __cplusplus
}
#endif
#endif
