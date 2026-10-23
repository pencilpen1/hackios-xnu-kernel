#!/usr/bin/env python3
"""Exercise the actual early-return boot-arg block before/after PE initialization."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "xnu/files/osfmk/arm64/loader_return.c"
text = source.read_text().split("loader_jump(uint64_t reason, uint64_t code)", 1)[1]
block = re.search(r"#if defined\(GENERIC_GIC_V3_IRQ\)\n(.*?)#endif", text, re.S)[1]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static struct { void *bootArgs; } PE_state;
static unsigned parsed, quiesced, masked, shutdowns;
static bool have_bootcpu;
static void gic_v3_irq_quiesce(void) { quiesced++; }
static void gic_v3_irq_cpu_shutdown(void) { shutdowns++; }
static bool PE_parse_boot_argn(const char *key, void *out, int size) {
    /* The real parser dereferences PE_state.bootArgs through PE_boot_args(). */
    assert(PE_state.bootArgs != NULL);
    assert(strcmp(key, "bootcpu") == 0 && size == sizeof(uint32_t));
    parsed++;
    *(uint32_t *)out = 6;
    return have_bootcpu;
}
#define CNTV_CTL_EL0_IMASKED 1
#define __builtin_arm_wsr64(reg, value) ((void)(value), masked++)
static void candidate(void) {
    uint32_t own_core = 0;
    BLOCK
}
int main(void) {
    candidate(); /* checkpoint 10, before PE: parser must not run */
    assert(quiesced == 1 && parsed == 0 && masked == 0 && shutdowns == 0);
    int args;
    PE_state.bootArgs = &args;
    candidate(); /* initialized same-core boot */
    assert(quiesced == 2 && parsed == 1 && masked == 0 && shutdowns == 0);
    have_bootcpu = true;
    candidate(); /* retain secondary-CPU shutdown after PE setup */
    assert(quiesced == 3 && parsed == 2 && masked == 1 && shutdowns == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    for label, candidate in (("fixed", block), ("unguarded", block.replace("PE_state.bootArgs != NULL && ", ""))):
        code = path / (label + ".c")
        binary = path / label
        code.write_text(fixture.replace("BLOCK", candidate))
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        str(code), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if label == "fixed":
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and "PE_state.bootArgs != NULL" in result.stderr, result.stderr
        print(label, "passed" if label == "fixed" else "reproduced pre-PE boot-arg failure")
print("Actual loader return block is safe before PE and preserves initialized CPU shutdown")
