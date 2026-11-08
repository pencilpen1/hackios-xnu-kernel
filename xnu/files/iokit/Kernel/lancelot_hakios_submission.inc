/* SPDX-License-Identifier: APSL-2.0
 * Donor-style asynchronous submission, with retained lancelot WB ownership.
 * The FIFO owns a reference until completion. Completion must not take the
 * global client lock, because enqueue/GEM_CLOSE may be sleeping under it.
 */
class HakiosMaliSubmission : public OSObject {
    OSDeclareDefaultStructors(HakiosMaliSubmission);
    bool fSubmitted=false,fCompleted=false;
    LancelotHakiosMaliUserClient *fClient=NULL;
public:
    HakiosSurfaceHandoff handoff;
    LancelotMaliQueueFence *fence=NULL;
    uint64_t preparationUs=0;

    static HakiosMaliSubmission *create(uint32_t capacity,LancelotHakiosMaliUserClient *client) {
        auto *work=OSTypeAlloc(HakiosMaliSubmission);
        if(!work || !work->init()){OSSafeReleaseNULL(work);return NULL;}
        if(!work->handoff.init(capacity)){work->release();return NULL;}
        work->fence=LancelotMaliQueueFence::create();
        if(!work->fence){work->release();return NULL;}
        work->fClient=client;client->retain();
        return work;
    }
    void reserveQueueReference() { fSubmitted=true;retain(); }
    void rejectQueueReference() { fSubmitted=false;release(); }
    int complete(uint64_t sequence,int result,bool *dmaSafe) {
        uint64_t began=lmq_clock();
        result=handoff.finish(result,*dmaSafe,fSubmitted,dmaSafe);
        if(fSubmitted)fClient->recordCompletion(result);
        fCompleted=true;fence->token=sequence;fence->finish(result);
        uint64_t retireUs=(lmq_clock()-began)/1000;
        if(handoff.count() && ((sequence&127u)==0 || preparationUs>10000 || retireUs>10000))
            IOLog("lancelot-hakios-surface: seq=%llu roots=%u prepare_us=%llu retire_us=%llu error=%d\n",
                sequence,handoff.count(),preparationUs,retireUs,result);
        return result;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if(!fCompleted) {
            int error=handoff.finish(LM_EIO,!fSubmitted,fSubmitted);
            if(fence)fence->finish(error);
        }
        OSSafeReleaseNULL(fence);
        OSSafeReleaseNULL(fClient);
        OSObject::free();
    }
};
OSDefineMetaClassAndStructors(HakiosMaliSubmission, OSObject);

class HakiosSubmissionOwner {
    HakiosMaliSubmission *fWork;
public:
    explicit HakiosSubmissionOwner(HakiosMaliSubmission *work):fWork(work){}
    ~HakiosSubmissionOwner(){if(fWork)fWork->release();}
};

static int hakios_submission_complete(void *context,uint64_t sequence,int result,bool *dmaSafe)
{
    auto *work=(HakiosMaliSubmission *)context;
    result=work->complete(sequence,result,dmaSafe);
    work->release();
    return result;
}
