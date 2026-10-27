#!/usr/bin/env python3
"""Execute actual SSPM boot adapter with mocked bootargs/board, real protocol core.

Fixtures are the same RAM-only fixtures compiled into XNU; never access a phone.
"""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    root = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', root + '/tools/test-lancelot-sspm-boot.py'], check=True)
    sys.exit(0)

source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static bool present, board = true;
static unsigned mode, board_calls;
static char output[4096];
static bool PE_parse_boot_argn(const char *name, void *out, unsigned size) {
    assert(!strcmp(name,"lancelot_sspm_pmic") && size == sizeof(uint32_t));
    if(present) *(uint32_t *)out = mode;
    return present;
}
static bool lancelot_touch_platform(void) {board_calls++; return board;}
static int capture_printf(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt);
    size_t length=strlen(output);
    int count=vsnprintf(output+length,sizeof(output)-length,fmt,ap);
    va_end(ap); assert(count>=0 && (size_t)count<sizeof(output)-length); return count;
}
#define printf capture_printf
#define LANCELOT_SSPM_BOOT_HOST_TEST
#include "lancelot_sspm_boot.c"
#undef printf
int main(void) {
    lancelot_sspm_pmic_init(); assert(!output[0] && !board_calls);
    present=true;mode=0;
    lancelot_sspm_pmic_init(); assert(!output[0] && !board_calls);
    mode=2;
    lancelot_sspm_pmic_init(); assert(strstr(output,"rejected mode=2") && !board_calls);
    assert(strstr(output,"no MMIO/IPI"));
    output[0]=0;mode=1;board=false;
    lancelot_sspm_pmic_init(); assert(strstr(output,"board contract rejected") && board_calls==1);
    assert(!strstr(output,"selftest="));
    output[0]=0;board=true;
    lancelot_sspm_pmic_init(); assert(strstr(output,"RAM selftest=PASS line=0"));
    assert(strstr(output,"synthetic fixture only") && strstr(output,"real backend=absent"));
    assert(strstr(output,"no sequence/length field") && !strstr(output,"FAIL"));
    assert(lancelot_sspm_protocol_selftest()==0);
    puts("Actual SSPM boot adapter: absent/off/invalid/board-reject guards and RAM success/failure/quarantine fixtures PASS; no hardware");
}
'''
dev = ROOT / 'xnu/files/bsd/dev'
files = [dev / name for name in ['lancelot_sspm_boot.c', 'lancelot_sspm_pmic.c',
                                'lancelot_sspm_pmic.h', 'lancelot_sspm_selftest.h']]
digest = lambda: [hashlib.sha256(p.read_bytes()).hexdigest() for p in files]
before = digest()
with tempfile.TemporaryDirectory(prefix='lancelot-sspm-boot-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(source)
    for cc in ['cc', 'clang']:
        binary = directory / cc
        subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I' + str(dev),
                        str(directory / 'test.c'), str(dev / 'lancelot_sspm_pmic.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
assert before == digest()
adapter = (dev / 'lancelot_sspm_boot.c').read_text()
assert not any(word in adapter for word in ['ml_io_map', 'LSP_DATA_PHYS', 'LSP_CTRL_PHYS', 'LancelotMaliPublish'])
patch = (ROOT / 'xnu/redwood.patch').read_text()
for name in ['lancelot_sspm_boot.c', 'lancelot_sspm_pmic.c']:
    assert '+bsd/dev/' + name + '\tstandard' in patch
input_source = (dev / 'redwood_input.c').read_text()
assert input_source.index('lancelot_sspm_pmic_init();') < input_source.index('lancelot_mali_init();')
print('SSPM production boot integration and exact guards PASS; no MMIO/backend registered')
