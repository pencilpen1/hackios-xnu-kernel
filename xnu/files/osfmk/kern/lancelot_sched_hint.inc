/* Called with pset locked, before the generic cached-CPU fast path.
 * Idle state/recommendations remain authoritative and normal scheduling
 * handles saturation. No IPI, binding, priority change or queue scan. */
static processor_t
lancelot_sched_idle_hint(processor_set_t pset, processor_t hint, thread_t thread)
{
    uint64_t idle = pset->cpu_state_map[PROCESSOR_IDLE] & pset->primary_map &
        pset->recommended_bitmask & ~pset->pending_AST_URGENT_cpu_mask &
        ~pset->pending_AST_PREEMPT_cpu_mask;
    uint64_t preferred = lsc_preferred_idle(idle,
        lancelot_sched_capacity_mask(true), lancelot_sched_capacity_mask(false),
        thread->sched_pri >= BASEPRI_DEFAULT,
        thread->bound_processor == PROCESSOR_NULL && thread->sched_pri < BASEPRI_RTQUEUES && !pset->is_SMT);
    if (!preferred) return hint;
    if (hint != PROCESSOR_NULL && hint->processor_set == pset &&
        (preferred & (UINT64_C(1) << hint->cpu_id))) return hint;
    return processor_array[lsb_first(preferred)];
}
