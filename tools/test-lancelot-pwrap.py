#!/usr/bin/env python3
"""Test the actual shared WACS2 lock with concurrent callers, no hardware."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    path = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', path + '/tools/test-lancelot-pwrap.py'], check=True)
    sys.exit(0)
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_pwrap.c"
static unsigned inside, maximum, commands, peer_calls, peer_releases;
static int peer_error;
static int peer_begin(void *cookie, uint64_t ns) {
    assert(cookie==(void*)1 && ns>0);
    assert(lancelot_pwrap_acquire(NULL,ns)==-16); /* callback reentry rejected */
    peer_calls++; return peer_error;
}
static void peer_end(void *cookie) { assert(cookie==(void*)1); peer_releases++; }
static void *worker(void *arg) {
    (void)arg;
    for(unsigned i=0;i<500;i++) {
        assert(lancelot_pwrap_acquire(NULL,1000000000)==0);
        inside++; if(inside>maximum)maximum=inside;
        commands++; inside--;
        lancelot_pwrap_release(NULL);
    }
    return NULL;
}
static void *timed_worker(void *arg) {
    (void)arg; assert(lancelot_pwrap_acquire(NULL,100000)==-110); return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    assert(!lancelot_pwrap_available());
    assert(lancelot_pwrap_acquire(NULL,10000000)==-95);
    assert(lancelot_pwrap_acquire(NULL,0)==-22);
    assert(lancelot_pwrap_acquire(NULL,1000000001)==-22);
    assert(lancelot_pwrap_poison()==-16);
    lancelot_pwrap_release(NULL);
    struct lancelot_pwrap_handoff h={.mode=LANCELOT_PWRAP_AP_EXCLUSIVE,.evidence="HOST fixture only, NOT phone proof"};
    assert(lancelot_pwrap_register_handoff(NULL)==-22);
    if(!strcmp(argv[1],"peer")) {
        h.mode=LANCELOT_PWRAP_COORDINATED;
        assert(lancelot_pwrap_register_handoff(&h)==-22);
        h.cookie=(void*)1;h.peer_acquire=peer_begin;h.peer_release=peer_end;
    }
    assert(lancelot_pwrap_register_handoff(&h)==0);
    assert(lancelot_pwrap_register_handoff(&h)==-16);
    assert(lancelot_pwrap_available());
    if(h.mode==LANCELOT_PWRAP_COORDINATED) {
        peer_error=-16;
        assert(lancelot_pwrap_acquire(NULL,10000000)==-16);
        assert(peer_calls==1 && peer_releases==0);
        peer_error=0;
    }
    assert(lancelot_pwrap_acquire(NULL,10000000)==0);
    assert(lancelot_pwrap_acquire(NULL,10000000)==-16); /* nonrecursive */
    assert(lancelot_pwrap_register_handoff(&h)==-16);
    pthread_t timed;assert(!pthread_create(&timed,NULL,timed_worker,NULL));
    assert(!pthread_join(timed,NULL));
    lancelot_pwrap_release(NULL);
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(commands==2000 && maximum==1);
    assert(lancelot_pwrap_acquire(NULL,10000000)==0);
    assert(lancelot_pwrap_poison()==0);
    lancelot_pwrap_release(NULL);
    assert(!lancelot_pwrap_available());
    assert(lancelot_pwrap_acquire(NULL,10000000)==-5);
    if(h.mode==LANCELOT_PWRAP_COORDINATED)assert(peer_calls==peer_releases+2); /* denied acquire + poisoned peer token retained */
    printf("WACS2 %s: default denied, 4 callers/2000 operations serialized, recursive/timeout/peer-failure/poison PASS; no MMIO\n",argv[1]);
}
'''
with tempfile.TemporaryDirectory(prefix='lancelot-pwrap-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(source)
    for cc in ['cc', 'clang']:
        binary = tmp / ('test-' + cc)
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DLANCELOT_PWRAP_HOST_TEST',
            '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-pthread',
            '-I', str(ROOT / 'xnu/files/bsd/dev'), str(tmp / 'test.c'), '-o', str(binary)], check=True)
        for mode in ['local', 'peer']:
            subprocess.run([str(binary), mode], check=True)
observe = (ROOT / 'xnu/files/bsd/dev/lancelot_mali_observe.c').read_text()
assert 'if (mode != 1)' in observe and observe.count('ml_io_map(') == 2
assert 'const volatile uint32_t *' in observe
assert not any(s in observe for s in ['0x13000000', '0x13040000', 'lm_core_', 'LancelotMaliPublish'])
assert 'if (!ao) return;' in observe and 'if (ao > 1)' in observe
print('Observation path: SPM by default, AO only with explicit opt-in; no GPU/PMIC command/DMA/service')
