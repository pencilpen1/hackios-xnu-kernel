#!/usr/bin/env python3
"""Sanitized hardware models; never accesses USB, a phone or live MMIO."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == "nt" and shutil.which("cc") is None:
    root = f"/mnt/{ROOT.drive[0].lower()}{ROOT.as_posix()[2:]}"
    subprocess.run(["wsl", "-d", "Ubuntu", "--", "python3", root + "/tools/test-lancelot-touch-hardware.py"], check=True)
    sys.exit(0)

firmware = ROOT / "local/lancelot-touch-android-20261024/firmware-nvt_dj_fw.bin"
with tempfile.TemporaryDirectory(prefix="lancelot-touch-hw-") as tmp:
    binary = Path(tmp) / "fixture"
    subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "xnu/files/bsd/dev"),
                    str(ROOT / "tools/test-lancelot-touch-hardware.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary), *([str(firmware)] if firmware.exists() else [])], check=True)
    # Compile the actual no-MMIO board/bootarg entry guards under a tiny shim.
    source = (ROOT / "xnu/files/bsd/dev/lancelot_touch.c").read_text()
    board = source[source.index("bool\nlancelot_touch_platform("):source.index("\nstatic uint64_t\ntouch_ticks(")]
    entry = source[source.index("void\nlancelot_touch_init("):]
    guard = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_storage.h"
typedef void *DTEntry;
typedef unsigned thread_t;
#define kSuccess 0
#define KERN_SUCCESS 0
static bool started;
static struct lancelot_storage offer;
static unsigned size, stage, cpus, launches;
static bool smp_valid;
static bool lancelot_smp_boot_allowed(unsigned n) { return n==1 || (n==8 && smp_valid); }
static int SecureDTLookupEntry(void *a, const char *name, DTEntry *out) {
    (void)a; assert(!strcmp(name,"/chosen")); *out=(void *)1;return 0;
}
static int SecureDTGetProperty(DTEntry e,const char *name,const void **p,unsigned *n) {
    (void)e; assert(!strcmp(name,"lancelot-emmc"));*p=&offer;*n=size;return 0;
}
static int PE_parse_boot_argn(const char *name,void *out,unsigned bytes) {
    assert(bytes==sizeof(unsigned));*(unsigned *)out=!strcmp(name,"cpus")?cpus:stage;return 1;
}
static void touch_thread(void *a,int b) {(void)a;(void)b;}
static int kernel_thread_start(void (*f)(void *,int),void *p,thread_t *t) {
    assert(f==touch_thread && (uintptr_t)p==stage && lancelot_smp_boot_allowed(cpus));*t=1;launches++;return 0;
}
static void thread_deallocate(thread_t t) {assert(t==1);}
'''
    tests = r'''
int main(void) {
    offer=(struct lancelot_storage){.magic=LANCELOT_STORAGE_MAGIC,.version=2,.size=96,
        .mmio=0x11230000,.partition_offset=0x100000,.partition_bytes=LANCELOT_UBUNTU_BYTES};size=96;
    assert(lancelot_touch_platform());
    for(stage=0;stage<5;stage++)for(cpus=0;cpus<3;cpus++) {
        started=false;launches=0;lancelot_touch_init();assert(launches==(stage>=1&&stage<=3&&cpus==1));
    }
    for(unsigned valid=0;valid<2;valid++) {
        cpus=8;stage=3;started=false;launches=0;smp_valid=valid;
        lancelot_touch_init();assert(launches==valid);
        lancelot_touch_init();assert(launches==valid); /* one owner */
    }
    stage=3;cpus=1;started=false;launches=0;offer.mmio=0xf100000;
    assert(!lancelot_touch_platform());lancelot_touch_init();assert(!launches);
    offer.mmio=0x11230000;offer.magic=0;assert(!lancelot_touch_platform());
    offer.magic=LANCELOT_STORAGE_MAGIC;size=97;assert(!lancelot_touch_platform());
    puts("Actual XNU entry guards: default-off, valid MTK/stage, cpus=1 or verified 8-core offer, one owner");
}
'''
    path = Path(tmp) / "entry.c"
    path.write_text(guard + board + entry + tests)
    subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "xnu/files/bsd/dev"),
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
