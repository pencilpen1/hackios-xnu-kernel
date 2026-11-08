/* Actual generated preload registration code against RAM-only reference stubs. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct panfrost_bo { uint32_t handle,refs; struct { uint64_t gpu; } ptr; uint64_t size; };
struct util_dynarray { struct panfrost_bo **entries; unsigned count; };
struct pan_pool { unsigned id; };
struct panfrost_pool { struct pan_pool base; struct util_dynarray bos; };
struct panfrost_batch { unsigned seqnum,count; struct panfrost_bo *bos[8]; };
struct cache_lock { unsigned id; };
struct panfrost_device {
    struct { struct { struct cache_lock lock; struct pan_pool *pool; } rsds,shaders; } fb_preload_cache;
};
static unsigned held[2],events[16],event_count;
static int fake_lock(struct cache_lock *lock) {
    assert(!held[lock->id]);
    if(lock->id==1)assert(held[0]);
    held[lock->id]=1;events[event_count++]=lock->id+1;return 0;
}
static int fake_unlock(struct cache_lock *lock) {
    assert(held[lock->id]);
    if(lock->id==0)assert(!held[1]);
    held[lock->id]=0;events[event_count++]=lock->id+3;return 0;
}
#define pthread_mutex_lock fake_lock
#define pthread_mutex_unlock fake_unlock
#define PIPE_SHADER_FRAGMENT 1
#define util_dynarray_foreach(array,type,entry) \
    for(type *entry=(array)->entries; entry<(array)->entries+(array)->count; ++entry)
static struct panfrost_pool *to_panfrost_pool(struct pan_pool *base) {return (struct panfrost_pool *)base;}
static uint32_t panfrost_bo_handle(struct panfrost_bo *bo) {return bo->handle;}
static uint64_t panfrost_bo_size(struct panfrost_bo *bo) {return bo->size;}
static void panfrost_batch_add_bo(struct panfrost_batch *batch,struct panfrost_bo *bo,int stage) {
    assert(held[0]&&held[1]&&stage==PIPE_SHADER_FRAGMENT&&bo);
    for(unsigned i=0;i<batch->count;i++)if(batch->bos[i]==bo)return;
    assert(batch->count<8);batch->bos[batch->count++]=bo;bo->refs++;
}
#include "actual-preload-helper.h"
static void register_pools(struct panfrost_batch *batch,struct panfrost_device *dev) {
#include "actual-preload-register.h"
}
static void release_batch(struct panfrost_batch *batch) {
    for(unsigned i=0;i<batch->count;i++){assert(batch->bos[i]->refs>1);batch->bos[i]->refs--;}
    batch->count=0;
}
int main(void) {
    struct panfrost_bo bo[4]={{.handle=9,.refs=1,.ptr={0x318c000},.size=65536},
        {.handle=10,.refs=1,.ptr={0x319c000},.size=4096},
        {.handle=11,.refs=1,.ptr={0x31ac000},.size=65536},
        {.handle=12,.refs=1,.ptr={0x31bc000},.size=4096}};
    struct panfrost_bo *rsds[3]={bo,bo+2,bo}; /* Duplicate reference must be retained once. */
    struct panfrost_bo *shaders[2]={bo+1,bo+3};
    struct panfrost_pool pools[2]={ {.base={0},.bos={rsds,0}}, {.base={1},.bos={shaders,0}} };
    struct panfrost_device dev={.fb_preload_cache={.rsds={{0},&pools[0].base},.shaders={{1},&pools[1].base}}};
    struct panfrost_batch batch={.seqnum=3};
    for(unsigned count=0;count<3;count++) {
        pools[0].bos.count=count==2?3:count;pools[1].bos.count=count;
        for(unsigned repeat=0;repeat<2;repeat++) {
            event_count=0;register_pools(&batch,&dev);
            const unsigned expected[]={1,2,4,3};
            assert(event_count==4&&!memcmp(events,expected,sizeof(expected)));
            assert(!held[0]&&!held[1]);
            assert(batch.count==2*count);
            for(unsigned i=0;i<4;i++)assert(bo[i].refs==(i<2*count?2u:1u));
        }
        release_batch(&batch);
        for(unsigned i=0;i<4;i++)assert(bo[i].refs==1);
    }
    puts("PASS: actual preload helper + registration block; empty/growing RSD/shader pools, duplicate/retained BOs, RSD->shader lock order, repeated registration and exact release; no hardware");
    return 0;
}
