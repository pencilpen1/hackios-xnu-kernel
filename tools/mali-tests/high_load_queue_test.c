#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_load.h"
#define MALI_QUEUE 64u
#define KERN_SUCCESS 0
#define LCK_SLEEP_DEFAULT 0
#define THREAD_UNINT 0
typedef void *event_t;
typedef char *caddr_t;
typedef int thread_t;
typedef int (*hkmt_completion_t)(void *,uint64_t,int,bool *);
static struct {uint64_t chain,seq,queued;bool fragment;hkmt_completion_t completion;void *completion_context;int owner_pid;} mali_queue[MALI_QUEUE];
static uint32_t native_dvfs_requested;
static unsigned mali_queue_head,mali_queue_count,mali_queue_lock;
static uint64_t mali_queued_seq,now;
static bool mali_worker_started,ready=true,start_fail,fail_after_sleep;
static unsigned sleep_calls,accepted,completed,max_count,seen[1024];

static void mali_worker(void *p,int result){(void)p;(void)result;}
static int kernel_thread_start(void (*f)(void *,int),void *p,thread_t *t){(void)f;(void)p;*t=1;return start_fail?-1:0;}
static void thread_set_thread_name(thread_t t,const char *n){(void)t;(void)n;}
static void thread_deallocate(thread_t t){(void)t;}
static bool hkmt_ready(void){return ready;}
static int proc_selfpid(void){return 123;}
static uint64_t mach_absolute_time(void){return ++now;}
static void lck_mtx_lock(unsigned *p){assert(!*p);*p=1;}
static void lck_mtx_unlock(unsigned *p){assert(*p);*p=0;}
static bool lck_mtx_try_lock(unsigned *p){if(*p)return false;*p=1;return true;}
static void wakeup(caddr_t event){(void)event;}
static int done(void *context,uint64_t seq,int result,bool *safe)
{
    assert(result==0&&*safe&&seq>0&&seq<=1024);
    assert((uintptr_t)context==seq&&seen[seq-1]==0);
    seen[seq-1]++;completed++;return 0;
}
static void consume(void)
{
    assert(!mali_queue_lock&&mali_queue_count);
    unsigned at=mali_queue_head;
    bool safe=true;
    assert(mali_queue[at].owner_pid==123);
    assert(mali_queue[at].seq==completed+1);
    assert(mali_queue[at].chain==1000+mali_queue[at].seq);
    assert(mali_queue[at].completion);
    mali_queue[at].completion(mali_queue[at].completion_context,mali_queue[at].seq,0,&safe);
    mali_queue_head=(mali_queue_head+1)%MALI_QUEUE;mali_queue_count--;
}
static void lck_mtx_sleep(unsigned *p,int flags,event_t event,int interrupt)
{
    (void)flags;(void)event;(void)interrupt;
    assert(*p);*p=0;sleep_calls++;consume();
    if(fail_after_sleep)ready=false;
    *p=1;
}
#include "actual-high-load-queue.h"

static void reset(uint32_t mode)
{
    memset(mali_queue,0,sizeof(mali_queue));memset(seen,0,sizeof(seen));
    native_dvfs_requested=mode;mali_queue_head=mali_queue_count=mali_queue_lock=0;
    mali_queued_seq=now=0;mali_worker_started=start_fail=fail_after_sleep=false;
    ready=true;sleep_calls=accepted=completed=max_count=0;
}
static void stress(uint32_t mode,unsigned limit)
{
    reset(mode);
    for(unsigned i=1;i<=512;i++) {
        uint64_t seq=lancelot_hakios_mali_enqueue_complete(1000+i,i&1,done,(void *)(uintptr_t)i);
        assert(seq==i);accepted++;assert(mali_queue_count<=limit);
        if(mali_queue_count>max_count)max_count=mali_queue_count;
    }
    while(mali_queue_count)consume();
    assert(accepted==512&&completed==512&&max_count==limit&&sleep_calls==512-limit);
    for(unsigned i=0;i<512;i++)assert(seen[i]==1);
    uint32_t depth=999;
    mali_queue_lock=1;assert(!hakios_queue_depth(&depth)&&depth==999);
    mali_queue_lock=0;assert(hakios_queue_depth(&depth)&&depth==0&&!mali_queue_lock);
}
int main(void)
{
    stress(1,64);stress(2,16);
    reset(2);start_fail=true;
    assert(!lancelot_hakios_mali_enqueue_complete(1001,false,done,(void *)1)&&!mali_queued_seq&&!mali_queue_count);
    reset(2);
    for(unsigned i=1;i<=16;i++)assert(lancelot_hakios_mali_enqueue_complete(1000+i,false,done,(void *)(uintptr_t)i)==i);
    fail_after_sleep=true;
    assert(!lancelot_hakios_mali_enqueue_complete(1017,false,done,(void *)17));
    assert(mali_queued_seq==16&&mali_queue_count==15&&completed==1&&seen[16]==0&&!mali_queue_lock);
    while(mali_queue_count)consume();
    assert(completed==16);
    puts("PASS actual FIFO producer:64/16 bounds,512 wraparound submissions,exact ordered completion,failed start and revocation while blocked; nonblocking backlog read");
}
