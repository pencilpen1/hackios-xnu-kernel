#!/usr/bin/env python3
"""Production always-on read allowlist/control flow tests; no phone access."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    path = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', path + '/tools/test-lancelot-mali-observe.py'], check=True)
    sys.exit(0)
test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_mali_observe.h"
struct fake { unsigned reads[LMAO_BANK_COUNT], total, delays, fail; uint32_t wacs; };
static int read32(void *cookie, enum lmao_bank bank, uint32_t off, uint32_t *v) {
    struct fake *f=cookie; assert(lmao_read_allowed(bank,off));
    f->reads[bank]++; f->total++;
    if(f->fail==f->total)return -5;
    *v=bank==LMAO_WRAP && off==0xc24?f->wacs:0;
    return 0;
}
static void delay(void *cookie, uint32_t us) { assert(us==1000); ((struct fake *)cookie)->delays++; }
int main(void) {
    struct fake f={0};struct lmao_io io={&f,read32,delay};struct lmao_snapshot s={0};uint32_t v;
    assert(!lmao_capture_spm(&io,&s)&&f.total==8&&f.reads[LMAO_SPM]==8);
    for(unsigned i=1;i<LMAO_BANK_COUNT;i++)assert(!f.reads[i]);
    assert(lmao_read(&io,LMAO_WRAP,0xc20,&v)==-22); /* CMD never allowed */
    assert(lmao_read(&io,LMAO_WRAP,0xc28,&v)==-22); /* VLDCLR never allowed */
    assert(lmao_read(&io,LMAO_TOP,0x54,&v)==-22);
    assert(lmao_read(&io,(enum lmao_bank)99,0,&v)==-22&&f.total==8);
    f.wacs=(1u<<20)|(1u<<21);
    assert(!lmao_capture_ao(&io,&s)&&s.samples==64&&f.delays==63&&!s.wacs2_active&&!s.wacs2_no_init);
    for(unsigned mode=0;mode<4;mode++) {
        f=(struct fake){.wacs=(1u<<20)|(1u<<21)};
        if(mode==0)f.wacs|=1u<<19; /* foreign REQ with IDLE FSM */
        if(mode==1)f.wacs|=6u<<16;
        if(mode==2)f.wacs&=~(1u<<20);
        if(mode==3)f.wacs&=~(1u<<21);
        assert(!lmao_capture_ao(&io,&s)&&s.samples==64&&f.delays==63);
        if(mode!=3)assert(s.wacs2_active==64);else assert(s.wacs2_no_init==64);
        assert(s.wacs2_fsm_or_req==(mode<2?64:0));
        assert(s.wacs2_sync_busy==(mode==2?64:0));
    }
    for(unsigned fail=1;fail<=99;fail++) {
        f=(struct fake){.fail=fail};
        int e=lmao_capture_ao(&io,&s);
        if(fail<=99)assert(e==-5&&f.total==fail);
    }
    puts("AO production: SPM isolation, exact read allowlists, foreign REQ/FSM/no-sync/no-init and every read-error exit PASS; no MMIO");
}
'''
with tempfile.TemporaryDirectory(prefix='lancelot-mali-ao-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(test)
    for cc in ['cc', 'clang']:
        subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-I', str(ROOT / 'xnu/files/bsd/dev'),
            str(tmp / 'test.c'), '-o', str(tmp / cc)], check=True)
        subprocess.run([str(tmp / cc)], check=True)
production = (ROOT / 'xnu/files/bsd/dev/lancelot_mali.c').read_text()
assert production.index('if(mode==1)') < production.index('int registration=')
assert 'owner->acquire!=lancelot_pwrap_acquire' in production
assert '!lancelot_pwrap_available()' in production
assert production.index('!lancelot_pwrap_available()') < production.index('int pe=lmp_pmic_probe')
print('Full-mode glue uses the shared WACS2 arbiter; missing board handoff stops before PMIC/GPU access')
