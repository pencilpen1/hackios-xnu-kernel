/* Opt-in SSPM protocol RAM selftest only. No real mailbox backend is installed.
 * Never infer live firmware/permissions from DT address or Android 'ready'. */
#ifndef LANCELOT_SSPM_BOOT_HOST_TEST
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#endif
#include "lancelot_sspm_selftest.h"

void
lancelot_sspm_pmic_init(void)
{
    uint32_t mode = 0;
    if (!PE_parse_boot_argn("lancelot_sspm_pmic", &mode, sizeof(mode)) || !mode) return;
    if (mode != 1) {
        printf("lancelot-sspm: rejected mode=%u; live firmware/PMIC-pin ownership/access unproven; no MMIO/IPI\n", mode);
        return;
    }
    if (!lancelot_touch_platform()) {
        printf("lancelot-sspm: board contract rejected; no MMIO/IPI\n");
        return;
    }
    unsigned failure = lancelot_sspm_protocol_selftest();
    printf("lancelot-sspm: production protocol RAM selftest=%s line=%u; synthetic fixture only, no MMIO/IPI/PMIC/DMA/service\n",
        failure ? "FAIL" : "PASS", failure);
    if (failure) return;
    printf("lancelot-sspm: ABI candidate pin=8 mailbox=1 irq-bit=2 slot=10 request-words=5 reply-words=2; no sequence/length field, NOT live ABI proof\n");
    printf("lancelot-sspm: real backend=absent firmware=unverified pin-handoff=unverified permissions=unverified; hardware read remains blocked\n");
}
