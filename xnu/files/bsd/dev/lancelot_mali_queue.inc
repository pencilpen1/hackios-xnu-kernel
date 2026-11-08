/* SPDX-License-Identifier: APSL-2.0
 * MT6768/Mali-G52 queue for the Panfrost DRM user client. Architecture from
 * oosexgang/hakios-mt6768@4f270371, kernel/bsd/dev/redwood_mali.c:
 * bounded FIFO, one sleeping worker, real JM completion before retirement.
 * Unlike the donor's shared AS0, each request retains its client's private VM.
 * Existing board code supplies the measured MT6358/clock and return contract.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include "lancelot_mali_queue.h"

static LCK_GRP_DECLARE(lmq_group, "redwood-mali-queue");
static LCK_MTX_DECLARE(lmq_lock, &lmq_group);
static struct lmq_job *lmq_jobs[LMQ_QUEUE_DEPTH];
static uint32_t lmq_head, lmq_count;
static uint64_t lmq_next_token;
static bool lmq_started, lmq_draining;

static uint64_t lmq_now(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

int lancelot_mali_queued_backend(void)
{
    uint32_t value = 0;
    (void)PE_parse_boot_argn("lancelot_mali_queue", &value, sizeof(value));
    return value == 1;
}

static void lmq_worker(__unused void *argument, __unused wait_result_t result)
{
    for (;;) {
        lck_mtx_lock(&lmq_lock);
        while (!lmq_count)
            lck_mtx_sleep(&lmq_lock, LCK_SLEEP_DEFAULT,
                (event_t)&lmq_count, THREAD_UNINT);
        struct lmq_job *job = lmq_jobs[lmq_head];
        lck_mtx_unlock(&lmq_lock);
        struct lm_job_result completed[2] = {};
        uint64_t began = lmq_now();
        int error;
        if (!began || began < job->queued_ns) error = LM_EIO;
        else if (began >= job->deadline_ns) error = LM_ETIMEDOUT;
        else {
            error=job->ready(job);
            if(!error) {
                uint64_t ready_at=lmq_now();
                if(ready_at<began)error=LM_EIO;
                else if(ready_at>=job->deadline_ns)error=LM_ETIMEDOUT;
                else if(job->fs_jc)error=lm_gpu_submit_pair(job->vm,job->jc,job->fs_jc,
                    job->deadline_ns-ready_at,completed,job->ranges,job->count,
                    job->ranges,job->count,job->owner_pid,job->token);
                else error = lm_gpu_submit_ranges_owned(job->vm, job->jc, job->requirements,
                    job->deadline_ns - ready_at, completed, job->ranges, job->count,
                    job->owner_pid,job->token);
            }
        }
        if(!error && lmq_now()>=job->deadline_ns)error=LM_ETIMEDOUT;
        uint64_t token = job->token, queued = job->queued_ns;
        uint64_t hw0=completed[0].sequence,hw1=completed[1].sequence;
        bool paired=job->fs_jc!=0;
        int32_t owner_pid = job->owner_pid;
        /* Retirement precedes making room/drain completion. The callback may
         * release the last owner/job reference; never dereference job after it. */
        job->complete(job, completed, error);
        uint64_t ended = lmq_now();
        lck_mtx_lock(&lmq_lock);
        lmq_jobs[lmq_head] = NULL;
        lmq_head = (lmq_head + 1) % LMQ_QUEUE_DEPTH;
        --lmq_count;
        lck_mtx_unlock(&lmq_lock);
        wakeup((caddr_t)&lmq_count);
        if (error || token <= 8 || (token & 255u) == 0)
            printf("lancelot-mali-queue: token=%llu owner_pid=%d queued_ns=%llu begin_ns=%llu end_ns=%llu e=%d retired=1 paired=%u hwq0=%llu hwq1=%llu\n",
                token, owner_pid, queued, began, ended, error,paired,hw0,hw1);
    }
}

int lancelot_mali_queue_start(void)
{
    lck_mtx_lock(&lmq_lock);
    if (lmq_started) { lck_mtx_unlock(&lmq_lock); return LM_OK; }
    thread_t thread;
    kern_return_t result = kernel_thread_start(lmq_worker, NULL, &thread);
    if (result == KERN_SUCCESS) {
        thread_set_thread_name(thread, "MT6768 Panfrost jobs");
        thread_deallocate(thread);
        lmq_started = true;
    }
    lck_mtx_unlock(&lmq_lock);
    return result == KERN_SUCCESS ? LM_OK : LM_ENOMEM;
}

int lancelot_mali_enqueue(struct lmq_job *job, uint64_t *token)
{
    if (!token) return LM_EINVAL;
    *token = 0;
    if (!job || !job->vm || !job->ranges || !job->count || !job->complete || !job->ready ||
        !job->owner || job->owner_pid <= 0 || !job->jc || (job->jc & 63u) ||
        job->requirements > 1 || (job->fs_jc && ((job->fs_jc&63u)||job->requirements)) ||
        job->token || !job->deadline_ns) return LM_EINVAL;
    lck_mtx_lock(&lmq_lock);
    uint64_t now = lmq_now();
    int error = !lmq_started || lmq_draining ? LM_ENODEV :
        lmq_count == LMQ_QUEUE_DEPTH ? LM_EBUSY :
        !now || now >= job->deadline_ns ? LM_ETIMEDOUT :
        job->deadline_ns - now > UINT64_C(5000000000) ? LM_EINVAL :
        lmq_next_token == UINT64_MAX ? LM_EBUSY : LM_OK;
    if (!error) {
        job->token = ++lmq_next_token;
        job->queued_ns = now;
        lmq_jobs[(lmq_head + lmq_count) % LMQ_QUEUE_DEPTH] = job;
        ++lmq_count;
        *token = job->token;
    }
    lck_mtx_unlock(&lmq_lock);
    if (!error) wakeup((caddr_t)&lmq_count);
    return error; /* EBUSY is before acceptance: ownership remains with caller. */
}

int lancelot_mali_queue_drain(uint64_t timeout_ns)
{
    uint64_t now = lmq_now(), absolute;
    if (!timeout_ns || timeout_ns > UINT64_C(10000000000) ||
        !now || now > UINT64_MAX - timeout_ns) return LM_EINVAL;
    uint64_t end = now + timeout_ns;
    nanoseconds_to_absolutetime(end, &absolute);
    lck_mtx_lock(&lmq_lock);
    lmq_draining = true;
    while (lmq_count && lmq_now() < end)
        lck_mtx_sleep_deadline(&lmq_lock, LCK_SLEEP_DEFAULT,
            (event_t)&lmq_count, THREAD_UNINT, absolute);
    int error = lmq_count ? LM_ETIMEDOUT : LM_OK;
    lck_mtx_unlock(&lmq_lock);
    return error;
}

/* A full FIFO is backpressure, not an accepted submission failure. Caller
 * retains its snapshot and releases client/presenter locks before waiting. */
int lancelot_mali_queue_wait_room(uint64_t deadline_ns)
{
    uint64_t absolute;
    nanoseconds_to_absolutetime(deadline_ns, &absolute);
    lck_mtx_lock(&lmq_lock);
    while (lmq_started && !lmq_draining && lmq_count == LMQ_QUEUE_DEPTH && lmq_now() < deadline_ns)
        lck_mtx_sleep_deadline(&lmq_lock, LCK_SLEEP_DEFAULT,
            (event_t)&lmq_count, THREAD_UNINT, absolute);
    int error = !lmq_started || lmq_draining ? LM_ENODEV :
        lmq_count == LMQ_QUEUE_DEPTH || lmq_now() >= deadline_ns ? LM_ETIMEDOUT : LM_OK;
    lck_mtx_unlock(&lmq_lock);
    return error;
}
