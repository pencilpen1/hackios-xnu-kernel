/* Synchronous transaction only: same caller task/client lock and same legacy
 * CPU-ownership rules. Never advertises asynchronous or all-alias permissions. */
static void
lmal_pair_bo_detail(struct lmal_pair_admission *d, uint32_t reason, uint32_t stage,
    uint32_t handle, const struct lmal_bo *b)
{
    bzero(d, sizeof(*d)); d->reason = reason; d->stage_index = stage; d->bo_handle = handle;
    if (!b) return;
    d->surface_id = b->surface_id; d->bo_flags = b->flags;
    if (b->memory) d->bo_state |= LMAL_PAIR_BO_MEMORY;
    if (b->mapped) d->bo_state |= LMAL_PAIR_BO_MAPPED;
    if (b->prepared) d->bo_state |= LMAL_PAIR_BO_PREPARED;
    if (b->quarantined) d->bo_state |= LMAL_PAIR_BO_QUARANTINED;
    if (b->cpu_lease) d->bo_state |= LMAL_PAIR_BO_LEASE;
}

void
LancelotMaliUserClient::pair_admission_diagnostic(uint32_t selector, int error,
    const struct lmal_pair_admission *d)
{
    /* Metadata only, while fLock protects BO/client state. First four rejects
     * and then one per second; every rejection increments the exact counter. */
    if (fSyncPairRejectCount != UINT64_MAX) ++fSyncPairRejectCount;
    uint64_t now = lmal_now_ns();
    if (fSyncPairRejectCount > 4 && now >= fSyncPairRejectAt &&
        now - fSyncPairRejectAt < UINT64_C(1000000000)) return;
    printf("lancelot-pair-admission: selector=%#x pid=%d e=%d eligible=%u reason=%u stage=%u handle=%u surface=%u flags=%#x memory=%u mapped=%u prepared=%u quarantined=%u lease=%u rejects=%llu no_pixel_access=1\n",
        selector, proc_selfpid(), error, d->eligible, d->reason, d->stage_index, d->bo_handle,
        d->surface_id, d->bo_flags, !!(d->bo_state & LMAL_PAIR_BO_MEMORY),
        !!(d->bo_state & LMAL_PAIR_BO_MAPPED), !!(d->bo_state & LMAL_PAIR_BO_PREPARED),
        !!(d->bo_state & LMAL_PAIR_BO_QUARANTINED), !!(d->bo_state & LMAL_PAIR_BO_LEASE),
        (unsigned long long)fSyncPairRejectCount);
    fSyncPairRejectAt = now;
}

int
LancelotMaliUserClient::submit_pair_sync(const struct lmal_submit_pair_sync *request,
    struct lmal_submit_pair_sync *response, uint64_t started, struct lmal_pair_admission *admission)
{
    struct stage_state {
        uint32_t *handles, *access, count;
        struct lmal_bo **refs;
        struct lm_range *ranges;
    } stage[2] = {};
    uint32_t *sync_handles = NULL;
    LancelotMaliCpuLease **leases = NULL;
    uint64_t *cookies = NULL;
    bool *writers = NULL;
    uint32_t roots = 0, capacity = 0;
    uint64_t deadline = 0, prepared = 0, dispatched = 0, retired = 0;
    struct lm_job_result result[2] = {};
    int ret = LMAL_OK;
    bool pinned = false, dispatched_any = false, safe = true;
    struct lmal_pair_admission detail = {};
    detail.reason = LMAL_PAIR_REASON_REQUEST; detail.stage_index = LMAL_PAIR_STAGE_NONE;
    if (admission) { bzero(admission, sizeof(*admission)); admission->stage_index = LMAL_PAIR_STAGE_NONE; }
    uint64_t now = lmal_now_ns();
    if (!started || now < started || !request->timeout_ns ||
        request->timeout_ns > LMAL_MAX_WAIT_NS ||
        lmal_add_overflow_u64(started, request->timeout_ns, &deadline)) { ret = LMAL_EINVAL; goto out; }
    detail.reason = LMAL_PAIR_REASON_DEADLINE;
    if (now >= deadline) { ret = LMAL_ETIMEDOUT; goto out; }
    detail.reason = LMAL_PAIR_REASON_VM;
    if (fVMQuarantined || !lm_gpu_ready()) { ret = LMAL_ENODEV; goto out; }
    detail.reason = LMAL_PAIR_REASON_REQUEST;
    if (fSyncPairToken == UINT64_MAX || fJobsSubmitted > UINT64_MAX-2u ||
        fJobsCompleted > UINT64_MAX-2u) { ret = LMAL_EINVAL; goto out; }
    /* Both handle/access snapshots, objects, JC headers and dependencies must
     * validate before acquiring GPU ownership or dispatching the first stage. */
    for (uint32_t s = 0; s < 2; s++) {
        lmal_pair_bo_detail(&detail, LMAL_PAIR_REASON_REQUEST, s, 0, NULL);
        const struct lmal_sync_pair_stage *in = &request->stage[s];
        if (!in->bo_count || in->bo_count > LMAL_SYNC_PAIR_STAGE_BOS ||
            !in->bo_handles || !in->bo_readonly || in->reserved ||
            !in->jc || (in->jc & 63u) || in->jc > UINT64_MAX - 64u) { ret = LMAL_EINVAL; goto out; }
        capacity += in->bo_count;
        detail.reason = LMAL_PAIR_REASON_COPY;
        ret = lmal_copy_u32_array(in->bo_handles, in->bo_count, LMAL_SYNC_PAIR_STAGE_BOS, &stage[s].handles);
        if (ret) goto out;
        ret = lmal_copy_u32_array(in->bo_readonly, in->bo_count, LMAL_SYNC_PAIR_STAGE_BOS, &stage[s].access);
        if (ret) goto out;
        stage[s].refs = IONew(struct lmal_bo *, in->bo_count);
        stage[s].ranges = IONew(struct lm_range, in->bo_count);
        detail.reason = LMAL_PAIR_REASON_ALLOC;
        if (!stage[s].refs || !stage[s].ranges) { ret = LMAL_ENOMEM; goto out; }
        for (uint32_t i = 0; i < in->bo_count; i++) {
            lmal_pair_bo_detail(&detail, LMAL_PAIR_REASON_ACCESS, s, stage[s].handles[i], NULL);
            if (stage[s].access[i] & ~LMAL_ACCESS_READONLY) { ret = LMAL_EINVAL; goto out; }
            struct lmal_bo *b = bo(stage[s].handles[i]);
            lmal_pair_bo_detail(&detail, LMAL_PAIR_REASON_BO_STATE, s, stage[s].handles[i], b);
            if (!b || !b->memory || !b->mapped || !b->prepared || b->quarantined ||
                !b->size || (b->size & LMAL_PAGE_MASK) ||
                b->in_flight > UINT32_MAX - 2u) { ret = LMAL_EINVAL; goto out; }
            if (!b->cpu_lease) {
                detail.reason = LMAL_PAIR_REASON_MISSING_LEASE;
                if (!admission) { ret = LMAL_EINVAL; goto out; } /* mandatory actual-pair gate */
                if (admission->reason == LMAL_PAIR_REASON_NONE) *admission = detail;
                /* Query still validates BOTH stages and all syncs; malformed
                 * work must never be disguised as legacy-eligible missing lease. */
            }
            uint32_t j = 0;
            for (; j < stage[s].count; j++) if (stage[s].refs[j] == b) break;
            if (j < stage[s].count) {
                stage[s].access[j] &= stage[s].access[i]; /* writable duplicate wins */
            } else {
                stage[s].handles[j] = stage[s].handles[i];
                stage[s].access[j] = stage[s].access[i];
                stage[s].refs[j] = b;
                stage[s].count++;
            }
        }
        bool jc_valid = false;
        for (uint32_t i = 0; i < stage[s].count; i++) {
            struct lmal_bo *b = stage[s].refs[i];
            lmal_pair_bo_detail(&detail, LMAL_PAIR_REASON_JC, s, stage[s].handles[i], b);
            uint64_t end = 0;
            if (lmal_add_overflow_u64(b->gpu_va, b->size, &end)) { ret = LMAL_EINVAL; goto out; }
            bool contains = in->jc >= b->gpu_va && in->jc < end;
            if (contains && stage[s].access[i] == LMAL_ACCESS_READONLY) { ret = LMAL_EINVAL; goto out; }
            jc_valid |= contains && (b->flags & LMAL_BO_WRITE) && in->jc + 64u <= end;
            stage[s].ranges[i].va = b->gpu_va;
            stage[s].ranges[i].pages = (uint32_t)(b->size / LMAL_PAGE_SIZE);
            stage[s].ranges[i].flags = lmal_client_gpu_only_eligible(gLancelotBOCached == 0,
                b->surface != NULL, b->export_memory != NULL, contains) ? LM_RANGE_GPU_ONLY : 0;
            if (stage[s].access[i] == LMAL_ACCESS_READONLY) stage[s].ranges[i].flags |= LM_RANGE_READONLY;
        }
        if (!jc_valid) { detail.bo_handle = 0; ret = LMAL_EINVAL; goto out; }
    }
    lmal_pair_bo_detail(&detail, LMAL_PAIR_REASON_SYNC, LMAL_PAIR_STAGE_NONE, 0, NULL);
    ret = lmal_copy_u32_array(request->in_syncs, request->in_sync_count, LMAL_MAX_SUBMIT_SYNCS, &sync_handles);
    if (ret) goto out;
    for (uint32_t i = 0; i < request->in_sync_count; i++) {
        for (uint32_t j = 0; j < i; j++) if (sync_handles[j] == sync_handles[i]) {
            ret = LMAL_EINVAL; goto out;
        }
        struct lmal_sync *f = sync(sync_handles[i]);
        if (!f || !f->signaled || f->in_flight == UINT32_MAX ||
            sync_handles[i] == request->out_sync) { ret = f && !f->signaled ? LMAL_EBUSY : LMAL_EINVAL; goto out; }
    }
    if (request->out_sync && (!sync(request->out_sync) || sync(request->out_sync)->in_flight == UINT32_MAX)) {
        ret = LMAL_EINVAL; goto out;
    }
    if (admission) {
        if (admission->reason == LMAL_PAIR_REASON_MISSING_LEASE) detail = *admission;
        else {
            bzero(admission, sizeof(*admission)); admission->eligible = 1;
            admission->stage_index = LMAL_PAIR_STAGE_NONE; detail = *admission;
        }
        goto out; /* read-only check: no CPU freeze, pin, sync mutation or DMA */
    }
    detail.reason = LMAL_PAIR_REASON_ALLOC;
    leases = IONew(LancelotMaliCpuLease *, capacity);
    cookies = IONew(uint64_t, capacity);
    writers = IONew(bool, capacity);
    if (!leases || !cookies || !writers) { ret = LMAL_ENOMEM; goto out; }
    bzero(cookies, capacity * sizeof(*cookies));
    for (uint32_t s = 0; s < 2; s++) for (uint32_t i = 0; i < stage[s].count; i++) {
        struct lmal_bo *b = stage[s].refs[i];
        LancelotMaliCpuLease *lease = b->cpu_lease;
        bool writer = (b->flags & LMAL_BO_WRITE) && !(stage[s].ranges[i].flags & LM_RANGE_READONLY);
        uint32_t j = 0;
        while (j < roots && (uintptr_t)leases[j] < (uintptr_t)lease) j++;
        if (j < roots && leases[j] == lease) { writers[j] |= writer; continue; }
        for (uint32_t k = roots; k > j; k--) { leases[k] = leases[k-1]; writers[k] = writers[k-1]; }
        leases[j] = lease; writers[j] = writer; roots++;
    }
    for (uint32_t i = 0; i < roots; i++) {
        lmal_pair_bo_detail(&detail, LMAL_PAIR_REASON_CPU, LMAL_PAIR_STAGE_NONE, 0, NULL);
        now = lmal_now_ns();
        if (now < started || now >= deadline) { ret = now < started ? LMAL_EIO : LMAL_ETIMEDOUT; goto out; }
        ret = leases[i]->beginGpu(deadline - now, &cookies[i], false, writers[i]);
        if (ret) goto out;
    }
    prepared = lmal_now_ns();
    if (prepared < started || prepared >= deadline) { ret = prepared < started ? LMAL_EIO : LMAL_ETIMEDOUT; goto out; }
    for (uint32_t s = 0; s < 2; s++) for (uint32_t i = 0; i < stage[s].count; i++) stage[s].refs[i]->in_flight++;
    for (uint32_t i = 0; i < request->in_sync_count; i++) fSyncs[sync_handles[i]-1].in_flight++;
    if (request->out_sync) { fSyncs[request->out_sync-1].in_flight++; fSyncs[request->out_sync-1].signaled = false; }
    pinned = true;
    for (uint32_t i = 0; i < roots; i++) {
        ret = leases[i]->markLegacy(cookies[i]);
        if (ret) goto out;
    }
    now = lmal_now_ns();
    if (now < prepared || now >= deadline) { ret = now < prepared ? LMAL_EIO : LMAL_ETIMEDOUT; goto out; }
    ++fSyncPairToken;
    dispatched_any = true;
    ret = lm_gpu_submit_pair(fVM, request->stage[0].jc, request->stage[1].jc,
        deadline - now, result, stage[0].ranges, stage[0].count, stage[1].ranges, stage[1].count,
        proc_selfpid(), fSyncPairToken);
    dispatched = lmal_now_ns();
    safe = !result[0].quarantined && !result[1].quarantined && lm_gpu_vm_safe_to_free(fVM);
    for (uint32_t s = 0; s < 2; s++) {
        response->sequence[s] = result[s].sequence;
        response->job_status[s] = result[s].job_status;
        response->stage_error[s] = result[s].pair_stage_error;
        if (result[s].pair_stage_attempted) {
            response->attempted_mask |= 1u << s;
            if (!result[s].pair_stage_error && result[s].sequence) response->completed_mask |= 1u << s;
            fJobsSubmitted++;
        } else if (result[s].sequence || result[s].pair_stage_error || result[s].started_ns || result[s].completed_ns) {
            ret = LMAL_EIO; safe = false;
        }
        if (result[s].gpu_fault || result[s].mmu_fault) fFaults++;
        if (result[s].reset_completed) fResets++;
    }
    if (!ret && (response->attempted_mask != 3 || response->completed_mask != 3 ||
        result[0].gpu_fault || result[0].mmu_fault || result[1].gpu_fault || result[1].mmu_fault)) {
        ret = LMAL_EIO; safe = false;
    }
out:
    if (cookies) for (uint32_t i = 0; i < roots; i++) if (cookies[i]) {
        /* No DMA before dispatch: exact rollback. Unknown DMA retains every
         * root's cookie/UPL internally, as in the existing legacy path. */
        int ended = leases[i]->endGpu(cookies[i], pinned ? ret : LMAL_OK, safe);
        cookies[i] = 0;
        if (ended) { if (!ret) ret = ended; safe = false; }
    }
    retired = lmal_now_ns();
    if (retired < started || (dispatched_any && retired < dispatched)) {
        if (!ret) ret = LMAL_EIO;
    } else if (retired >= deadline && !ret) { ret = LMAL_ETIMEDOUT; detail.reason = LMAL_PAIR_REASON_DEADLINE; }
    if (!dispatched_any && (ret || (admission && !admission->eligible))) {
        detail.eligible = 0;
        if (admission && ret) *admission = detail;
        pair_admission_diagnostic(admission ? LMAL_SELECTOR_CHECK_PAIR_SYNC : LMAL_SELECTOR_SUBMIT_PAIR_SYNC,
            ret, &detail);
    }
    if (!safe) {
        if (!fVMQuarantined) fQuarantined++;
        fVMQuarantined = true; response->completed_mask = 0;
    }
    if (pinned) {
        for (uint32_t s = 0; s < 2; s++) for (uint32_t i = 0; i < stage[s].count; i++) {
            struct lmal_bo *b = stage[s].refs[i]; b->in_flight--;
            b->last_sequence = result[s].sequence; b->last_status = (uint32_t)(ret ? ret : result[s].pair_stage_error);
        }
        for (uint32_t i = 0; i < request->in_sync_count; i++) fSyncs[sync_handles[i]-1].in_flight--;
        if (request->out_sync) {
            fSyncs[request->out_sync-1].in_flight--;
            if (!ret && safe && response->completed_mask == 3) fSyncs[request->out_sync-1].signaled = true;
        }
        if (safe) for (uint32_t s = 0; s < 2; s++) if (response->completed_mask & (1u << s)) fJobsCompleted++;
    }
    if (dispatched_any && (!fSyncPairReportAt || retired < fSyncPairReportAt ||
        retired - fSyncPairReportAt >= UINT64_C(1000000000) || ret)) {
        printf("lancelot-sync-pair: at_ns=%llu token=%llu roots=%u prepare_ns=%llu dispatch_ns=%llu retire_ns=%llu attempted=%u completed=%u e=%d synchronous=1\n",
            (unsigned long long)retired, (unsigned long long)fSyncPairToken, roots,
            (unsigned long long)(prepared >= started ? prepared-started : 0),
            (unsigned long long)(dispatched >= prepared ? dispatched-prepared : 0),
            (unsigned long long)(retired >= dispatched ? retired-dispatched : 0),
            response->attempted_mask, response->completed_mask, ret);
        fSyncPairReportAt = retired;
    }
    if (cookies) IODelete(cookies, uint64_t, capacity);
    if (leases) IODelete(leases, LancelotMaliCpuLease *, capacity);
    if (writers) IODelete(writers, bool, capacity);
    lmal_free_u32_array(sync_handles, request->in_sync_count);
    for (uint32_t s = 0; s < 2; s++) {
        lmal_free_u32_array(stage[s].handles, request->stage[s].bo_count);
        lmal_free_u32_array(stage[s].access, request->stage[s].bo_count);
        if (stage[s].refs) IODelete(stage[s].refs, struct lmal_bo *, request->stage[s].bo_count);
        if (stage[s].ranges) IODelete(stage[s].ranges, struct lm_range, request->stage[s].bo_count);
    }
    return ret;
}
