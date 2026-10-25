#!/usr/bin/env python3
"""Architected cache decode and real stack-independent snapshot compile tests."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    root = f'/mnt/{ROOT.drive[0].lower()}{ROOT.as_posix()[2:]}'
    subprocess.run(['wsl', '-d', 'Ubuntu', '--', 'python3', root + '/tools/test-lancelot-cache.py'], check=True)
    sys.exit(0)

test = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "lancelot_cache.h"
int main(void) {
    struct lancelot_cache_geometry g;
    assert(lancelot_cache_geometry(2 | (3u<<3) | (127u<<13),0,&g));
    assert(g.line==64 && g.ways==4 && g.sets==128 && g.bytes==32768);
    assert(lancelot_cache_geometry(2 | (7u<<3) | (1023u<<13),0,&g) && g.bytes==524288);
    assert(lancelot_cache_geometry(2 | (15u<<3) | (2047u<<13),0,&g) && g.bytes==2097152);
    assert(lancelot_cache_geometry(2 | (15u<<3) | (2047ULL<<32),1,&g) && g.bytes==2097152);
    assert(!lancelot_cache_geometry(0,2,&g) && !lancelot_cache_geometry(0,0,0));
    for(unsigned ccidx=0;ccidx<2;ccidx++)for(unsigned n=0;n<10000;n++) {
        uint64_t v=(uint64_t)n*0x1234567890abcdefULL;
        assert(lancelot_cache_geometry(v,ccidx,&g));
        assert(g.line>=16 && g.line<=2048 && g.ways && g.sets);
        assert(g.bytes==(uint64_t)g.line*g.ways*g.sets);
    }
    uint32_t bytes=4u<<20;uint8_t *memory=malloc(bytes);uint32_t *order=malloc((bytes/8)*sizeof(*order));
    assert(memory && order);
    for(unsigned line=8;line<=2048;line*=2)for(unsigned size=8192;size<=bytes;size*=2) {
        assert(lancelot_cache_ring(memory,order,size,line,0x1709a1+line+size));
        unsigned n=size/line,index=order[0];
        assert(lancelot_cache_chase(memory,line,n,n,&index) && index==order[0]);
        *(uint32_t *)(memory+(size_t)index*line)=n;
        assert(!lancelot_cache_chase(memory,line,n,1,&index));
    }
    assert(!lancelot_cache_ring(memory,order,1,64,0));
    unsigned index=0;assert(!lancelot_cache_chase(memory,64,1,0x100001,&index));
    free(memory);free(order);
    puts("Cache geometry: legacy/CCIDX, 32KiB/512KiB/2MiB fixtures and 20000 bounds checks passed");
    puts("Working-set fixture: all line sizes, complete randomized cycles, corrupt indices fail before next access");
}
'''
with tempfile.TemporaryDirectory(prefix='lancelot-cache-') as tmp:
    tmp = Path(tmp)
    source = tmp / 'test.c'
    source.write_text(test)
    for cc in ['cc', 'clang']:
        binary = tmp / ('test-' + cc)
        subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', str(ROOT / 'xnu/files/bsd/dev'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    production = (ROOT / 'xnu/files/osfmk/arm64/psci_smp.c').read_text()
    begin = production.index('static void\npsci_smp_capture_cache(')
    end = production.index('\nvoid\npsci_smp_report_caches(', begin)
    snapshot = production[begin:end]
    assert 'printf(' not in snapshot and 'IOLog(' not in snapshot
    assert '__builtin_arm_wsr64("CSSELR_EL1", saved)' in snapshot
    assert not any(op in snapshot for op in ['"SCTLR_EL1",', '"dc ', '"ic ', '"tlbi ', 'ml_io_map'])
    harness = r'''
#include <stdint.h>
#define MAX_CPUS 8
#define ISB_SY 15
#define os_atomic_store(p,v,o) __atomic_store_n(p,v,__ATOMIC_RELEASE)
static unsigned cpu_number(void) { return 0; }
struct psci_smp_cache_snapshot {
    uint64_t mpidr, midr, sctlr, ctr, clidr, mmfr2, ccsidr[7][2];
    uint32_t ready;
};
static struct psci_smp_cache_snapshot psci_smp_caches[MAX_CPUS];
'''
    cross = tmp / 'snapshot.c'
    cross.write_text(harness + snapshot + '\nvoid run(void) { psci_smp_capture_cache(); }\n')
    subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-mgeneral-regs-only', '-O1',
                    '-Wall', '-Wextra', '-Werror', '-c', str(cross), '-o', str(tmp / 'snapshot.o')], check=True)
    print('Real ARM64 snapshot compiles: local CSSELR restored, no IRQ-off console/cache-disable/MMIO/cache flush')
