#!/usr/bin/env python3
"""Compile the actual Mu-Silicium MsdcFifoWrite with an instrumented FIFO."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
function = re.search(r"VOID\s+MsdcFifoWrite\s*\(.*?\n}\n", source, re.S)
if not function:
    raise SystemExit("MsdcFifoWrite not found")
shim = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef void VOID;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uintptr_t UINTN;
typedef struct { UINTN MsdcMmioReg; } MSDC_PRIVATE_DATA;
#define MSDC_TXDATA 0x18
static uint8_t fifo[8192];
static size_t used;
static void MmioWrite8(UINTN reg, UINT8 v) {
    if (reg != 0x1018 || used >= sizeof(fifo)) abort();
    fifo[used++] = v;
}
static void MmioWrite32(UINTN reg, UINT32 v) {
    for (int i = 0; i < 4; i++) MmioWrite8(reg, (UINT8)(v >> (i * 8)));
}
'''
main = r'''
int main(void) {
    _Alignas(8) uint8_t buffer[8200];
    MSDC_PRIVATE_DATA dev = {0x1000};
    for (size_t i=0; i<sizeof(buffer); i++) buffer[i]=(uint8_t)(i*31+0x52);
    for (size_t align=0; align<8; align++) {
        for (size_t n=0; n<=4096; n++) {
            used=0;
            MsdcFifoWrite(&dev,buffer+align,(UINT32)n);
            if (used!=n || memcmp(fifo,buffer+align,n)) {
                fprintf(stderr,"FIFO mismatch: alignment=%zu length=%zu\n",align,n);
                return 1;
            }
        }
    }
    puts("MSDC FIFO: all alignments and lengths 0..4096 preserved");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / "fifo.c"
    binary = Path(directory) / "fifo"
    c.write_text(shim + function[0] + main)
    subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
