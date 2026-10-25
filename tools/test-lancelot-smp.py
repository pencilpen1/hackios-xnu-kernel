#!/usr/bin/env python3
"""Host-only MTK CPU enumeration, fail-closed handoff and kernel guard tests."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    path = f'/mnt/{ROOT.drive[0].lower()}{ROOT.as_posix()[2:]}'
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', path + '/tools/test-lancelot-smp.py'], check=True)
    sys.exit(0)

harness = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
typedef void *DTEntry;
#define kSuccess 0
static int SecureDTLookupEntry(void *,const char *,DTEntry *);
static int SecureDTGetProperty(DTEntry,const char *,const void **,unsigned *);
#define KERNEL 1
#include "lancelot_smp.h"
static struct lancelot_smp_offer offer;
static unsigned queries, seen, missing, property_size, bad_reg;
static int64_t states[8];
static int64_t affinity(uint32_t id) { assert(!(id&255) && id<=0x700);queries++;seen|=1u<<(id>>8);return states[id>>8]; }
static int SecureDTLookupEntry(void *a,const char *name,DTEntry *out) {
    (void)a;
    if(!strcmp(name,"/chosen")) { *out=(void *)9;return missing ? -1 : 0; }
    assert(!strncmp(name,"/cpus/cpu",9) && name[9]>='0' && name[9]<='7' && !name[10]);
    *out=(void *)(uintptr_t)(name[9]-'0'+1);return 0;
}
static int SecureDTGetProperty(DTEntry e,const char *name,const void **p,unsigned *n) {
    if((uintptr_t)e==9) {
        if(!strcmp(name,"lancelot-smp")) {*p=&offer;*n=property_size;return 0;}
        assert(!strcmp(name,"psci-method"));*p="smc";*n=4;return 0;
    }
    static uint32_t reg;
    assert(!strcmp(name,"reg"));reg=offer.mpidr[(uintptr_t)e-1]+bad_reg;*p=&reg;*n=4;return 0;
}
int main(void) {
    assert(lancelot_cpu_request("")==1 && lancelot_cpu_request("cpus=1")==1);
    assert(lancelot_cpu_request(" cpus=8 small_cores=0 ")==8);
    const char *bad[]={"cpus=8 cpus=1","cpus=1 cpus=1","cpus=80","cpus=0","cpus=2","cpumask=ff","bootcpu=0","cpus=-8","cpus=8x"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(lancelot_cpu_request(bad[i])<0);
    for(unsigned boot=0;boot<8;boot++) {
        for(unsigned i=0;i<8;i++)states[i]=1;
        queries=seen=0;assert(lancelot_smp_plan(boot<<8,affinity,&offer));
        assert(lancelot_smp_valid(&offer) && queries==7 && seen==(255u^(1u<<boot)));
        property_size=64;assert(lancelot_smp_boot_allowed(8) && lancelot_smp_boot_allowed(1));
        assert(!lancelot_smp_boot_allowed(2) && !lancelot_smp_boot_allowed(0));
        bad_reg=1;assert(!lancelot_smp_boot_allowed(8));bad_reg=0;
        property_size=63;assert(!lancelot_smp_boot_allowed(8));property_size=64;
        missing=1;assert(!lancelot_smp_boot_allowed(8));missing=0;
        struct lancelot_smp_offer good=offer;
        for(unsigned byte=0;byte<64;byte++) {
            offer=good;((uint8_t *)&offer)[byte]^=0x80;
            assert(!lancelot_smp_valid(&offer));
        }
        for(unsigned cpu=0;cpu<8;cpu++) if(cpu!=boot) {
            for(int64_t state=-9;state<4;state++) if(state!=1) {
                states[cpu]=state;offer=good;assert(!lancelot_smp_plan(boot<<8,affinity,&offer));
                assert(!offer.magic);states[cpu]=1;
            }
        }
    }
    assert(!lancelot_smp_plan(1,affinity,&offer));
    assert(!lancelot_smp_plan(0x800,affinity,&offer));
    puts("MTK SMP: eight real MPIDRs, seven OFF-only queries, malformed/ON/PENDING/UNKNOWN refused, actual kernel guards passed");
}
'''
with tempfile.TemporaryDirectory(prefix='lancelot-smp-') as tmp:
    tmp = Path(tmp)
    for name in ['libkern/libkern.h', 'pexpert/device_tree.h']:
        file = tmp / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text('/* provided by the harness */\n')
    source = tmp / 'test.c'
    source.write_text(harness)
    for compiler in ['cc', 'clang']:
        if not shutil.which(compiler):
            continue
        binary = tmp / ('test-' + compiler)
        subprocess.run([compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', str(tmp), '-I', str(ROOT / 'xnu/files/bsd/dev'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

# Prevent the real startup ordering bug without touching IOSecureBSDRoot:
# PE publication must follow boot CPU initialization and precede PSCI waits.
pe = (ROOT / 'xnu/files/iokit/Kernel/GenericARMPE.cpp').read_text()
start = pe[pe.index('GenericARMPE::start('):pe.index('static long gBootEpoch;')]
boot = start.index('cpus[master_cpu]->run();')
publish = start.index('registerService();', boot)
secondary = start.index('cpus[number]->run();', boot)
assert boot < publish < secondary
print('Real platform expert publication precedes secondary startup waits; secure-root checks unchanged')

# Cross-compile the actual physical entry function: verify no implicit C stack
# or helper call is introduced before the architectural reset vector.
psci = (ROOT / 'xnu/files/osfmk/arm64/psci_smp.c').read_text()
running = psci[psci.index('psci_smp_cpu_running(void)'):psci.index('static void\npsci_smp_clean_segment')]
assert 'printf(' not in running and 'IOLog(' not in running, 'interrupt-off CPU startup must only publish state'
print('Secondary initCPU notification is atomic-only; console output remains in the CPU_ON caller')
begin = psci.index('__attribute__((naked)) static void\npsci_smp_mtk_entry(')
end = psci.index('\nstatic uint32_t psci_smp_cpu_stage', begin)
with tempfile.TemporaryDirectory(prefix='lancelot-entry-') as tmp:
    tmp = Path(tmp)
    source = tmp / 'entry.c'
    source.write_text(psci[begin:end] + '\nvoid *const entry_reference=(void *)&psci_smp_mtk_entry;\n')
    obj = tmp / 'entry.o'
    subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-mgeneral-regs-only',
                    '-fno-stack-protector', '-O1', '-Wall', '-Wextra', '-Werror', '-c', str(source), '-o', str(obj)], check=True)
    dump = subprocess.check_output(['llvm-objdump', '-d', str(obj)], text=True)
    assert re.search(r'\bbr\s+x16\b', dump)
    assert not re.search(r'\b(?:bl|blr|sp)\b', dump), dump
    assert re.search(r'\beret\b', dump), 'EL2 must use exception return, not branch to EL1 code'
    for register in ['hcr_el2', 'cnthctl_el2', 'cntvoff_el2', 'spsr_el2', 'elr_el2', 'vbar_el2', 'cptr_el2', 'mdcr_el2']:
        assert register in dump.lower(), register
    assert 'b.eq 70f' in psci and '"cmp x9, #8\\n"' in psci, 'EL1 path must bypass EL2 register writes'
    assert '"mov x9, #0x3c5\\n"' in psci, 'AArch64 EL1h with DAIF masked'
    assert '"mov x9, #0x80000000\\n"' in psci, 'HCR RW, no stage2/interrupt routing'
    assert '"cmp x9, #4\\n"' in psci, 'actual EL1 must be checked after ERET'
    assert 'PSCI_CPU_ON, phys_id, entry, context' in psci
    assert 'stage == PSCI_SMP_CPU_RUNNING' in psci
    print('Actual AArch64 entry: stackless, guarded EL1/EL2, ERET to EL1h, timer/GIC/trap setup, original reset validation; OFF is not RUNNING')
