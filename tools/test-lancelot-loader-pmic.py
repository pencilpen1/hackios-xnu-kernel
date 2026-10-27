#!/usr/bin/env python3
"""Exercise the actual UEFI snapshot core/bootarg parser in RAM, no device."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    path = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3',
                    path + '/tools/test-lancelot-loader-pmic.py'], check=True)
    sys.exit(0)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_pmic.h"
static unsigned calls, fail_at;
static uint16_t identity=0x5830, final=0x5830;
static efi_status fake_read(uint16_t addr,uint16_t *out) {
    unsigned at=calls++;
    assert(at<10&&addr==(at==9?0xa:lpm_read_addresses[at]));
    *out=at==0?identity:at==9?final:(uint16_t)(0x100+at);
    return calls==fail_at?(EFI_ERROR_BIT|18):0;
}
int main(void) {
    struct lpm_snapshot s,golden;memset(&golden,0xde,sizeof(golden));
    efi_mtk_pwrap_checked io={fake_read,NULL,NULL};
    assert(!lpm_requested("")&&!lpm_requested("cpus=8 lancelot_pmic_probe=0"));
    assert(lpm_requested("cpus=8 lancelot_pmic_probe=1 gpu=0")==1);
    assert(!lpm_requested("x_lancelot_pmic_probe=1"));
    assert(lpm_requested("lancelot_pmic_probe=1 lancelot_pmic_probe=0")==-1);
    assert(lpm_requested("lancelot_pmic_probe=11")==-1);
    assert(lpm_requested("lancelot_pmic_probe=")==-1);
    assert(lpm_requested(NULL)==-1);
    s=golden;assert(lpm_capture(NULL,&s)==EFI_UNSUPPORTED&&!calls&&!memcmp(&s,&golden,sizeof(s)));
    assert(lpm_capture(&io,NULL)==EFI_UNSUPPORTED&&!calls);
    assert(!lpm_capture(&io,&s)&&calls==10&&s.raw[0]==0x5830&&s.raw[8]==0x108);
    for(unsigned i=1;i<=10;i++) {
        calls=0;fail_at=i;s=golden;
        assert(lpm_capture(&io,&s)==(EFI_ERROR_BIT|18)&&calls==i&&!memcmp(&s,&golden,sizeof(s)));
    }
    fail_at=0;calls=0;identity=0x5831;s=golden;
    assert(lpm_capture(&io,&s)==EFI_UNSUPPORTED&&calls==1&&!memcmp(&s,&golden,sizeof(s)));
    calls=0;identity=0x5830;final=0x5820;s=golden;
    assert(lpm_capture(&io,&s)==EFI_UNSUPPORTED&&calls==10&&!memcmp(&s,&golden,sizeof(s)));
    puts("Actual UEFI PMIC snapshot: token/absent guards, exact 9 wire addresses, 10 fault positions, HWCID/changed SWCID rejection and unchanged failed outputs PASS; RAM ONLY");
}
'''
files = [ROOT / 'xnuloader' / name for name in ['efi.h', 'lancelot_pmic.h', 'lancelot.h']]
digest = lambda: [hashlib.sha256(p.read_bytes()).hexdigest() for p in files]
before = digest()
with tempfile.TemporaryDirectory(prefix='lancelot-efi-pmic-') as temp:
    temp = Path(temp)
    (temp / 'test.c').write_text(code)
    for cc in ['cc', 'clang']:
        binary = temp / cc
        subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I' + str(ROOT / 'xnuloader'),
                        str(temp / 'test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
assert before == digest()
print('UEFI bounded-read snapshot/guards GCC+Clang PASS; source hashes unchanged; no power/DMA/ownership claim')
