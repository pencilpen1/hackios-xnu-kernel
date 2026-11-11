/*
 * A command buffer's life, as far as it is not Metal's objects: made, committed -- run here, or handed to the driver's
 * worker --, run, scheduled, completed, and the waits for each. Metal's own rule is kept: the recorded commands run
 * at -commit and not when they were encoded, in the order they were encoded.
 */
#include "rwm.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

/*
 * Boot-arg rwm_async=1: the driver's own thread (src/metal/worker.cpp). Off unless asked since 27 October: with the
 * worker, frames drew parts of other frames (the "breakcore": runs 236, 240, 244, the in-use counts and the waits for
 * it notwithstanding -- and runs 415 and 416, the old plug-in and the new alike), without it they are clean and no
 * slower -- 88% of busy frames within 18 ms against 74% (runs 243, 236). The GPU stays asynchronous: the next frame is
 * built while it draws this one.
 */
bool
async_wanted()
{
	static int wanted = -1;

	if (wanted < 0) {
		const struct rwm_air *air = rwm_air();

		wanted = rwm_boot_arg("rwm_async=1") && air->flush_seq != nullptr && air->surface_seq != nullptr;
	}
	return wanted;
}

/*
 * Boot-arg rwm_finish=1: every command buffer waits for the GPU to be idle before it counts as done, the display's
 * wait for the surface's last job then having nothing left to wait for. A test (27 October): if the strips go with
 * it, the job the display waits for is not the one that finishes the picture.
 */
bool
finish_wanted()
{
	static int wanted = -1;

	if (wanted < 0) {
		wanted = rwm_boot_arg("rwm_finish=1");
	}
	return wanted;
}

/*
 * The surfaces a buffer samples in place, counted in use from -commit until the GPU has read them, as Apple's GPU
 * drivers count them (src/metal/surfaces.cpp). Boot-arg rwm_inuse=0: not counted, as until 27 October.
 */
bool
inuse_wanted()
{
	static int wanted = -1;

	if (wanted < 0) {
		wanted = !rwm_boot_arg("rwm_inuse=0");
	}
	return wanted;
}

/* (the black snapshots: the first command buffer that draws into each surface, what it does) */
void
say_first_drawn(struct rwm_work *const *works, unsigned count, const char *(*label_of)(void *))
{
	static uint32_t seen_ids[64];
	static unsigned seen_n;
	uint32_t sid0 = 0;
	unsigned draws = 0, clears = 0, k;
	const char *label = "";
	float cc[4] = { 0, 0, 0, 0 };

	for (unsigned n = 0; n < count; n++) {
		const struct rwm_work *w = works[n];
		const uint32_t sid = w->kind == RWM_WORK_DRAW && w->target_surface_count != 0 ? w->target_surface[0] : 0;

		if (sid == 0 || (sid0 != 0 && sid != sid0)) {
			continue;
		}
		sid0 = sid;
		draws++;
		if (w->draw.clear_color != nullptr) {
			clears++;
			memcpy(cc, w->clear_color, sizeof(cc));
		}
		if (*label == 0) {
			const char *l = label_of != nullptr ? label_of(w->pipeline) : nullptr;

			label = l != nullptr ? l : "?";
		}
	}
	for (k = 0; sid0 != 0 && k < seen_n && seen_ids[k] != sid0; k++) {
	}
	if (sid0 != 0 && k == seen_n && seen_n < 64) {
		char line[200];

		seen_ids[seen_n++] = sid0;
		snprintf(line, sizeof(line), "!rwm surface %u first drawn: %u draws (%u clearing, to %.2f %.2f %.2f %.2f), first %s",
			 sid0, draws, clears, cc[0], cc[1], cc[2], cc[3], label);
		sysctlbyname("kern.lancelot_hakios.say", NULL, NULL, line, strlen(line));
	}
}

} // namespace

extern "C" bool
rwm_copyin_late(void)
{
	static int late = -1;

	if (late < 0) {
		late = rwm_boot_arg("rwm_copyin_late=1");
	}
	return late;
}

extern "C" void
rwm_command_begin(struct rwm_command *c)
{
	memset(c, 0, sizeof(*c));
	static uint64_t trace_next;
	c->trace_id=__atomic_add_fetch(&trace_next,1,__ATOMIC_RELAXED);
	c->born = rwm_ns();
	c->born_cpu = rwm_thread_cpu_ns();
	c->status = RWM_CB_ENQUEUED;    /* nothing is pending yet */
}

extern "C" unsigned
rwm_command_status(const struct rwm_command *c)
{
	return __atomic_load_n(&c->status, __ATOMIC_ACQUIRE);
}

extern "C" void
rwm_command_set_error(struct rwm_command *c, int error)
{
	__atomic_store_n(&c->failure_error, error != 0 ? error : EIO, __ATOMIC_RELAXED);
	__atomic_store_n(&c->status, RWM_CB_ERROR, __ATOMIC_RELEASE);
}

extern "C" bool
rwm_command_commit(struct rwm_command *c, struct rwm_work *const *works, unsigned count,
		   const char *(*label)(void *pipeline))
{
	static __thread bool bound;

	if (!rwm_worker_on()) {
		__atomic_fetch_add(&rwm_perf.made_cbs,1,__ATOMIC_RELAXED);
		__atomic_fetch_add(&rwm_perf.made_ns,rwm_ns() - c->born,__ATOMIC_RELAXED);
		__atomic_fetch_add(&rwm_perf.made_cpu,rwm_thread_cpu_ns() - c->born_cpu,__ATOMIC_RELAXED);
	}
	if (!bound) {
		bound = true;
		if (!rwm_worker_on()) {
			rwm_bind_big(1);
		}
	}
	if (inuse_wanted()) {
		rwm_works_sampled(works, count, &c->sampled);
	}
	if (!async_wanted()) {
		return false;
	}
	__atomic_store_n(&c->status, RWM_CB_COMMITTED, __ATOMIC_RELEASE);
	c->drawn.count = 0;
	if (!rwm_copyin_late()) {
		/* the surfaces this buffer samples, copied in now, as they are when it is committed: on the worker, later,
		 * the process drawing one may already be halfway into its next picture -- icons cut in two and widgets
		 * drawn as an "H" while paging, gone with rwm_async=0 (runs 234, 236, 242) */
		char why[256] = { 0 };
		if (rwm_works_copy_in_checked(works, count, why, sizeof(why)) != 0) {
			rwm_print("command buffer copy-in failed: %s\n", why);
			rwm_command_set_error(c, EIO);
			return false;
		}
	}
	say_first_drawn(works, count, label);
	rwm_works_drawn(works, count, &c->drawn);       /* (pending for the display) */
	c->queued = true;
	return true;
}

extern "C" int
rwm_command_run(struct rwm_command *c, struct rwa_device *device, struct rwm_work *const *works, unsigned count,
		char *why, size_t why_size)
{
	if (rwm_command_status(c) == RWM_CB_ERROR) {
		snprintf(why, why_size, "command buffer was rejected before execution");
		return RWM_RUN_COPYIN_FAILED;
	}
	const uint64_t began = rwm_ns();
	uint64_t began_cpu=rwm_thread_cpu_ns();
	uint64_t waited = 0;
	int ran;

	__atomic_fetch_add(&rwm_perf.recording,began - c->born,__ATOMIC_RELAXED);
	uint64_t last_end=__atomic_load_n(&rwm_perf.last_end,__ATOMIC_RELAXED);
	if (last_end != 0 && began > last_end) {
		__atomic_fetch_add(&rwm_perf.between,began - last_end,__ATOMIC_RELAXED);
	}
	__atomic_store_n(&c->status, RWM_CB_COMMITTED, __ATOMIC_RELEASE);
	/* its copies in -- here, unless the driver's worker made them at -commit --, the draws, dispatches and copies in
	 * order, the work sent off, the surfaces copied back */
	ran = rwm_run_works(device, works, count, !(rwm_worker_on() && !rwm_copyin_late()), finish_wanted(), &c->seq,
			    &waited, why, why_size);
	/* submitted; the fence is what makes it completed */
	if (ran != 0) {
		rwm_command_set_error(c, EIO);
	} else {
		__atomic_store_n(&c->status, RWM_CB_SCHEDULED, __ATOMIC_RELEASE);
	}

	const uint64_t now = rwm_ns(), took = now - began;

	rwm_perf_max(&rwm_perf.last_end,now);
	__atomic_fetch_add(&rwm_perf.cbs,1,__ATOMIC_RELAXED);
	__atomic_fetch_add(&rwm_perf.wait,waited,__ATOMIC_RELAXED);
	__atomic_fetch_add(&rwm_perf.encode,took - waited,__ATOMIC_RELAXED);
	rwm_perf_max(&rwm_perf.worst,took);
	static uint64_t last_report;
	uint64_t prior=__atomic_load_n(&last_report,__ATOMIC_RELAXED);
	if(ran || (took>=16000000 && now>=prior+1000000000 && __atomic_compare_exchange_n(&last_report,&prior,now,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))) {
		char line[256];uint64_t cpu=rwm_thread_cpu_ns();
		snprintf(line,sizeof(line),"lancelot-metal-submit: pid=%d cb=%llu seq=%llu at=%llu wall_us=%llu cpu_us=%llu wait_us=%llu works=%u error=%d",getpid(),(unsigned long long)c->trace_id,(unsigned long long)c->seq,(unsigned long long)(now/1000),(unsigned long long)(took/1000),(unsigned long long)(cpu>=began_cpu?(cpu-began_cpu)/1000:0),(unsigned long long)(waited/1000),count,ran);
		sysctlbyname("kern.lancelot_hakios.say",NULL,NULL,line,strlen(line));
	}
	rwm_perf_report(now);
	return ran;
}

extern "C" void
rwm_command_processed(struct rwm_command *c)
{
	rwm_drawn_done(&c->drawn, c->seq);
	rwm_worker_mark(&c->processed);
}

extern "C" void
rwm_command_wait_processed(struct rwm_command *c)
{
	if (!c->queued || rwm_worker_on()) {
		return;
	}
	rwm_worker_wait(&c->processed);
}

/*
 * Scheduled once it is in the worker's queue: the worker runs buffers in the order committed, and whatever reads what
 * one draws waits for it on its own -- the display for the surface's jobs, a lock of the surface likewise,
 * -waitUntilCompleted for the GPU. Waiting here for the worker to have recorded every draw (as until 26 October) held
 * CoreAnimation's next frame for the whole of a heavy one's recording: 20 of 53 late frames began within 2 ms of the
 * worker's finishing the buffer before (run 183). Boot-arg rwm_sched_wait=1: the old wait.
 */
extern "C" void
rwm_command_wait_scheduled(struct rwm_command *c)
{
	static int wait = -1;

	if (wait < 0) {
		wait = rwm_boot_arg("rwm_sched_wait=1");
	}
	if (wait || !c->queued) {
		rwm_command_wait_processed(c);
	}
}

/* The fence is the whole point: a dispatch that the GPU does not finish must not read as a result. air_run learned
 * that the expensive way (docs/air.md section 5). */
extern "C" int
rwm_command_wait_completed(struct rwm_command *c, struct rwa_device *device, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();

	if (c->queued && !c->processed && !rwm_worker_on()) {
		rwm_command_wait_processed(c);
	}
	unsigned status = rwm_command_status(c);
	if (status == RWM_CB_ERROR) {
		snprintf(why, why_size, "command buffer failed (error %d)",
			 __atomic_load_n(&c->failure_error, __ATOMIC_RELAXED));
		return -1;
	}
	if (status == RWM_CB_COMPLETED) {
		return 0;
	}
	if (status != RWM_CB_SCHEDULED) {
		snprintf(why, why_size, "command buffer has not been submitted (status %u)", status);
		return -1;
	}
	if (c->seq != 0) {
		const uint64_t from = rwm_ns();

		/* its own last job, and nothing after it */
		int error = air->wait_seq != nullptr ? air->wait_seq(c->seq, 5000000000ll) : ENOTSUP;
		__atomic_fetch_add(&rwm_perf.completed_waits,1,__ATOMIC_RELAXED);
		__atomic_fetch_add(&rwm_perf.completed_ns,rwm_ns() - from,__ATOMIC_RELAXED);
		if (error != 0) {
			snprintf(why, why_size, "GPU job %llu did not finish (error %d)",
				 (unsigned long long)c->seq, error);
			rwm_command_set_error(c, error);
			rwm_print("THE GPU DID NOT FINISH: %s\n", why);
			return -1;
		}
	} else if (air->finish == nullptr || air->finish(device, why, why_size) != 0) {
		if (air->finish == nullptr) {
			snprintf(why, why_size, "compiler library cannot wait for GPU completion");
		}
		rwm_print("THE GPU DID NOT FINISH: %s\n", why);
		rwm_command_set_error(c, EIO);
		return -1;
	}
	/* A concurrent failed wait cannot be overwritten by a successful waiter. */
	unsigned expected = RWM_CB_SCHEDULED;
	if (!__atomic_compare_exchange_n(&c->status, &expected, RWM_CB_COMPLETED, false,
					__ATOMIC_RELEASE, __ATOMIC_ACQUIRE) && expected != RWM_CB_COMPLETED) {
		snprintf(why, why_size, "command buffer completion failed concurrently");
		return -1;
	}
	return 0;
}
