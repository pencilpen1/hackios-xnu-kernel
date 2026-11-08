/* Real production sampling helper, no GPU/MMIO. ASan/UBSan host fixture. */
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include "lancelot_mali_profile.h"
static void legacy(void) {
    struct lm_profile_policy p={0};unsigned count=0;uint64_t last=0;
    for(uint64_t q=1;q<=10000;q++)if(lm_profile_sample(&p,1,q,0,q)) {count++;last=q;}
    assert(count==384&&last==3264&&p.legacy_pairs==384);
    assert(!lm_profile_sample(&p,1,20000,1,LM_PROFILE_STEADY_NS));
    puts("profile: mode1 unchanged first192/every16/384-pair cap PASS");
}
static void steady(void) {
    struct lm_profile_policy p={0};unsigned startup=0,steady[2]={0};
    for(uint64_t q=1;q<=192;q++)startup+=lm_profile_sample(&p,2,q,(uint32_t)(q&1),q);
    assert(startup==192);
    uint64_t previous[2]={0};
    for(uint64_t i=0;i<200000;i++) {
        uint64_t now=LM_PROFILE_STEADY_NS+i*UINT64_C(1000000);uint32_t req=(uint32_t)(i&1);
        if(lm_profile_sample(&p,2,193+i,req,now)) {
            if(steady[req])assert(now-previous[req]>=LM_PROFILE_STEADY_NS);
            previous[req]=now;steady[req]++;
        }
    }
    assert(steady[0]==200&&steady[1]==200&&p.legacy_pairs==0);
    assert(previous[0]>UINT64_C(180000000000)&&previous[1]>UINT64_C(180000000000));
    assert(lm_profile_sample(&p,2,1000000,0,UINT64_C(900000000000)));
    assert(lm_profile_sample(&p,2,1000001,1,UINT64_C(900000000000)));
    puts("profile: mode2 survives startup/400-pair history/15min, each requirement <=1Hz independently PASS");
}
static void bounds(void) {
    struct lm_profile_policy p={0};
    assert(!lm_profile_sample(NULL,2,1,0,0));
    assert(!lm_profile_sample(&p,0,1,0,0));assert(!lm_profile_sample(&p,3,1,0,0));
    assert(!lm_profile_sample(&p,2,0,0,0));assert(!lm_profile_sample(&p,2,1,2,0));
    assert(lm_profile_sample(&p,2,193,0,LM_PROFILE_STEADY_NS));
    assert(!lm_profile_sample(&p,2,194,0,LM_PROFILE_STEADY_NS));
    assert(!lm_profile_sample(&p,2,195,0,LM_PROFILE_STEADY_NS-1));
    assert(!lm_profile_sample(&p,2,196,0,2*LM_PROFILE_STEADY_NS-1));
    assert(lm_profile_sample(&p,2,197,0,2*LM_PROFILE_STEADY_NS));
    assert(lm_profile_sample(&p,2,198,1,2*LM_PROFILE_STEADY_NS));
    assert(!lm_profile_sample(&p,2,199,0,0));
    assert(lm_profile_sample(&p,2,200,0,UINT64_MAX));
    assert(!lm_profile_sample(&p,2,201,0,UINT64_MAX));
    assert(!lm_profile_sample(&p,2,202,0,0));
    puts("profile: disabled/invalid/zero sequence/clock regression/wrap fail closed; exact1s boundary PASS");
}
int main(void) {legacy();steady();bounds();return 0;}
