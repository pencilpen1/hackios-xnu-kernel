#include <assert.h>
#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
struct MiniIOSurface {unsigned id;};
static std::mutex lock;
static std::condition_variable condition;
static unsigned readers,begin_calls,end_calls,fail_id;
static bool LancelotMaliQueueSurfaceReadBegin(MiniIOSurface *surface,uint64_t ns)
{
    assert(ns);std::lock_guard<std::mutex> held(lock);
    begin_calls++;
    if(surface->id==fail_id)return false;
    readers++;return true;
}
static void LancelotMaliQueueSurfaceReadEnd(MiniIOSurface *surface)
{
    assert(surface);std::lock_guard<std::mutex> held(lock);
    assert(readers);readers--;end_calls++;condition.notify_all();
}
#include "actual-presenter-read-group.h"
int main()
{
    MiniIOSurface surfaces[17];for(unsigned i=0;i<17;i++)surfaces[i].id=i+1;
    {
        LancelotMaliQueueReadGroup group;
        assert(group.add(nullptr,1)&&group.add(&surfaces[0],1)&&group.add(&surfaces[0],1));
        assert(begin_calls==1&&readers==1);
        bool writer_waiting=false;std::atomic<bool> writer_finished=false;
        std::thread writer([&]{
            std::unique_lock<std::mutex> held(lock);writer_waiting=true;condition.notify_all();
            condition.wait(held,[]{return readers==0;});writer_finished=true;
        });
        {std::unique_lock<std::mutex> held(lock);condition.wait(held,[&]{return writer_waiting;});}
        assert(!writer_finished); /* copying and recording still own the read gate */
        group.releaseReaders();writer.join();
        assert(writer_finished&&readers==0&&end_calls==1);
        group.releaseReaders();assert(end_calls==1);
    }
    assert(end_calls==1);
    {
        LancelotMaliQueueReadGroup group;
        for(unsigned i=0;i<16;i++)assert(group.add(&surfaces[i],1));
        assert(!group.add(&surfaces[16],1));
        assert(readers==16);group.releaseReaders();assert(readers==0);
    }
    unsigned before=end_calls;
    {
        LancelotMaliQueueReadGroup group;
        fail_id=2;
        assert(group.add(&surfaces[0],1));
        assert(!group.add(&surfaces[1],1));
    }
    assert(readers==0&&end_calls==before+1);
}
