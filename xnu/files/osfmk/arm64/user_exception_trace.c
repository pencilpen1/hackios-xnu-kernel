/*
 * Bring-up aid: with boot-arg systrace set, describe every fatal-looking
 * exception a user thread takes, before it is delivered. There is no crash
 * reporter in this system, so this is the only place a crash is recorded.
 *
 * Besides the registers it prints the shared cache slide (most crashes are
 * inside system libraries: subtract the slide and look the address up in
 * the cache with "ipsw dyld a2s"), any text a register points at (the
 * library crash helpers leave their message in one) and the return
 * addresses found by walking the frame pointers.
 */

#include <arm/thread.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/exception_types.h>
#include <mach/machine/thread_status.h>
#include <string.h>
#include <os/atomic_private.h>
#include <pexpert/arm64/board_config.h>
#include <arm64/lancelot_sgi_observation.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>

extern char *proc_name_address(struct proc *p);

void user_exception_trace(exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t count, thread_t thread);

static TUNABLE(uint32_t, user_exception_trace_enabled, "systrace", 0);

static bool
plausible_user_address(uint64_t address)
{
	return address >= 0x100000000ULL && address < MACH_VM_MAX_ADDRESS;
}

void
user_exception_trace(exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t count, thread_t thread)
{
	static const unsigned message_registers[] = { 0, 1, 2, 3, 8, 16 };
	const arm_saved_state_t *state;
	vm_shared_region_t region;
	uint64_t frame[2], fp;
	uint32_t slide = 0;
	task_t task;
	struct proc *proc;

	if (!user_exception_trace_enabled || thread != current_thread()) {
		return;
	}
	if (exception != EXC_BAD_ACCESS && exception != EXC_BAD_INSTRUCTION && exception != EXC_ARITHMETIC &&
	    exception != EXC_BREAKPOINT) {
		return;
	}
	task = get_threadtask(thread);
	state = get_user_regs(thread);
	if (task == kernel_task || state == NULL || !is_saved_state64(state)) {
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		vm_shared_region_deallocate(region);
	}
	proc = get_bsdtask_info(task);

	printf("usertrap: %s[%d] exception %d code 0x%llx 0x%llx: pc 0x%llx lr 0x%llx sp 0x%llx, shared cache slide 0x%x\n",
	    proc ? proc_name_address(proc) : "?", proc ? proc_pid(proc) : -1, exception,
	    count > 0 ? (unsigned long long)code[0] : 0, count > 1 ? (unsigned long long)code[1] : 0,
	    (unsigned long long)get_saved_state_pc(state), (unsigned long long)get_saved_state_lr(state),
	    (unsigned long long)get_saved_state_sp(state), slide);

	for (unsigned i = 0; i < sizeof(message_registers) / sizeof(message_registers[0]); i++) {
		uint64_t value = (uint64_t)get_saved_state_reg(state, message_registers[i]);
		char text[128];
		vm_size_t length = 0;
		bool printable = true;

		if (!plausible_user_address(value)) {
			continue;
		}
		bzero(text, sizeof(text));
		(void)copyinstr((user_addr_t)value, text, sizeof(text) - 1, &length);
		for (unsigned c = 0; c < 6; c++) {
			printable = printable && text[c] >= 0x20 && text[c] < 0x7f;
		}
		if (printable) {
			printf("usertrap:   x%u -> \"%s\"\n", message_registers[i], text);
		}
	}

	fp = (uint64_t)get_saved_state_fp(state);
	for (int depth = 0; depth < 10 && plausible_user_address(fp) && (fp & 7) == 0; depth++) {
		if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
			break;
		}
		printf("usertrap:   called from 0x%llx\n", (unsigned long long)frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
}

/*
 * A program calls abort(): say which, and from where. The reason is usually an exception nobody
 * caught, or an assertion; the text of it goes to the program's standard error (boot-arg applog=1
 * puts that on the console), the stack is here.
 */
void user_abort_trace(void);

void
user_abort_trace(void)
{
	thread_t thread = current_thread();
	task_t task = get_threadtask(thread);
	const arm_saved_state_t *state = get_user_regs(thread);
	struct proc *proc = get_bsdtask_info(task);
	vm_shared_region_t region;
	uint64_t frame[2], fp;
	uint32_t slide = 0;

	if (task == kernel_task || state == NULL || !is_saved_state64(state)) {
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		vm_shared_region_deallocate(region);
	}
	printf("userabort: %s[%d] aborts, shared cache slide 0x%x; called from:", proc ? proc_name_address(proc) : "?",
	    proc ? proc_pid(proc) : -1, slide);
	fp = (uint64_t)get_saved_state_fp(state);
	for (int depth = 0; depth < 24 && plausible_user_address(fp) && (fp & 7) == 0; depth++) {
		if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
			break;
		}
		printf(" 0x%llx", (unsigned long long)frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	printf("\n");
}

/*
 * Boot-args ustacks=NAME ustacks_at=SECONDS [ustacks_ms=MILLISECONDS]: that long after boot
 * (and then periodically) print where every thread of every process called NAME is -- pc, lr
 * and the return addresses up the stack. Shows what a daemon that "does nothing" waits for.
 * ustacks_now(NAME) does it at once: under QEMU, "qemu/touch.py stacks NAME" on the host.
 * The addresses are symbolized on the host (qemu/symstack.py).
 * The name "top" asks for something else: who has had the processor (see ustacks_top()).
 */
#include <kern/recount.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/queue.h>
#include <pexpert/pexpert.h>

extern queue_head_t tasks;
extern lck_mtx_t tasks_threads_lock;

#define USTACKS_MAX_THREADS 96
#define USTACKS_MAX_FRAMES  14

static char ustacks_name[32];
static uint32_t ustacks_every;
static uint32_t ustacks_threads = USTACKS_MAX_THREADS;  /* boot-arg ustacks_threads=N: only the first N (1: the main thread) */
/*
 * Boot-arg ustacks_tname=TEXT: only the threads whose name has TEXT in it (ustacks_tname=coreanimation:
 * the window server's render-server and its other threads), each line with the thread's name and, in
 * the first, the time in microseconds of mach_absolute_time (as the "tl:" lines of tools/latency.py);
 * the lines are few enough then to go to the loader's log as well, where all of a long run is kept.
 * ustacks_tname=* takes every thread, named or not, and sends them to the loader's log too.
 */
static char ustacks_tname[32];
static uint32_t ustacks_count;          /* boot-arg ustacks_count=N: so many times, then no more (0: for ever) */
static thread_call_t ustacks_call;

extern void bsd_getthreadname(void *uth, char *buffer);
extern void loader_log_line(const char *line);

static void
ustacks_say(const char *line)
{
	printf("%s", line);
	if (ustacks_tname[0] != 0) {
		loader_log_line(line);
	}
}

struct ustacks_snapshot {
	uint64_t tid, pc, lr, fp;
	uint64_t syscall, a0, a1, a2, wait_event;
	uint32_t compact_tid;
	int state;
	char name[64];          /* (bsd_getthreadname() copies MAXTHREADNAMESIZE, 64) */
};

static void
ustacks_task_sample(task_t task, struct proc *proc, struct ustacks_snapshot *snap,
    unsigned capacity, unsigned thread_limit, uint32_t wanted_owner)
{
	char line[512];
	int at;
	vm_shared_region_t region;
	vm_map_t map, old_map;
	thread_t thread;
	uint32_t slide = 0;
	unsigned count = 0;
	struct { uint64_t tid, wait_event; uint32_t ctid; int state; bool user_regs; char name[64]; } owner = {0};

	task_lock(task);
	queue_iterate(&task->threads, thread, thread_t, task_threads) {
		const arm_saved_state_t *state = get_user_regs(thread);
		if (wanted_owner != 0 && thread_get_ctid(thread) == wanted_owner) {
			owner.tid = thread_tid(thread); owner.ctid = wanted_owner;
			owner.state = thread->state; owner.wait_event = thread->wait_event;
			owner.user_regs = state != NULL && is_saved_state64(state);
			thread_get_thread_name(thread, owner.name); owner.name[63] = 0;
		}

		if (count == capacity || count >= thread_limit || state == NULL || !is_saved_state64(state)) {
			continue;
		}
		snap[count].name[0] = 0;
		if (get_bsdthread_info(thread) != NULL) {
			bsd_getthreadname(get_bsdthread_info(thread), snap[count].name);
			snap[count].name[63] = 0;
		}
		if (ustacks_tname[0] != 0 && strcmp(ustacks_tname, "*") != 0 &&
		    strnstr(snap[count].name, ustacks_tname, sizeof(snap[count].name)) == NULL) {
			continue;
		}
		snap[count].tid = thread_tid(thread);
		snap[count].compact_tid = thread_get_ctid(thread);
		snap[count].state = thread->state;
		snap[count].pc = get_saved_state_pc(state);
		snap[count].lr = get_saved_state_lr(state);
		snap[count].fp = get_saved_state_fp(state);
		snap[count].syscall = get_saved_state_reg(state, 16);
		snap[count].a0 = get_saved_state_reg(state, 0);
		snap[count].a1 = get_saved_state_reg(state, 1);
		snap[count].a2 = get_saved_state_reg(state, 2);
		snap[count].wait_event = thread->wait_event;
		count++;
	}
	map = task->map;
	if (map != VM_MAP_NULL) {
		vm_map_reference(map);
	}
	task_unlock(task);
	if (owner.ctid != 0) {
		snprintf(line, sizeof(line), "ustacks-owner: %s[%d] thread=0x%llx compact_tid=%u "
		    "state=%#x wait_event=0x%llx user_regs=%d name=%s owner_may_change=1\n",
		    proc_name_address(proc), proc_pid(proc), owner.tid, owner.ctid,
		    owner.state, owner.wait_event, owner.user_regs, owner.name[0] ? owner.name : "(unnamed)");
		ustacks_say(line);
	}
	if (map == VM_MAP_NULL) {
		return;
	}
	if (count == 0) {
		vm_map_deallocate(map);
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		vm_shared_region_deallocate(region);
	}
	clock_sec_t secs;
	clock_usec_t usecs;

	uint64_t nanos;

	clock_get_system_microtime(&secs, &usecs);
	absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
	snprintf(line, sizeof(line), "ustacks: %s[%d] %u threads, shared cache slide 0x%x, uptime %lu.%03u, at %llu\n",
	    proc_name_address(proc), proc_pid(proc), count, slide, (unsigned long)secs, (unsigned)usecs / 1000, nanos / 1000);
	ustacks_say(line);

	old_map = vm_map_switch(map);
	for (unsigned i = 0; i < count; i++) {
		uint64_t frame[2], fp = snap[i].fp;

		/* (a thread that runs shows where it last came into the kernel, not where it is) */
		at = snprintf(line, sizeof(line), "ustacks:  thread 0x%llx%s%s%s%s: 0x%llx 0x%llx", (unsigned long long)snap[i].tid,
		    snap[i].name[0] ? " \"" : "", snap[i].name, snap[i].name[0] ? "\"" : "",
		    (snap[i].state & TH_RUN) ? " (running)" : "", (unsigned long long)snap[i].pc, (unsigned long long)snap[i].lr);
		for (int depth = 0; depth < USTACKS_MAX_FRAMES && plausible_user_address(fp) && (fp & 7) == 0; depth++) {
			if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
				break;
			}
			if (at < (int)sizeof(line) - 24) {
				at += snprintf(line + at, sizeof(line) - at, " 0x%llx", (unsigned long long)frame[1]);
			}
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
		snprintf(line + at, sizeof(line) - at, "\n");
		ustacks_say(line);
		snprintf(line, sizeof(line), "ustacks-state: %s[%d] thread=0x%llx compact_tid=%u state=%#x "
		    "syscall_raw=0x%llx a0=0x%llx a1=0x%llx a2=0x%llx wait_event=0x%llx\n",
		    proc_name_address(proc), proc_pid(proc), snap[i].tid, snap[i].compact_tid, snap[i].state,
		    snap[i].syscall, snap[i].a0, snap[i].a1, snap[i].a2, snap[i].wait_event);
		ustacks_say(line);
	}
	vm_map_switch(old_map);
	vm_map_deallocate(map);
}

static void
ustacks_task(task_t task, struct proc *proc)
{
	static struct ustacks_snapshot snap[USTACKS_MAX_THREADS];
	ustacks_task_sample(task, proc, snap, USTACKS_MAX_THREADS, ustacks_threads, 0);
}

/*
 * ustacks_now("top"): the processes that have had the processor since this was last asked (the
 * first time: since boot), each with its share of that time. A system that feels slow is either
 * busy -- this says with what -- or waiting for something, and the two look the same on the screen.
 */
#define USTACKS_TOP_TASKS 512
#define USTACKS_TOP_LINES 12

static void
ustacks_top(void)
{
	static struct {
		int pid;
		uint64_t total;                 /* user + system time so far, in Mach time units */
		uint64_t used;                  /* ... of it since the last time */
		char name[33];                  /* as long as a process name gets (2 * MAXCOMLEN + 1) */
	} now[USTACKS_TOP_TASKS];
	static struct {
		int pid;
		uint64_t total;
	} before[USTACKS_TOP_TASKS];
	static unsigned before_count;
	static uint64_t before_when;
	uint64_t when = mach_absolute_time(), elapsed = when - before_when, elapsed_ns, busy = 0;
	unsigned count = 0;
	task_t task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		struct proc *proc = get_bsdtask_info(task);
		struct recount_times_mach times;

		if (count == USTACKS_TOP_TASKS || proc == NULL) {
			continue;
		}
		times = recount_task_times(task);
		now[count].pid = proc_pid(proc);
		now[count].total = times.rtm_user + times.rtm_system;
		strlcpy(now[count].name, proc_name_address(proc), sizeof(now[count].name));
		count++;
	}
	lck_mtx_unlock(&tasks_threads_lock);

	/* a process that was not there the last time has used all it has since */
	for (unsigned i = 0; i < count; i++) {
		uint64_t earlier = 0;

		for (unsigned j = 0; j < before_count; j++) {
			if (before[j].pid == now[i].pid) {
				earlier = before[j].total;
				break;
			}
		}
		now[i].used = now[i].total - MIN(earlier, now[i].total);
		busy += now[i].used;
	}
	absolutetime_to_nanoseconds(elapsed, &elapsed_ns);
	printf("top: in the last %llu ms the processor was busy %llu%% of the time\n", elapsed_ns / NSEC_PER_MSEC,
	    elapsed ? busy * 100 / elapsed : 0);
	for (unsigned i = 0; i < count; i++) {
		before[i].pid = now[i].pid;
		before[i].total = now[i].total;
	}
	before_count = count;
	before_when = when;
	for (unsigned line = 0; line < USTACKS_TOP_LINES && count != 0 && elapsed != 0; line++) {
		unsigned most = 0;

		for (unsigned i = 1; i < count; i++) {
			if (now[i].used > now[most].used) {
				most = i;
			}
		}
		if (now[most].used * 1000 / elapsed == 0) {
			break;
		}
		printf("top:  %3llu.%llu%%  %s[%d]\n", now[most].used * 100 / elapsed, now[most].used * 1000 / elapsed % 10,
		    now[most].name, now[most].pid);
		now[most].used = 0;
	}
}

static void
ustacks_fire(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static task_t found[8];
	unsigned count = 0;
	task_t task;

	if (strcmp(ustacks_name, "top") == 0) {
		ustacks_top();
		goto again;
	}
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		struct proc *proc = get_bsdtask_info(task);

		if (count < 8 && proc != NULL && strncmp(proc_name_address(proc), ustacks_name, sizeof(ustacks_name)) == 0) {
			task_reference(task);
			found[count++] = task;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	if (count == 0) {
		printf("ustacks: no process called %s\n", ustacks_name);
	}
	for (unsigned i = 0; i < count; i++) {
		struct proc *proc = get_bsdtask_info(found[i]);

		if (proc != NULL) {
			ustacks_task(found[i], proc);
		}
		task_deallocate(found[i]);
	}
again:
	if (ustacks_count != 0 && --ustacks_count == 0) {
		return;
	}
	if (ustacks_every != 0) {
		uint64_t deadline;

		clock_interval_to_deadline(ustacks_every, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(ustacks_call, deadline);
	}
}

/* The same at once, for whoever asks (a debugging key sequence, see bsd/dev/redwood_virtio_input.c). */
void ustacks_now(const char *name);

void
ustacks_now(const char *name)
{
	strlcpy(ustacks_name, name, sizeof(ustacks_name));
	if (ustacks_call == NULL) {
		ustacks_call = thread_call_allocate(ustacks_fire, NULL);
	}
	thread_call_enter(ustacks_call);
}

static void
ustacks_init(void)
{
	uint32_t at = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("ustacks", ustacks_name, sizeof(ustacks_name)) ||
	    !PE_parse_boot_argn("ustacks_at", &at, sizeof(at)) || at == 0) {
		return;
	}
	PE_parse_boot_argn("ustacks_ms", &ustacks_every, sizeof(ustacks_every));
	PE_parse_boot_argn("ustacks_threads", &ustacks_threads, sizeof(ustacks_threads));
	PE_parse_boot_argn("ustacks_tname", ustacks_tname, sizeof(ustacks_tname));
	PE_parse_boot_argn("ustacks_count", &ustacks_count, sizeof(ustacks_count));
	ustacks_call = thread_call_allocate(ustacks_fire, NULL);
	clock_interval_to_deadline(at, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(ustacks_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, ustacks_init);

/* One observation for the native data-volume bring-up. It uses the existing
 * saved-user-state reader, leaves all CPUs and GPU policy running, and never
 * schedules another pass. Explicit ustacks boot arguments retain their own
 * observer and suppress this automatic diagnostic. */
extern void lancelot_data_startup_diagnostics(void);
extern uint32_t lancelot_data_current_mutex_state(void);
uint32_t lancelot_mmc_mutex_state(const void *address);
uint32_t
lancelot_mmc_mutex_state(const void *address)
{
#if !LCK_MTX_USE_ARCH
	const lck_mtx_t *lock = address;
	return os_atomic_load(&lock->lck_mtx.data, relaxed);
#else
	(void)address;
	return UINT32_MAX;
#endif
}

extern uint64_t gic_v3_irq_peek_cpu(unsigned cpu);
extern uint32_t loader_log_is_busy(void);
size_t lancelot_mmc_format_irq(char *,size_t);
size_t
lancelot_mmc_format_irq(char *out,size_t capacity)
{
    struct cpu_irq_sample {
        cpu_data_t *cpu;
        thread_t active;
        void *state;
        uint64_t iar;
        uint32_t ast,signal;
        bool running;
    } samples[8]={0};
    uint32_t initial_mutex=lancelot_data_current_mutex_state();
    uint32_t owner=initial_mutex==UINT32_MAX?0:initial_mutex&0x0fffffffu;
    /* The unsafe lookup result is used only for pointer comparison. Never
     * dereference a thread whose lifetime has not been pinned. */
    thread_t candidate=owner?ctid_get_thread_unsafe((ctid_t)owner):THREAD_NULL;
    unsigned owner_mask=0,irq_mask=0,chosen=0xff,flags=0;
    for(unsigned i=0;i<8&&i<MAX_CPUS;i++) {
        cpu_data_t *cpu=cpu_datap(i);
        if(!cpu)continue;
        samples[i].cpu=cpu;
        samples[i].active=os_atomic_load(&cpu->cpu_active_thread,relaxed);
        samples[i].state=os_atomic_load(&cpu->cpu_int_state,relaxed);
        samples[i].ast=os_atomic_load(&cpu->cpu_pending_ast,relaxed);
        samples[i].signal=os_atomic_load(&cpu->cpu_signal,relaxed);
        samples[i].running=os_atomic_load(&cpu->cpu_running,relaxed);
#if defined(GENERIC_GIC_V3_IRQ)
        samples[i].iar=gic_v3_irq_peek_cpu(i);
#endif
        if(candidate&&samples[i].active==candidate)owner_mask|=1u<<i;
        if(samples[i].state||(samples[i].iar&(1ull<<63)))irq_mask|=1u<<i;
    }
    for(unsigned i=0;i<8;i++)if(owner_mask&(1u<<i)){chosen=i;break;}
    if(chosen==0xff)for(unsigned i=0;i<8;i++)if(irq_mask&(1u<<i)){chosen=i;flags|=0x10;break;}
    uint64_t pc=0;uint32_t ast=0,signal=0,intid=0x3ff;
    bool consistent=true;
    if(chosen<8) {
        struct cpu_irq_sample *sample=&samples[chosen];
        ast=sample->ast;signal=sample->signal;
        if(owner_mask&(1u<<chosen))flags|=8;
        if(sample->running)flags|=0x20;
        if(sample->iar&(1ull<<63)) {
            flags|=1;intid=(uint32_t)(sample->iar&0xffffff);
            if(intid>=1020)intid=0x3ff;
        }
        if(sample->state) {
            flags|=2;arm_saved_state_t saved={0};
            /* The saved interrupt frame can live on a thread's wired stack.
             * A no-fault copy avoids following an unmapped or changed frame. */
            if(ml_nofault_copy((vm_offset_t)sample->state,(vm_offset_t)&saved,sizeof(saved))==sizeof(saved)&&is_saved_state64(&saved)) {
                pc=get_saved_state_pc(&saved);flags|=4;
            }else flags|=0x80;
        }
        consistent=sample->active==os_atomic_load(&sample->cpu->cpu_active_thread,relaxed)&&
            sample->state==os_atomic_load(&sample->cpu->cpu_int_state,relaxed);
#if defined(GENERIC_GIC_V3_IRQ)
        consistent=consistent&&sample->iar==gic_v3_irq_peek_cpu(chosen);
#endif
    }
    consistent=consistent&&initial_mutex!=UINT32_MAX&&owner==(lancelot_data_current_mutex_state()&0x0fffffffu);
    if(loader_log_is_busy())flags|=0x40;
    /* Advisory boundary comparison, not an atomic CPU-wide stackshot. With
     * 28-bit owners, eight CPUs, 32-bit AST/signals, a 64-bit PC and 10-bit
     * interrupt IDs, all ten hexadecimal fields fit in 60 payload bytes. */
    int n=snprintf(out,capacity,"%x,%x,%x,%x,%x,%x,%x,%llx,%x,%x",
        consistent,owner,owner_mask,irq_mask,chosen,ast,signal,pc,intid,flags);
    return n>0&&(size_t)n<capacity&&n<=60?(size_t)n:0;
}

size_t lancelot_mmc_format_sgi(unsigned,char *,size_t);
extern _Atomic int debugger_cpu;
extern debugger_op debugger_current_op;
extern boolean_t debugger_allcpus_halted,debugger_safe_to_return;
extern char *debug_buf_base,*debug_buf_ptr;
extern unsigned int debug_buf_size;
extern uint32_t loader_log_is_linear(void);
size_t lancelot_debug_format(char *,size_t);
size_t
lancelot_debug_format(char *out,size_t capacity)
{
    if(!out||!capacity)return 0;
    int cpu=os_atomic_load(&debugger_cpu,relaxed);
    unsigned op=os_atomic_load(&debugger_current_op,relaxed);
    unsigned entries=os_atomic_load(&kernel_debugger_entry_count,relaxed);
    uintptr_t base=(uintptr_t)os_atomic_load(&debug_buf_base,relaxed);
    uintptr_t end=(uintptr_t)os_atomic_load(&debug_buf_ptr,relaxed);
    unsigned size=os_atomic_load(&debug_buf_size,relaxed),bytes=0;
    unsigned flags=(panic_active()?1:0)|(os_atomic_load(&debugger_allcpus_halted,relaxed)?2:0)|
        (os_atomic_load(&debugger_safe_to_return,relaxed)?4:0)|
        (!os_atomic_load(&not_in_kdp,relaxed)?8:0)|(loader_log_is_linear()?16:0);
    if(base&&end>=base&&size<=256*1024&&end-base<=size)bytes=(unsigned)(end-base);
    else if(base||end||size)flags|=64;
    int consistent=cpu==os_atomic_load(&debugger_cpu,relaxed)&&op==(unsigned)os_atomic_load(&debugger_current_op,relaxed)&&
        entries==os_atomic_load(&kernel_debugger_entry_count,relaxed)&&
        base==(uintptr_t)os_atomic_load(&debug_buf_base,relaxed)&&end==(uintptr_t)os_atomic_load(&debug_buf_ptr,relaxed);
    unsigned bounded_cpu=cpu>=0&&cpu<8?(unsigned)cpu:255;
    int n=snprintf(out,capacity,"%x,%x,%x,%x,%x,%x",consistent,bounded_cpu,op,entries,bytes,flags);
    return n>0&&(size_t)n<capacity&&n<=60?(size_t)n:0;
}
size_t
lancelot_mmc_format_sgi(unsigned cpu,char *out,size_t capacity)
{
#if defined(GENERIC_GIC_V3_IRQ)
    struct lsg_sample v={0};
    if(!out||!capacity||cpu>=8||!lancelot_sgi_read(cpu,&v))return 0;
    uint64_t now=__builtin_arm_rsr64("CNTVCT_EL0"),freq=__builtin_arm_rsr64("CNTFRQ_EL0"),age=0;
    if(v.depth) {
        if(v.began&&now>=v.began&&freq>=1000000&&freq<=1000000000) {
            uint64_t ticks=now-v.began;
            age=(ticks/freq)*1000000+(ticks%freq)*1000000/freq;
        }else v.consistent=0;
    }
    uint32_t bounded_age=age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
    /* Eight hexadecimal fields have a 60-byte maximum. Callback/argument
     * are values only: the reader never follows either pointer. */
    int n=snprintf(out,capacity,"%x,%x,%x,%x,%x,%llx,%llx,%x",
        v.consistent,cpu,v.depth>15?15:v.depth,v.stage,v.signal,v.callback,v.argument,bounded_age);
    return n>0&&(size_t)n<capacity&&n<=60?(size_t)n:0;
#else
    (void)cpu;(void)out;(void)capacity;return 0;
#endif
}

static void
lancelot_data_startup_stacks(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static const char *const wanted[] = {
		"SpringBoard", "backboardd", "runningboardd", "launchd", "logd", "logd_helper", "cfprefsd", "kernel_task"
	};
	task_t found[8], task;
	unsigned count = 0;
	printf("lancelot-startup-stacks: begin one_shot=1 maximum_tasks=8 maximum_threads_per_task=8\n");
	lancelot_data_startup_diagnostics();
	uint32_t owner_ctid = lancelot_data_current_mutex_state() & 0x0fffffffu;
	ustacks_top();
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		struct proc *proc = get_bsdtask_info(task);
		if (proc == NULL || count == 8) {
			continue;
		}
		for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
			if (strcmp(proc_name_address(proc), wanted[i]) == 0) {
				task_reference(task);
				found[count++] = task;
				break;
			}
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	struct ustacks_snapshot snap[8];
	for (unsigned i = 0; i < count; i++) {
		struct proc *proc = get_bsdtask_info(found[i]);
		if (proc != NULL) {
			ustacks_task_sample(found[i], proc, snap, 8, 8, owner_ctid);
		}
		task_deallocate(found[i]);
	}
	printf("lancelot-startup-stacks: end tasks=%u one_shot=1 reschedule=0\n", count);
}

static void
lancelot_data_startup_stacks_init(void)
{
	uint32_t enabled = 0, profile = 0;
	char explicit_name[32];
	if (PE_parse_boot_argn("ustacks", explicit_name, sizeof(explicit_name)) ||
	    !PE_parse_boot_argn("lancelot_data", &enabled, sizeof(enabled)) || enabled != 1 ||
	    !PE_parse_boot_argn("lancelot_profile", &profile, sizeof(profile)) || profile != 2) {
		return;
	}
	thread_call_t call = thread_call_allocate(lancelot_data_startup_stacks, NULL);
	if (call == NULL) {
		printf("lancelot-startup-stacks: unavailable allocation_failed=1 one_shot=1\n");
		return;
	}
	uint64_t deadline;
	clock_interval_to_deadline(20, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, lancelot_data_startup_stacks_init);
