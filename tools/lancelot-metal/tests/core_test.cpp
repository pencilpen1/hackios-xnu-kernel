/* Actual Metal core with a modeled Gallium/fence provider. */
#include "rwm.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <mutex>
#include <condition_variable>

struct rwa_device {};
struct rwa_buffer { size_t length; unsigned char bytes[256]; };
struct rwm_texture { bool surface; };
static rwa_device device;
static struct rwm_air air;
static int fence_error, finish_error, draw_error, blit_error, copyin_error, copyback_error;
static unsigned waits, finishes, draws, blits, copies, destroyed;
static uint64_t queued_sequence, ticks;
static uint64_t rejection_epoch, accepted_sequence;
static bool reject_draw, reject_flush;
static bool map_fails, small_buffer, async_on;
static std::mutex barrier;
static std::condition_variable changed;
static unsigned concurrent_waiters;
static bool wait_for_pair;
struct rwm_perf rwm_perf;

extern "C" const struct rwm_air *rwm_air(void) { return &air; }
extern "C" bool rwm_air_available(void) { return true; }
extern "C" void rwm_print(const char *, ...) {}
extern "C" uint64_t rwm_ns(void) { return __atomic_add_fetch(&ticks, 100, __ATOMIC_RELAXED); }
extern "C" uint64_t rwm_thread_cpu_ns(void) { return rwm_ns(); }
extern "C" void rwm_perf_report(uint64_t) {}
extern "C" bool rwm_worker_on(void) { return false; }
extern "C" void rwm_bind_big(int) {}
extern "C" bool rwm_boot_arg(const char *arg) { return async_on && !strcmp(arg, "rwm_async=1"); }
extern "C" void rwm_worker_wait(const volatile bool *) {}
extern "C" void rwm_worker_mark(volatile bool *flag) { *flag=true; }
extern "C" void rwm_works_sampled(rwm_work *const *, unsigned, rwm_sampled *) {}
extern "C" void rwm_works_drawn(rwm_work *const *, unsigned, rwm_drawn *) {}
extern "C" void rwm_drawn_done(const rwm_drawn *, uint64_t) {}
extern "C" bool rwm_texture_has_surface(const rwm_texture *t) { return t->surface; }
extern "C" int rwm_texture_copy_in(rwm_texture *, size_t *bytes, char *why, size_t n) {
    copies++;*bytes=64;if(copyin_error)snprintf(why,n,"copy-in error");return copyin_error;
}
extern "C" int rwm_texture_sync_surface(rwm_texture *, char *why, size_t n) {
    if(copyback_error)snprintf(why,n,"copy-back error");return copyback_error;
}
extern "C" int rwm_run_draw(const rwm_draw *, char *why, size_t n) {
    if(reject_draw){++rejection_epoch;accepted_sequence=42;}
    draws++;if(draw_error)snprintf(why,n,"draw error");return draw_error;
}
extern "C" int rwm_run_blit(const rwm_blit *, char *why, size_t n) {
    blits++;if(blit_error)snprintf(why,n,"blit error");return blit_error;
}
static int wait_sequence(uint64_t seq, int64_t timeout) {
    assert(seq==42 && timeout==5000000000ll);
    __atomic_add_fetch(&waits,1,__ATOMIC_RELAXED);
    if(wait_for_pair) {
        std::unique_lock<std::mutex> lock(barrier);
        unsigned number=++concurrent_waiters;
        changed.notify_all();
        changed.wait(lock,[]{return concurrent_waiters==2;});
        return number==1?ETIMEDOUT:0;
    }
    return fence_error;
}
static int finish(rwa_device *d,char *why,size_t n) {
    assert(d==&device);finishes++;if(finish_error)snprintf(why,n,"finish error");return finish_error;
}
static uint64_t flush(rwa_device *d) { assert(d==&device);if(reject_flush)++rejection_epoch;return queued_sequence; }
static int submit_status(uint64_t *epoch,uint64_t *accepted) {*epoch=rejection_epoch;*accepted=accepted_sequence;return rejection_epoch?ETIMEDOUT:0;}
static int surface_seq(uint32_t,uint64_t) { return 0; }
static int dispatch(rwa_compute *,const rwa_bindings *,const unsigned[3],const unsigned[3],char *,size_t) { return 0; }
static rwa_buffer *buffer_create(rwa_device *d,size_t n,char *,size_t) {
    assert(d==&device && n<=256);auto*b=new rwa_buffer{};b->length=small_buffer?n-1:n;return b;
}
static void *buffer_map(rwa_buffer *b) { return map_fails?nullptr:b->bytes; }
static size_t buffer_size(rwa_buffer *b) { return b->length; }
static void buffer_destroy(rwa_buffer *b) { destroyed++;delete b; }
static uint64_t buffer_gpu_address(rwa_buffer *) { return 0x60004000; }
static void reset(void) {
    fence_error=finish_error=draw_error=blit_error=copyin_error=copyback_error=0;
    waits=finishes=draws=blits=copies=destroyed=0;queued_sequence=0;
    rejection_epoch=accepted_sequence=0;reject_draw=reject_flush=false;
    map_fails=small_buffer=false;
    air={};air.wait_seq=wait_sequence;air.finish=finish;air.flush_seq=flush;
    air.surface_seq=surface_seq;air.dispatch=dispatch;
    air.submit_status=submit_status;
    air.buffer_create=buffer_create;air.buffer_map=buffer_map;air.buffer_size=buffer_size;
    air.buffer_destroy=buffer_destroy;air.buffer_gpu_address=buffer_gpu_address;
}
#include "completion_before.inc"

static void test_completion(void) {
    reset();char why[256]={};rwm_command c{};
    c.status=RWM_CB_SCHEDULED;c.seq=42;fence_error=ETIMEDOUT;
    assert(!completion_before(&c,&device,why,sizeof(why)) && c.status==RWM_CB_COMPLETED);
    c={};c.status=RWM_CB_SCHEDULED;c.seq=42;
    assert(rwm_command_wait_completed(&c,&device,why,sizeof(why))==-1);
    assert(rwm_command_status(&c)==RWM_CB_ERROR && c.failure_error==ETIMEDOUT);
    unsigned before=waits;
    assert(rwm_command_wait_completed(&c,&device,why,sizeof(why))==-1 && waits==before);
    reset();c={};c.status=RWM_CB_SCHEDULED;c.seq=42;
    assert(!rwm_command_wait_completed(&c,&device,why,sizeof(why)) && c.status==RWM_CB_COMPLETED);
    assert(!rwm_command_wait_completed(&c,&device,why,sizeof(why)) && waits==1);
    reset();c={};c.status=RWM_CB_SCHEDULED;c.seq=42;air.wait_seq=nullptr;
    assert(rwm_command_wait_completed(&c,&device,why,sizeof(why))==-1 && c.status==RWM_CB_ERROR);
    reset();c={};c.status=RWM_CB_SCHEDULED;finish_error=EIO;
    assert(rwm_command_wait_completed(&c,&device,why,sizeof(why))==-1 && c.status==RWM_CB_ERROR);
    reset();c={};rwm_command_begin(&c);
    assert(rwm_command_wait_completed(&c,&device,why,sizeof(why))==-1 && c.status==RWM_CB_ENQUEUED && !finishes);
    reset();c={};c.status=RWM_CB_SCHEDULED;c.seq=42;wait_for_pair=true;concurrent_waiters=0;
    std::thread a([&]{char reason[100];rwm_command_wait_completed(&c,&device,reason,sizeof(reason));});
    std::thread b([&]{char reason[100];rwm_command_wait_completed(&c,&device,reason,sizeof(reason));});
    a.join();b.join();wait_for_pair=false;
    assert(rwm_command_status(&c)==RWM_CB_ERROR);
    puts("PASS completed command wait: old timeout falsely completes; current timeout/error/missing provider/unsubmitted and concurrent waiters stay accurate");
}

static void test_run(void) {
    reset();char why[256]={};uint64_t seq=99,waited=0;
    rwm_work first{},second{};first.kind=RWM_WORK_BLIT;second.kind=RWM_WORK_DRAW;
    rwm_work* works[]={&first,&second};blit_error=-1;
    assert(rwm_run_works(&device,works,2,false,false,&seq,&waited,why,sizeof(why))==RWM_RUN_BLIT_FAILED);
    assert(!draws && !finishes && !seq);
    reset();finish_error=EIO;
    assert(rwm_run_works(&device,works,2,false,false,&seq,&waited,why,sizeof(why))==RWM_RUN_SUBMIT_FAILED);
    reset();rwm_texture t{true};second.targets[0]=&t;second.target_count=1;copyback_error=-1;
    assert(rwm_run_works(&device,works,2,false,false,&seq,&waited,why,sizeof(why))==RWM_RUN_COPYBACK_FAILED);
    reset();first.copy_in[0]=&t;first.copy_in_count=1;copyin_error=-1;
    assert(rwm_run_works(&device,works,2,true,false,&seq,&waited,why,sizeof(why))==RWM_RUN_COPYIN_FAILED);
    assert(!blits && !draws);
    reset();first.copy_in_count=0;second.target_count=0;air.finish=nullptr;
    assert(rwm_submit(&device,false,&seq,why,sizeof(why))==-1);
    assert(rwm_submit(nullptr,false,&seq,why,sizeof(why))==-1);
    reset();queued_sequence=42;
    assert(!rwm_submit(&device,false,&seq,why,sizeof(why)) && seq==42 && !finishes);
    rwm_command c{};rwm_command_begin(&c);
    assert(!rwm_command_run(&c,&device,works,2,why,sizeof(why)) && rwm_command_status(&c)==RWM_CB_SCHEDULED);
    assert(!rwm_command_wait_completed(&c,&device,why,sizeof(why)) && rwm_command_status(&c)==RWM_CB_COMPLETED);
    puts("PASS actual encoded run: blit/copy-in/submit/copy-back failures stop and propagate; real async sequence keeps wait separate");
}

static void test_buffers(void) {
    reset();char why[256]={};auto*b=rwm_buffer_new(&device,0,64,why,sizeof(why));
    assert(b && rwm_buffer_contents(b) && rwm_buffer_length(b)==64 && rwm_buffer_gpu_address(b)==0x60004000);
    assert(rwm_buffer_mesa(b));rwm_buffer_free(b);assert(destroyed==1);
    assert(!rwm_buffer_new(nullptr,123,64,why,sizeof(why)) && destroyed==1);
    map_fails=true;assert(!rwm_buffer_new(&device,0,64,why,sizeof(why)) && destroyed==2);
    map_fails=false;small_buffer=true;assert(!rwm_buffer_new(&device,0,64,why,sizeof(why)) && destroyed==3);
    puts("PASS native buffer wrapper: one Mesa backend, GPU VA, allocation mapping bounds and cleanup");
}
static void test_gallium_rejection(void) {
    char why[256]={};rwm_work draw{},later{};draw.kind=RWM_WORK_DRAW;later.kind=RWM_WORK_BLIT;
    rwm_work *works[]={&draw,&later};uint64_t seq=0,waited=0;
    reset();air.submit_status=nullptr;reject_draw=true;
    assert(!rwm_run_works(&device,works,2,false,false,&seq,&waited,why,sizeof(why)) && blits==1);
    reset();reject_draw=true;rwm_command command{};rwm_command_begin(&command);
    assert(rwm_command_run(&command,&device,works,2,why,sizeof(why))==RWM_RUN_SUBMIT_FAILED);
    assert(rwm_command_status(&command)==RWM_CB_ERROR && !blits && waits==1 && !command.seq);
    assert(strstr(why,"Mesa submission rejected"));
    reset();reject_flush=true;
    assert(rwm_run_works(&device,works,1,false,false,&seq,&waited,why,sizeof(why))==RWM_RUN_SUBMIT_FAILED && !seq);
    reset();rejection_epoch=4;accepted_sequence=42;
    assert(!rwm_run_works(&device,works,1,false,false,&seq,&waited,why,sizeof(why)) && !waits);
    puts("PASS hidden Gallium rejection: old void callbacks falsely succeed; current draw/flush rejection fails Metal, drains accepted work, skips later commands, isolates old errors");
}
int main(int argc,char**) {
    async_on=argc>1;
    if(async_on) {
        reset();rwm_work w{};rwm_texture t{true};w.copy_in[0]=&t;w.copy_in_count=1;
        rwm_work*list[]={&w};rwm_command c{};rwm_command_begin(&c);copyin_error=-1;
        assert(!rwm_command_commit(&c,list,1,nullptr) && rwm_command_status(&c)==RWM_CB_ERROR);
        char why[128];assert(rwm_command_run(&c,&device,list,1,why,sizeof(why))==RWM_RUN_COPYIN_FAILED);
        puts("PASS async copy-in failure cannot execute or complete");return 0;
    }
    test_completion();test_run();test_buffers();test_gallium_rejection();
}
