/*
 * Where a frame's time goes, as kept lines every five seconds ("rwm perf", "rwm waits", "rwm making",
 * "rwm batches", "rwm draw us"): what the window server's frame rate is made of, measured rather than
 * guessed. The counters are added to where the work is (the plug-in and the core alike); the formats of
 * the lines are kept to the character. And the boot arguments, read once.
 */
#include "rwm.h"
#include "boot_policy.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

struct rwm_perf rwm_perf;

namespace {

void
say(const char *line)
{
	char tagged[512];
	int n=snprintf(tagged,sizeof(tagged),"lancelot-metal-perf: p=%d n=%.16s t=%llu %s",getpid(),getprogname(),(unsigned long long)(rwm_ns()/1000),line);
	if(n>0)sysctlbyname("kern.lancelot_hakios.say",NULL,NULL,tagged,strlen(tagged));
}

/* one of the compiler library's profile calls, if it is loaded and has it (not part of the contract) */
void *
profile_call(const char *name)
{
	void *lib = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);

	return lib != nullptr ? dlsym(lib, name) : nullptr;
}

} // namespace

extern "C" const char *
rwm_boot_args(void)
{
	static char arguments[1024];
	static int read;

	if (!read) {
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
			arguments[0] = '\0';
		}
		arguments[sizeof(arguments) - 1] = '\0';
		read = 1;
	}
	return arguments;
}

extern "C" bool
rwm_boot_arg(const char *text)
{
	return lancelot_metal_boot_token(rwm_boot_args(), text);
}

extern "C" uint64_t
rwm_thread_cpu_ns(void)
{
	thread_basic_info_data_t info;
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	mach_port_t self = mach_thread_self();
	uint64_t ns = 0;

	if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
		ns = (uint64_t)(info.user_time.seconds + info.system_time.seconds) * 1000000000ull +
		     (uint64_t)(info.user_time.microseconds + info.system_time.microseconds) * 1000ull;
	}
	mach_port_deallocate(mach_task_self(), self);
	return ns;
}

extern "C" uint64_t
rwm_ns(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom;
}

extern "C" void
rwm_perf_report(uint64_t now)
{
	uint64_t since=__atomic_load_n(&rwm_perf.since,__ATOMIC_RELAXED);
	if(!since){__atomic_compare_exchange_n(&rwm_perf.since,&since,now,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);return;}
	if(now<since || now-since<5000000000ull || !__atomic_compare_exchange_n(&rwm_perf.since,&since,now,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return;
	struct rwm_perf p={};
	p.cbs=__atomic_exchange_n(&rwm_perf.cbs,0,__ATOMIC_RELAXED);
	p.draws=__atomic_exchange_n(&rwm_perf.draws,0,__ATOMIC_RELAXED);
	p.encode=__atomic_exchange_n(&rwm_perf.encode,0,__ATOMIC_RELAXED);
	p.wait=__atomic_exchange_n(&rwm_perf.wait,0,__ATOMIC_RELAXED);
	p.worst=__atomic_exchange_n(&rwm_perf.worst,0,__ATOMIC_RELAXED);
	p.pipes=__atomic_exchange_n(&rwm_perf.pipes,0,__ATOMIC_RELAXED);
	p.compile=__atomic_exchange_n(&rwm_perf.compile,0,__ATOMIC_RELAXED);
	p.recording=__atomic_exchange_n(&rwm_perf.recording,0,__ATOMIC_RELAXED);
	p.between=__atomic_exchange_n(&rwm_perf.between,0,__ATOMIC_RELAXED);
	p.copyins=__atomic_exchange_n(&rwm_perf.copyins,0,__ATOMIC_RELAXED);
	p.copyin_bytes=__atomic_exchange_n(&rwm_perf.copyin_bytes,0,__ATOMIC_RELAXED);
	p.copyin_ns=__atomic_exchange_n(&rwm_perf.copyin_ns,0,__ATOMIC_RELAXED);
	p.queue_waits=__atomic_exchange_n(&rwm_perf.queue_waits,0,__ATOMIC_RELAXED);
	p.queue_ns=__atomic_exchange_n(&rwm_perf.queue_ns,0,__ATOMIC_RELAXED);
	p.processed_waits=__atomic_exchange_n(&rwm_perf.processed_waits,0,__ATOMIC_RELAXED);
	p.processed_ns=__atomic_exchange_n(&rwm_perf.processed_ns,0,__ATOMIC_RELAXED);
	p.completed_waits=__atomic_exchange_n(&rwm_perf.completed_waits,0,__ATOMIC_RELAXED);
	p.completed_ns=__atomic_exchange_n(&rwm_perf.completed_ns,0,__ATOMIC_RELAXED);
	p.drains=__atomic_exchange_n(&rwm_perf.drains,0,__ATOMIC_RELAXED);
	p.drain_ns=__atomic_exchange_n(&rwm_perf.drain_ns,0,__ATOMIC_RELAXED);
	p.worker_cbs=__atomic_exchange_n(&rwm_perf.worker_cbs,0,__ATOMIC_RELAXED);
	p.worker_ns=__atomic_exchange_n(&rwm_perf.worker_ns,0,__ATOMIC_RELAXED);
	p.worker_cpu=__atomic_exchange_n(&rwm_perf.worker_cpu,0,__ATOMIC_RELAXED);
	p.made_cbs=__atomic_exchange_n(&rwm_perf.made_cbs,0,__ATOMIC_RELAXED);
	p.made_ns=__atomic_exchange_n(&rwm_perf.made_ns,0,__ATOMIC_RELAXED);
	p.made_cpu=__atomic_exchange_n(&rwm_perf.made_cpu,0,__ATOMIC_RELAXED);
	char line[320];
	const uint64_t span=now-since;

	snprintf(line,sizeof(line),"cb=%llu draws=%llu wall_us=%llu wait_us=%llu max_us=%llu",p.cbs,p.draws,p.encode/1000,p.wait/1000,p.worst/1000);say(line);
	snprintf(line,sizeof(line),"window_us=%llu pipes=%llu compile_us=%llu",span/1000,p.pipes,p.compile/1000);say(line);
	snprintf(line,sizeof(line),"record_us=%llu between_us=%llu",p.recording/1000,p.between/1000);say(line);
	snprintf(line,sizeof(line),"copy_n=%llu bytes=%llu us=%llu",p.copyins,p.copyin_bytes,p.copyin_ns/1000);say(line);
	snprintf(line,sizeof(line),"queue_n=%llu us=%llu",p.queue_waits,p.queue_ns/1000);say(line);
	snprintf(line,sizeof(line),"processed_n=%llu us=%llu",p.processed_waits,p.processed_ns/1000);say(line);
	snprintf(line,sizeof(line),"completed_n=%llu us=%llu",p.completed_waits,p.completed_ns/1000);say(line);
	snprintf(line,sizeof(line),"worker_n=%llu wall_us=%llu cpu_us=%llu",p.worker_cbs,p.worker_ns/1000,p.worker_cpu/1000);say(line);
	snprintf(line,sizeof(line),"made_n=%llu wall_us=%llu cpu_us=%llu",p.made_cbs,p.made_ns/1000,p.made_cpu/1000);say(line);

	static void (*profile)(uint64_t *);
	static void (*submits)(uint64_t *);
	static void (*states)(uint64_t *);
	static int looked;

	if (!looked) {
		looked = 1;
		profile = (void (*)(uint64_t *))profile_call("rwa_draw_profile");
		submits = (void (*)(uint64_t *))profile_call("rwa_submit_profile");
		states = (void (*)(uint64_t *))profile_call("rwa_state_profile");
	}
	if (submits != nullptr) {
		uint64_t sb[5] = { 0, 0, 0, 0, 0 };

		submits(sb);
		snprintf(line, sizeof(line), "batch_n=%llu mean_us=%llu blend_n=%llu variants=%llu compile_ms=%llu", sb[0], sb[0] ? sb[1] / 1000 / sb[0] : 0, sb[2], sb[3],
			 sb[4] / 1000000);
		say(line);
	}
	if (profile != nullptr) {
		uint64_t d[7];

		profile(d);
		if (d[6] != 0) {
			snprintf(line,sizeof(line),"check_us=%llu bind_us=%llu state_us=%llu",d[0]/1000/d[6],d[1]/1000/d[6],d[2]/1000/d[6]);say(line);
			snprintf(line,sizeof(line),"resources_us=%llu draw_us=%llu unbind_us=%llu n=%llu",d[3]/1000/d[6],d[4]/1000/d[6],d[5]/1000/d[6],d[6]);say(line);
		}
	}
	if(states != nullptr) {
		uint64_t s[8]={};states(s);
		snprintf(line,sizeof(line),"fb_requests=%llu fb_reuse=%llu fb_changes=%llu switch_flushes=%llu flush_us=%llu fb_set_us=%llu destroy_unbinds=%llu",
		    s[0],s[1],s[2],s[3],s[4]/1000,s[5]/1000,s[6]);say(line);
	}

}
