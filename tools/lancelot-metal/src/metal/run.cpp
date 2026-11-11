/*
 * A command buffer's work, run when it is committed -- Metal's rule, kept: the recorded commands run here and not
 * when they were encoded, in the order they were encoded.
 */
#include "rwm.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace {

class submission_observer {
	const struct rwm_air *air = rwm_air();
	uint64_t epoch = 0, accepted_before = 0;
public:
	submission_observer() {
		if (air->submit_status) (void)air->submit_status(&epoch, &accepted_before);
	}
	bool failed(uint64_t *seq, uint64_t *waited, char *why, size_t size) {
		if (!air->submit_status) return false;
		uint64_t now = 0, accepted = 0;
		int error = air->submit_status(&now, &accepted);
		if (now == epoch) return false;
		/* Some earlier batches may already be using the command's textures.
		 * Retire those before the error path releases sampled IOSurfaces. */
		int retirement = 0;
		if (accepted != accepted_before && accepted) {
			uint64_t start = rwm_ns();
			retirement = air->wait_seq ? air->wait_seq(accepted, 5000000000ll) : ENOTSUP;
			*waited += rwm_ns() - start;
		}
		*seq = 0; /* a rejected buffer cannot publish a successful completion */
		snprintf(why, size, "Mesa submission rejected (error %d, accepted seq %llu, retirement %d)",
		    error ? error : EIO, (unsigned long long)accepted, retirement);
		return true;
	}
};

/* the first, second, fourth... time a site says it, with the count after the first (the plug-in's RW_AGAIN) */
bool
again(unsigned long &times)
{
	++times;
	return (times & (times - 1)) == 0;
}

void
say_again(unsigned long &times, const char *what, const char *why)
{
	if (again(times)) {
		rwm_print("%s: %s\n", what, why);
		if (times > 1) {
			rwm_print("  ... and that is the %lu'th time\n", times);
		}
	}
}

/* the targets of a command that are over a surface, once each */
void
note_drawn(const struct rwm_work *w, struct rwm_texture **drawn, unsigned &ndrawn, unsigned room)
{
	for (unsigned i = 0; i < w->target_count; i++) {
		struct rwm_texture *t = w->targets[i];
		unsigned k;

		if (t == nullptr || !rwm_texture_has_surface(t)) {
			continue;
		}
		for (k = 0; k < ndrawn && drawn[k] != t; k++) {
		}
		if (k == ndrawn && ndrawn < room) {
			drawn[ndrawn++] = t;
		}
	}
}

} // namespace

/* a texture over another process's surface brought up to date, counted in rwm_perf (also -[RedwoodMetalTexture
 * redwoodCopyInNow]) */
extern "C" void
rwm_copy_in(struct rwm_texture *texture)
{
	static unsigned long times;
	char why[256] = { 0 };
	size_t bytes = 0;
	const uint64_t began = rwm_ns();

	if (rwm_texture_copy_in(texture, &bytes, why, sizeof(why)) != 0) {
		say_again(times, "copy-in: the surface could not be written into its texture", why);
	}
	if (bytes != 0) {
		__atomic_fetch_add(&rwm_perf.copyins,1,__ATOMIC_RELAXED);
		__atomic_fetch_add(&rwm_perf.copyin_bytes,bytes,__ATOMIC_RELAXED);
		__atomic_fetch_add(&rwm_perf.copyin_ns,rwm_ns() - began,__ATOMIC_RELAXED);
	}
}

/* at -commit with the driver's worker: the surfaces the works sample copied in as they are now */
extern "C" void
rwm_works_copy_in(struct rwm_work *const *works, unsigned count)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned i = 0; i < works[n]->copy_in_count; i++) {
			rwm_copy_in(works[n]->copy_in[i]);
		}
	}
}

extern "C" int
rwm_works_copy_in_checked(struct rwm_work *const *works, unsigned count, char *why, size_t why_size)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned i = 0; i < works[n]->copy_in_count; i++) {
			size_t bytes = 0;
			uint64_t began = rwm_ns();
			if (rwm_texture_copy_in(works[n]->copy_in[i], &bytes, why, why_size) != 0) {
				return -1;
			}
			if (bytes != 0) {
				__atomic_fetch_add(&rwm_perf.copyins,1,__ATOMIC_RELAXED);
				__atomic_fetch_add(&rwm_perf.copyin_bytes,bytes,__ATOMIC_RELAXED);
				__atomic_fetch_add(&rwm_perf.copyin_ns,rwm_ns() - began,__ATOMIC_RELAXED);
			}
		}
	}
	return 0;
}

extern "C" int
rwm_run_works(struct rwa_device *device, struct rwm_work *const *works, unsigned count, bool copy, bool finish,
	      uint64_t *seq, uint64_t *waited_ns, char *why, size_t why_size)
{
	struct rwm_texture *drawn[64];
	unsigned ndrawn = 0;

	*waited_ns = 0;
	*seq = 0;
	submission_observer submissions;
	for (unsigned n = 0; n < count; n++) {
		struct rwm_work *w = works[n];

		if (copy && rwm_works_copy_in_checked(&w, 1, why, why_size) != 0) {
			if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
			return RWM_RUN_COPYIN_FAILED;
		}
		if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
		switch (w->kind) {
		case RWM_WORK_BLIT:
			if (rwm_run_blit(&w->blit, why, why_size) != 0) {
				if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
				static unsigned long times;

				say_again(times, "a copy failed", why);
				return RWM_RUN_BLIT_FAILED;
			} else {
				note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			}
			why[0] = '\0';
			break;
		case RWM_WORK_DRAW: {
			int failed;

			__atomic_fetch_add(&rwm_perf.draws,1,__ATOMIC_RELAXED);
			failed = rwm_run_draw(&w->draw, why, why_size);
			if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
			if (failed == -2) {
				rwm_print("the draw failed: %s\n", why);
			}
			if (failed != 0) {
				/*
				 * A buffer the shader is declared to read and the window server did not bind: QuartzCore's
				 * specialized uber shader keeps its configuration buffer (0) among its arguments although the
				 * constants decide it is never read. The draw is skipped and the rest of the frame drawn --
				 * failing the whole command buffer for it left every frame black (24 October).
				 */
				if (strstr(why, "reads buffer") != nullptr && strstr(why, "were bound") != nullptr) {
					static unsigned long times;

					say_again(times, "a draw skipped, the frame kept", why);
					why[0] = '\0';
					break;
				}
				return RWM_RUN_DRAW_FAILED;
			}
			note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			break;
		}
		case RWM_WORK_DISPATCH:
			if (rwm_run_dispatch(&w->dispatch, why, why_size) != 0) {
				if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
				rwm_print("the dispatch failed: %s\n", why);
				return RWM_RUN_DISPATCH_FAILED;
			}
			note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			break;
		}
		if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
	}

	/*
	 * Sent off and NOT waited for when the kernel queues the Mali's jobs: the window server goes on to its next frame
	 * while the GPU draws this one; the display waits for the surface's last job before it shows it, the completed
	 * handlers and -waitUntilCompleted for the buffer's. A surface drawn by the copy-back road still needs the frame
	 * now, and waits.
	 */
	const uint64_t began = rwm_ns();

	if (rwm_submit(device, ndrawn != 0 || finish, seq, why, why_size) != 0) {
		rwm_print("THE GPU DID NOT FINISH the command buffer: %s\n", why);
		*waited_ns = rwm_ns() - began;
		(void)submissions.failed(seq, waited_ns, why, why_size);
		return RWM_RUN_SUBMIT_FAILED;
	}
	*waited_ns = rwm_ns() - began;
	if (submissions.failed(seq, waited_ns, why, why_size)) return RWM_RUN_SUBMIT_FAILED;
	for (unsigned k = 0; k < ndrawn; k++) {
		static unsigned long times;
		char reason[256] = { 0 };

		if (rwm_texture_sync_surface(drawn[k], reason, sizeof(reason)) != 0) {
			say_again(times, "copy-back", reason);
			snprintf(why, why_size, "copy-back failed: %s", reason);
			return RWM_RUN_COPYBACK_FAILED;
		}
	}
	return 0;
}
