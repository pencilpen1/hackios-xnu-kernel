/* Executes the same portable functions used by LancelotMali.cpp/Export.h.
 * No fake GPU, IOKit transport, VM unmap success, or hardware claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../../xnu/files/iokit/Kernel/lancelot_mali_client_logic.h"
#include "../../xnu/files/bsd/dev/lancelot_mali_core.h"
static _Atomic unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); assert(expr); } } while (0)
static union lmal_request request(uint32_t sel) {
    union lmal_request r;
    memset(&r, 0, sizeof(r));
    r.h.magic=LMAL_UAPI_MAGIC; r.h.version=LMAL_UAPI_VERSION;
    r.h.size=(uint16_t)lmal_client_message_size(sel);
    switch(sel) {
    case LMAL_SELECTOR_BO_CREATE: r.bo_create.size=4096; r.bo_create.flags=LMAL_BO_READ|LMAL_BO_WRITE|LMAL_BO_FIXED_BACKING; break;
    case LMAL_SELECTOR_SUBMIT: r.submit.jc=0x10000; r.submit.bo_count=1; r.submit.bo_handles=0x1000; r.submit.timeout_ns=LMAL_MAX_WAIT_NS; break;
    case LMAL_SELECTOR_SYNCOBJ_RESET:
    case LMAL_SELECTOR_SYNCOBJ_SIGNAL: r.sync_array.count=1; r.sync_array.handles=0x1000; break;
    case LMAL_SELECTOR_SYNCOBJ_WAIT: r.sync_wait.count=1; r.sync_wait.handles=0x1000; break;
    case LMAL_SELECTOR_SURFACE_QUERY: r.surface_query.surface_id=1; break;
    case LMAL_SELECTOR_SURFACE_IMPORT: r.surface_import.surface_id=1; break;
    default: break;
    }
    return r;
}
static void abi_tests(void) {
    const size_t sizes[]={40,64,64,32,40,88,32,32,40,40,56,80,32,32,64,88,32};
    unsigned i;
    for (i=0;i<17;++i) {
        uint32_t sel=LMAL_SELECTOR_BASE+1+i;
        union lmal_request r=request(sel), bad, output;
        size_t exact=sizes[i], n=999;
        CHECK(lmal_client_message_size(sel)==exact);
        CHECK(lmal_client_validate(sel,&r,exact,exact)==LMAL_OK);
        CHECK(lmal_client_validate(sel,&r,exact,sizeof(output))==LMAL_OK);
        CHECK(lmal_client_validate(sel,&r,exact-1,exact)==LMAL_EINVAL);
        CHECK(lmal_client_validate(sel,&r,exact+1,exact)==LMAL_EINVAL);
        CHECK(lmal_client_validate(sel,&r,exact,exact-1)==LMAL_EFAULT);
        CHECK(lmal_client_validate(sel,&r,exact,0)==LMAL_EFAULT);
        bad=r; bad.h.size--; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.size++; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.magic^=1; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.version++; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.flags=1; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.reserved0=1; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.reserved1=1; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        bad=r; bad.h.result=-1; CHECK(lmal_client_validate(sel,&bad,exact,exact)==LMAL_EINVAL);
        memset(&output,0xa5,sizeof(output));
        CHECK(lmal_client_copy_reply(&output,sizeof(output),&r,sel,LMAL_ENOTSUP,&n)==LMAL_ENOTSUP);
        CHECK(n==exact && output.h.result==LMAL_ENOTSUP && output.h.size==exact);
        for(size_t j=exact;j<sizeof(output);++j) CHECK(((unsigned char*)&output)[j]==0xa5);
        memset(&output,0xa5,sizeof(output));
        {
            unsigned char unaligned[sizeof(output)+2];
            int32_t status;
            memset(unaligned,0xa5,sizeof(unaligned));
            CHECK(lmal_client_copy_reply(unaligned+1,sizeof(output),&r,sel,LMAL_EIO,&n)==LMAL_EIO);
            memcpy(&status,unaligned+1+offsetof(struct lmal_header,result),sizeof(status));
            CHECK(status==LMAL_EIO && n==exact && unaligned[0]==0xa5 && unaligned[exact+1]==0xa5);
        }
        CHECK(lmal_client_copy_reply(&output,exact-1,&r,sel,LMAL_OK,&n)==LMAL_EFAULT);
        CHECK(n==0);
        for(size_t j=0;j<sizeof(output);++j) CHECK(((unsigned char*)&output)[j]==0xa5);
    }
    CHECK(lmal_client_message_size(0)==0);
    CHECK(lmal_client_validate(0,NULL,0,0)==LMAL_ENOTSUP);
#define BAD_MEMBER(sel,member) do { union lmal_request r=request(sel); r.member=1; CHECK(lmal_client_validate(sel,&r,r.h.size,r.h.size)==LMAL_EINVAL); } while(0)
    BAD_MEMBER(LMAL_SELECTOR_GET_PARAM,get_param.reserved);
    BAD_MEMBER(LMAL_SELECTOR_BO_CREATE,bo_create.reserved);
    BAD_MEMBER(LMAL_SELECTOR_BO_QUERY,bo_query.reserved0);
    BAD_MEMBER(LMAL_SELECTOR_BO_QUERY,bo_query.reserved1);
    BAD_MEMBER(LMAL_SELECTOR_BO_CLOSE,bo_close.reserved);
    BAD_MEMBER(LMAL_SELECTOR_BO_UNMAP,bo_close.reserved);
    BAD_MEMBER(LMAL_SELECTOR_SUBMIT,submit.reserved);
    {
        union lmal_request r=request(LMAL_SELECTOR_SUBMIT);
        unsigned effects=0;
        r.submit.timeout_ns=0;
        CHECK(lmal_client_validate(LMAL_SELECTOR_SUBMIT,&r,r.h.size,r.h.size)==LMAL_EINVAL);
        if(lmal_client_validate(LMAL_SELECTOR_SUBMIT,&r,r.h.size,r.h.size)==LMAL_OK) ++effects;
        CHECK(effects==0);
        r.submit.timeout_ns=LMAL_MAX_WAIT_NS+1;
        CHECK(lmal_client_validate(LMAL_SELECTOR_SUBMIT,&r,r.h.size,r.h.size)==LMAL_EINVAL);
        r.submit.timeout_ns=1;
        CHECK(lmal_client_validate(LMAL_SELECTOR_SUBMIT,&r,r.h.size,r.h.size)==LMAL_OK);
    }
    BAD_MEMBER(LMAL_SELECTOR_SYNCOBJ_DESTROY,sync_handle.reserved);
    BAD_MEMBER(LMAL_SELECTOR_SYNCOBJ_RESET,sync_array.reserved);
    BAD_MEMBER(LMAL_SELECTOR_SYNCOBJ_SIGNAL,sync_array.reserved);
    BAD_MEMBER(LMAL_SELECTOR_SYNCOBJ_WAIT,sync_wait.reserved1);
    BAD_MEMBER(LMAL_SELECTOR_GET_CAPS,caps.reserved);
    BAD_MEMBER(LMAL_SELECTOR_GET_CAPS,caps.flags);
    BAD_MEMBER(LMAL_SELECTOR_SURFACE_IMPORT,surface_import.reserved);
    CHECK(lmal_client_bo_flags_valid(LMAL_BO_FIXED_BACKING|LMAL_BO_HEAP|LMAL_BO_NOEXEC,0));
    CHECK(!lmal_client_bo_flags_valid(LMAL_BO_EXEC|LMAL_BO_NOEXEC,0));
    CHECK(!lmal_client_bo_flags_valid(LMAL_BO_READONLY|LMAL_BO_WRITE,0));
    CHECK(!lmal_client_bo_flags_valid(0x80000000,0));
    CHECK(!lmal_client_bo_flags_valid(LMAL_BO_EXEC,1));
    CHECK(!lmal_client_bo_flags_valid(LMAL_BO_HEAP,1));
    CHECK(lmal_client_bo_flags_valid(LMAL_BO_FIXED_BACKING|LMAL_BO_READONLY,1));
    /* Validator is the production side-effect gate, not a substitute driver. */
    for(uint32_t sel=LMAL_SELECTOR_BASE+1;sel<=LMAL_SELECTOR_BO_UNMAP;++sel) {
        union lmal_request r=request(sel); unsigned effects=0;
        if(lmal_client_validate(sel,&r,r.h.size,r.h.size-1)==LMAL_OK) ++effects;
        CHECK(effects==0);
        r.h.flags=1;
        if(lmal_client_validate(sel,&r,r.h.size,r.h.size)==LMAL_OK) ++effects;
        CHECK(effects==0);
    }
}
static void sync_tests(void) {
    struct lmal_sync s[3]={{0}};
    uint32_t h[]={1,2,1}, first; uint64_t gen[3];
    CHECK(lmal_client_sync_create(&s[0],0)==LMAL_OK);
    CHECK(lmal_client_sync_create(&s[1],1)==LMAL_OK);
    CHECK(lmal_client_wait_pin(s,3,h,gen,3)==LMAL_OK);
    CHECK(s[0].wait_pins==2 && s[1].wait_pins==1);
    CHECK(lmal_client_sync_destroy(&s[0])==LMAL_EBUSY);
    CHECK(lmal_client_wait_poll(s,h,gen,3,0,0,&first)==LMAL_OK && first==1);
    CHECK(lmal_client_wait_poll(s,h,gen,3,1,0,&first)==LMAL_EBUSY);
    s[0].signaled=1;
    CHECK(lmal_client_wait_poll(s,h,gen,3,1,0,&first)==LMAL_OK && first==0);
    CHECK(lmal_client_wait_poll(s,h,gen,3,1,1,&first)==LMAL_ENODEV && first==UINT32_MAX);
    lmal_client_wait_unpin(s,h,gen,3);
    CHECK(s[0].wait_pins==0 && s[1].wait_pins==0);
    uint64_t old=s[0].generation;
    CHECK(lmal_client_sync_destroy(&s[0])==LMAL_OK);
    CHECK(lmal_client_sync_create(&s[0],1)==LMAL_OK && s[0].generation==old+1);
    CHECK(lmal_client_wait_poll(s,h,gen,3,0,0,&first)==LMAL_EINVAL);
    h[1]=0; CHECK(lmal_client_wait_pin(s,3,h,gen,3)==LMAL_EINVAL && s[0].wait_pins==0);
    h[1]=4; CHECK(lmal_client_wait_pin(s,3,h,gen,3)==LMAL_EINVAL && s[0].wait_pins==0);
    h[1]=3; CHECK(lmal_client_wait_pin(s,3,h,gen,3)==LMAL_EINVAL && s[0].wait_pins==0);
    s[0].wait_pins=UINT32_MAX; CHECK(lmal_client_wait_pin(s,3,h,gen,1)==LMAL_EINVAL);
    s[0].wait_pins=0; s[2].generation=UINT64_MAX;
    CHECK(lmal_client_sync_create(&s[2],1)==LMAL_EBUSY && !s[2].used);
}
/* Deterministic unlocked-wait race, using production pin/poll under a host lock. */
struct race { pthread_mutex_t lock; pthread_cond_t cv; int sleeping,closing,result; struct lmal_sync s; };
static void *waiter(void *arg) {
    struct race *r=arg; uint32_t h=1,first; uint64_t gen;
    pthread_mutex_lock(&r->lock);
    CHECK(lmal_client_wait_pin(&r->s,1,&h,&gen,1)==LMAL_OK);
    r->sleeping=1; pthread_cond_broadcast(&r->cv);
    while(!r->closing) pthread_cond_wait(&r->cv,&r->lock);
    r->result=lmal_client_wait_poll(&r->s,&h,&gen,1,0,r->closing,&first);
    lmal_client_wait_unpin(&r->s,&h,&gen,1);
    pthread_mutex_unlock(&r->lock); return NULL;
}
static void race_test(void) {
    struct race r={.lock=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER}; pthread_t t;
    CHECK(lmal_client_sync_create(&r.s,0)==LMAL_OK);
    CHECK(pthread_create(&t,NULL,waiter,&r)==0);
    pthread_mutex_lock(&r.lock);
    while(!r.sleeping) pthread_cond_wait(&r.cv,&r.lock);
    CHECK(lmal_client_sync_destroy(&r.s)==LMAL_EBUSY);
    r.s.signaled=1; r.closing=1; pthread_cond_broadcast(&r.cv);
    pthread_mutex_unlock(&r.lock); CHECK(pthread_join(t,NULL)==0);
    CHECK(r.result==LMAL_ENODEV && r.s.wait_pins==0);
    pthread_mutex_destroy(&r.lock); pthread_cond_destroy(&r.cv);
}
struct map { unsigned unmaps,releases; int fail; };
static unsigned event, last_unmap, first_release;
static int unmap(void *p) { struct map *m=p; ++m->unmaps; last_unmap=++event; return m->fail?LMAL_EIO:LMAL_OK; }
static void release(void *p) { struct map *m=p; ++m->releases; if(!first_release) first_release=event+1; ++event; }
static void export_tests(void) {
    struct lmal_client_export e={0}; struct map maps[LMAL_CLIENT_EXPORT_MAPS+1]={{0}};
    CHECK(lmal_client_export_track(&e,&maps[0])==1);
    CHECK(lmal_client_export_track(&e,&maps[0])==0 && e.count==1);
    /* Map and Unmap descriptor callbacks never change this policy count. */
    CHECK(e.count==1);
    for(unsigned i=1;i<LMAL_CLIENT_EXPORT_MAPS;++i) CHECK(lmal_client_export_track(&e,&maps[i])==1);
    CHECK(lmal_client_export_track(&e,&maps[LMAL_CLIENT_EXPORT_MAPS])==-1);
    CHECK(lmal_client_export_revoke(&e,0,unmap,release)==LMAL_OK);
    CHECK(e.count==0 && !e.closed && first_release>last_unmap);
    for(unsigned i=0;i<LMAL_CLIENT_EXPORT_MAPS;++i) CHECK(maps[i].unmaps==1 && maps[i].releases==1);
    CHECK(lmal_client_export_revoke(&e,0,unmap,release)==LMAL_OK); /* repeated ACK */
    CHECK(lmal_client_export_track(&e,&maps[0])==1);
    CHECK(lmal_client_export_revoke(&e,1,unmap,release)==LMAL_OK && e.closed);
    CHECK(lmal_client_export_track(&e,&maps[1])==-1); /* escaped close descriptor */
    memset(&e,0,sizeof(e)); memset(maps,0,sizeof(maps));
    CHECK(lmal_client_export_track(&e,&maps[0])==1);
    CHECK(lmal_client_export_track(&e,&maps[1])==1); maps[1].fail=1;
    CHECK(lmal_client_export_revoke(&e,0,unmap,release)==LMAL_EIO);
    CHECK(e.closed && e.poisoned && e.count==2);
    CHECK(maps[0].releases==0 && maps[1].releases==0);
    maps[1].fail=0;
    CHECK(lmal_client_export_revoke(&e,0,unmap,release)==LMAL_EIO);
    CHECK(maps[0].releases==0 && maps[1].releases==0);
    CHECK(lmal_client_export_track(&e,&maps[2])==-1);
}
struct new_backing { unsigned completed,released,surface_dropped,step; int fail; };
static int complete_new(void *p) {
    struct new_backing *b=p;
    CHECK(b->released==0 && b->surface_dropped==0);
    ++b->completed; b->step=1;
    return b->fail ? LMAL_EIO : LMAL_OK;
}
static void release_new(void *p) {
    struct new_backing *b=p;
    CHECK(b->completed==1 && !b->fail && b->step==1);
    ++b->released; b->step=2;
}
static void drop_new_surface(void *p) {
    struct new_backing *b=p;
    CHECK(b->released==1 && b->step==2);
    ++b->surface_dropped; b->step=3;
}
static void allocation_tests(void) {
    for(unsigned i=0;i<16;++i) {
        uint32_t flags=((i&1)?LMAL_BO_READ:0)|((i&2)?LMAL_BO_WRITE:0)|
            ((i&4)?LMAL_BO_READONLY:0)|((i&8)?LMAL_BO_EXEC:0);
        uint32_t expected=((i&2)&&!(i&4)?LM_MAP_WRITE:0)|((i&8)?LM_MAP_EXEC:0);
        CHECK(lmal_client_gpu_permissions(flags,LM_MAP_WRITE,LM_MAP_EXEC)==expected);
        CHECK(lmal_client_gpu_permissions(flags|LMAL_BO_FIXED_BACKING|LMAL_BO_HEAP,
            LM_MAP_WRITE,LM_MAP_EXEC)==expected);
    }
    CHECK(lmal_client_gpu_permissions(LMAL_BO_READ,LM_MAP_WRITE,LM_MAP_EXEC)==0);
    CHECK(lmal_client_gpu_permissions(LMAL_BO_READ|LMAL_BO_EXEC,LM_MAP_WRITE,LM_MAP_EXEC)==LM_MAP_EXEC);
    CHECK(lmal_client_gpu_permissions(LMAL_BO_WRITE|LMAL_BO_EXEC,LM_MAP_WRITE,LM_MAP_EXEC)==(LM_MAP_WRITE|LM_MAP_EXEC));
    CHECK(lmal_client_gpu_permissions(LMAL_BO_WRITE|LMAL_BO_READONLY,LM_MAP_WRITE,LM_MAP_EXEC)==0);
    for(int ready=0;ready<2;++ready) for(int q=0;q<2;++q) for(int vm=0;vm<2;++vm) {
        int expected=(!ready||!vm)?LMAL_ENODEV:(q?LMAL_EBUSY:LMAL_OK);
        CHECK(lmal_client_allocation_admit(ready,q,vm)==expected);
    }
    /* Repeated ENOMEM/VM-quota rollbacks must not accumulate backing retains. */
    for(unsigned i=0;i<100;++i) {
        struct new_backing b={0};
        CHECK(lmal_client_cleanup_new_backing(&b,(i&1)?&b:NULL,1,
            complete_new,release_new,drop_new_surface)==LMAL_OK);
        CHECK(b.completed==1 && b.released==1 && b.surface_dropped==(i&1));
    }
    struct new_backing b={0};
    CHECK(lmal_client_cleanup_new_backing(&b,&b,0,complete_new,release_new,drop_new_surface)==LMAL_EBUSY);
    CHECK(b.completed==0 && b.released==0 && b.surface_dropped==0);
    b.fail=1;
    CHECK(lmal_client_cleanup_new_backing(&b,&b,1,complete_new,release_new,drop_new_surface)==LMAL_EIO);
    CHECK(b.completed==1 && b.released==0 && b.surface_dropped==0);
}
static void granule_tests(void) {
    uint64_t kernel_pages[]={4096,16384};
    for(unsigned k=0;k<2;++k) {
        uint64_t page=kernel_pages[k], cap=LMAL_MAX_BO_BYTES;
        CHECK(lmal_client_round_backing(1,page,cap)==page);
        CHECK(lmal_client_round_backing(page,page,cap)==page);
        CHECK(lmal_client_round_backing(page+1,page,cap)==2*page);
        CHECK(lmal_client_round_backing(cap-1,page,cap)==cap);
        CHECK(lmal_client_round_backing(cap,page,cap)==cap);
        CHECK(lmal_client_round_backing(cap+1,page,cap)==0);
        CHECK(lmal_client_round_backing(0,page,cap)==0);
        CHECK(lmal_client_round_backing(1,page,page-1)==0);
        CHECK(lmal_client_round_backing(page+1,page,page+1)==0);
        CHECK(lmal_client_round_backing(UINT64_MAX,page,UINT64_MAX)==0);
        CHECK(lmal_client_round_backing(UINT64_MAX-(page-1),page,UINT64_MAX)==UINT64_MAX-(page-1));
        CHECK(lmal_client_round_backing(UINT64_MAX-(page-1)+1,page,UINT64_MAX)==0);
        CHECK(lmal_client_surface_backing(page+1,page,3*page,cap)==2*page);
        CHECK(lmal_client_surface_backing(page+1,page,3*page-1,cap)==0);
        CHECK(lmal_client_surface_backing(1,page,page-1,cap)==0);
        CHECK(lmal_client_surface_backing(1,page,page,cap)==0);
        CHECK(lmal_client_surface_backing(cap,page,page+cap,cap)==cap);
        CHECK(lmal_client_surface_backing(UINT64_MAX-(page-1),page,UINT64_MAX,UINT64_MAX)==0);
        /* getPhysicalSegment is byte-offset based: sample all GPU4K subpages
         * of two noncontiguous kernel pages, never infer the next kernel PA. */
        uint64_t size=lmal_client_round_backing(page+1,page,cap);
        uint32_t count=lmal_client_gpu_page_count(size,LM_MAX_MAP_PAGES);
        uint64_t bases[]={0x200000,0x900000};
        unsigned seen[2][4]={{0}};
        CHECK(count==2*page/LMAL_PAGE_SIZE);
        for(uint32_t i=0;i<count;++i) {
            uint64_t offset=lmal_client_gpu_page_offset(i);
            uint32_t kernel_index=(uint32_t)(offset/page);
            uint64_t within=offset&(page-1);
            uint64_t pa=bases[kernel_index]+within, segment=page-within;
            CHECK(offset==(uint64_t)i*4096 && offset+4096<=size);
            CHECK(lmal_client_gpu_segment_valid(pa,segment));
            CHECK((pa&(page-1))==within);
            ++seen[kernel_index][within/4096];
        }
        for(unsigned i=0;i<2;++i) for(unsigned sub=0;sub<page/4096;++sub) CHECK(seen[i][sub]==1);
        CHECK(lmal_client_gpu_page_offset(count)==size);
    }
    CHECK(lmal_client_round_backing(1,0,LMAL_MAX_BO_BYTES)==0);
    CHECK(lmal_client_round_backing(1,8192,LMAL_MAX_BO_BYTES)==0);
    CHECK(lmal_client_round_backing(1,65536,LMAL_MAX_BO_BYTES)==0);
    CHECK(lmal_client_gpu_page_count(16384,LM_MAX_MAP_PAGES)==4);
    CHECK(lmal_client_gpu_page_count(LMAL_MAX_BO_BYTES,LM_MAX_MAP_PAGES)==LM_MAX_MAP_PAGES);
    CHECK(lmal_client_gpu_page_count(LMAL_MAX_BO_BYTES+4096,LM_MAX_MAP_PAGES)==0);
    CHECK(lmal_client_gpu_page_count(4095,LM_MAX_MAP_PAGES)==0);
    CHECK(lmal_client_gpu_page_count(0,LM_MAX_MAP_PAGES)==0);
    CHECK(lmal_client_gpu_page_count(4096,0)==0);
    CHECK(lmal_client_gpu_page_count(UINT64_MAX&~UINT64_C(4095),UINT32_MAX)==0);
    CHECK(lmal_client_gpu_page_offset(UINT32_MAX)==(uint64_t)UINT32_MAX*4096);
    CHECK(!lmal_client_gpu_segment_valid(0,16384));
    CHECK(!lmal_client_gpu_segment_valid(0x200001,16384));
    CHECK(!lmal_client_gpu_segment_valid(0x201000,4095));
    CHECK(lmal_client_gpu_segment_valid(0x203000,4096)); /* last 4K of a 16K page */
}
static void surface_layout_tests(void) {
    for(unsigned planes=0;planes<=1;planes++)
        CHECK(lmal_client_surface_layout(1080,2340,4352,0,0x42475241,planes,4,1,1,4352ULL*2340,0,0,0,0)==LMAL_OK);
    CHECK(lmal_client_surface_layout(16,16,64,0,0x52474241,0,4,1,1,1024,0,0,0,0)==LMAL_OK);
    for(unsigned mode=0;mode<12;mode++) {
        unsigned w=16,h=16,pitch=64,offset=0,fmt=0x42475241,planes=0,bpe=4,ew=1,eh=1,compression=0,extended=0,protection=0,parent=0;
        uint64_t size=1024;
        switch(mode) {
        case 0:offset=4;break;case 1:planes=2;break;case 2:fmt=0x34323076;break;
        case 3:pitch=32;break;case 4:size=1023;break;case 5:compression=1;break;
        case 6:extended=1;break;case 7:protection=1;break;case 8:parent=1;break;
        case 9:ew=2;break;case 10:bpe=8;break;case 11:w=UINT32_MAX;break;
        }
        CHECK(lmal_client_surface_layout(w,h,pitch,offset,fmt,planes,bpe,ew,eh,size,compression,extended,protection,parent)==LMAL_ENOTSUP);
    }
}
int main(void) {
    abi_tests(); sync_tests(); race_test(); export_tests(); allocation_tests(); granule_tests();
    surface_layout_tests();
    printf("PASS production client policy: %u checks (17 selectors, exact replies, sync/close, permissions, failed-map cleanup, kernel4K/16K rounding and GPU4K PA stepping)\n",checks);
    return 0;
}
