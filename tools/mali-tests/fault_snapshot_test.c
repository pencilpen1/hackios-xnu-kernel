/* Actual production snapshot policy, with no GPU/register implementation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_fault.h"
int main(void) {
    struct lm_first_failure first={0};struct lm_job_result r={0};
    r.sequence=2048;r.failed_stage=LM_JOB_WAIT;r.mmu_fault=0xc1;r.fault_address=0x318c000;
    r.reset_completed=true;r.profile.clean_pages=5752;r.profile.run_ns=123456;
    struct lm_job_result original=r;
    assert(!lm_first_failure_save(NULL,true,LM_EIO,20,1,1000,LM_VA_MIN,&r));
    assert(!lm_first_failure_save(&first,false,LM_EIO,20,1,1000,LM_VA_MIN,&r));
    assert(!lm_first_failure_save(&first,true,0,20,1,1000,LM_VA_MIN,&r));
    assert(!lm_first_failure_save(&first,true,LM_EIO,20,1,1000,LM_VA_MIN,NULL));
    assert(!first.valid&&!lm_first_failure_should_report(&first,0));
    assert(lm_first_failure_save(&first,true,LM_EIO,20,1,1000,LM_VA_MIN,&r));
    assert(!memcmp(&r,&original,sizeof(r))&&first.pid==20&&first.error==LM_EIO);
    struct lm_first_failure retained=first;
    memset(&r,0,sizeof(r));r.sequence=3000;r.quarantined=true;
    assert(!lm_first_failure_save(&first,true,LM_ENODEV,462,0,2000,LM_VA_MIN+4096,&r));
    assert(!memcmp(&first,&retained,sizeof(first))); /* Respawn cannot erase the first cause. */
    assert(lm_first_failure_should_report(&first,100));
    assert(!lm_first_failure_should_report(&first,99));
    assert(!lm_first_failure_should_report(&first,100+LM_FIRST_FAILURE_REPORT_NS-1));
    assert(lm_first_failure_should_report(&first,100+LM_FIRST_FAILURE_REPORT_NS));
    assert(first.result.sequence==2048&&first.result.mmu_fault==0xc1&&first.result.fault_address==0x318c000);
    assert(first.result.reset_completed&&!first.result.quarantined);
    first.report_ns=UINT64_MAX;first.reported=true;
    assert(!lm_first_failure_should_report(&first,0)&&!lm_first_failure_should_report(&first,UINT64_MAX));
    struct lm_first_failure before_launch={0};memset(&r,0,sizeof(r));
    r.failed_stage=LM_JOB_BIND;r.mismatch_reg=0x2400;r.expected=0x1234;r.observed=0x5678;r.quarantined=true;
    assert(lm_first_failure_save(&before_launch,true,LM_EIO,20,0,3000,LM_VA_MIN,&r));
    assert(!before_launch.result.sequence&&before_launch.result.quarantined&&before_launch.result.mismatch_reg==0x2400);
    puts("First GPU failure policy: fixed snapshot, offline/error-only, full original result immutable across respawns, prelaunch mismatch/quarantine retained,5s rate/clock bounds PASS; NO GPU recovery/MMIO/backing release");
    return 0;
}
