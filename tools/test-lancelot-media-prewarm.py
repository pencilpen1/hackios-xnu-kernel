#!/usr/bin/env python3
"""ASan/UBSan host fixtures for the exact new media guard; never executes a hook."""
import argparse
from pathlib import Path
import subprocess
import tempfile


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *Method;
typedef void (*IMP)(void);
typedef struct { const char *dli_fname; void *dli_fbase; } Dl_info;
struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct uuid_command { uint32_t cmd, cmdsize; unsigned char uuid[16]; };
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_ARM64 0x100000c
#define LC_UUID 0x1b
static uintptr_t slide = 0x161f8000;
static struct { struct mach_header_64 header; struct uuid_command uuid; } image;
static const char *types, *path;
static bool gpu_off;
static int lookup;
static void *base;
static const char *method_getTypeEncoding(Method method) { (void)method; return types; }
static bool boot_arg(const char *what) { assert(!strcmp(what, "gpu=0")); return gpu_off; }
static int dladdr(const void *imp, Dl_info *out) { (void)imp; out->dli_fname = path; out->dli_fbase = base; return lookup; }
'''

SUFFIX = r'''
static void reset(void) {
    memset(&image, 0, sizeof(image));
    image.header.magic = MH_MAGIC_64; image.header.cputype = CPU_TYPE_ARM64;
    image.header.ncmds = 1; image.header.sizeofcmds = sizeof(image.uuid);
    image.uuid.cmd = LC_UUID; image.uuid.cmdsize = sizeof(image.uuid);
    const unsigned char uuid[16] = {0x45,0xd3,0x74,0x88,0x2c,0xc7,0x30,0x28,0x8a,0x4d,0x93,0x6b,0xce,0x7a,0x22,0x0b};
    memcpy(image.uuid.uuid, uuid, sizeof(uuid));
    types = "v24@0:8I16B20"; path = "/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture";
    base = &image.header; lookup = 1; gpu_off = true;
}
int main(void) {
    IMP imp = (IMP)(0x1a2165b8cUL + slide);
    reset(); assert(portrait_prewarm_20h392((Method)1, imp));
    assert(!portrait_prewarm_20h392(NULL, imp));
    assert(!portrait_prewarm_20h392((Method)1, (IMP)((uintptr_t)imp + 4)));
    assert(!portrait_prewarm_20h392((Method)1, NULL));
    types = "v24@0:8i16B20"; assert(!portrait_prewarm_20h392((Method)1, imp));
    types = NULL; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); gpu_off = false; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); lookup = 0; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); base = NULL; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); path = NULL; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); path = "/wrong/CMCapture"; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.uuid.uuid[0] ^= 1; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.header.magic = 0; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.header.cputype = 7; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.header.ncmds = 257; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.header.sizeofcmds = 65537; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.header.sizeofcmds = 4; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.uuid.cmdsize = 4; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.uuid.cmdsize = 28; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.uuid.cmdsize = 8; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.uuid.cmd = 0; assert(!portrait_prewarm_20h392((Method)1, imp));
    reset(); image.header.ncmds = 0; assert(!portrait_prewarm_20h392((Method)1, imp));
    puts("22 portrait-prewarm guard fixtures passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text()
    begin = source.index("static bool\nportrait_prewarm_20h392(")
    end = source.index("\n/* ---- the flashlight", begin)
    guard = source[begin:end]
    with tempfile.TemporaryDirectory(prefix="media-prewarm-") as directory:
        path = Path(directory)
        (path / "fixture.c").write_text(PREFIX + guard + SUFFIX)
        subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                        str(path / "fixture.c"), "-o", str(path / "fixture")], check=True)
        subprocess.run([str(path / "fixture")], check=True)


if __name__ == "__main__":
    main()
