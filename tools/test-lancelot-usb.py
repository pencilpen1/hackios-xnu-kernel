#!/usr/bin/env python3
"""Run the actual MUSB core and host decoder against fault/transport fixtures."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("lancelot_log", root / "tools/read-lancelot-log.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
for size in (0, 1, 63, 64, 65, 4096, 16384):
    data = bytes(i % 256 for i in range(size))
    blob = struct.pack("<8sQQQII", b"KLOG2\0\0\0", 123, 777, 777 + size, zlib.crc32(data), size) + data
    assert module.parse_klog(blob).data == data
    for bad in (blob[:39], blob[:-1], blob + b"x", blob[:32] + bytes([blob[32] ^ 1]) + blob[33:]):
        try:
            module.parse_klog(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted corrupt/truncated snapshot")
with tempfile.TemporaryDirectory() as tmp:
    program = str(Path(tmp) / "test-usb")
    subprocess.run(["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(root / "tools/test-lancelot-usb.c"), "-o", program], check=True)
    subprocess.run([program], check=True)
    # Compile the actual logger's write/snapshot functions with small rings to
    # exhaust head/tail boundaries and wrap cases (MMIO/locking stubbed only).
    source = (root / "xnu/files/osfmk/arm64/loader_return.c").read_text()
    def body(name):
        begin = source.index("\n" + name + "(") + 1
        brace = source.index("{", begin)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[begin:end]
    harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define LOADER_LOG_HEAD 16u
#define LOADER_LOG_RING 32u
#define LOADER_LOG_SIZE 64u
#define DAIFSC_IRQF 0
#define DAIFSC_FIQF 0
#define __builtin_arm_rsr64(x) 0
#define __builtin_arm_wsr(a,b) ((void)0)
#define __builtin_arm_wsr64(a,b) ((void)(b))
#define os_atomic_cmpxchg(p,o,n,m) (*(p)==(o) ? (*(p)=(n),true) : false)
#define os_atomic_store(p,v,m) (*(p)=(v))
static char loader_log[LOADER_LOG_SIZE];
static uint64_t loader_log_written;
static uint32_t loader_log_len, loader_log_ring_at, loader_log_lock;
static bool loader_log_linear, loader_log_wrapped;
'''
    harness += "static void\n" + body("loader_log_put_locked") + "\nsize_t\n" + body("loader_log_snapshot")
    harness += r'''
int main(void) {
 for (unsigned total=0;total<200;total++) {
  for (unsigned cursor=0;cursor<220;cursor++) {
   for(unsigned capacity=1;capacity<=16;capacity++) {
    uint8_t out[16]; uint64_t start;
    size_t count=loader_log_snapshot(cursor,&start,out,capacity);
    uint64_t expected=cursor>total?0:cursor;
    if(total>48 && expected>=16 && expected<total-32)expected=total-32;
    assert(start==expected);
    uint64_t end=expected<16 && total>16?16:total;
    assert(count==(end-expected<capacity?end-expected:capacity));
    for(unsigned i=0;i<count;i++)assert(out[i]==(uint8_t)(start+i));
   }
  }
  loader_log_put_locked((char)total);
 }
 loader_log_lock=1; uint64_t start=0; uint8_t out[8];
 assert(!loader_log_snapshot(123,&start,out,sizeof(out)) && start==123);
}
'''
    test = Path(tmp) / "ring.c"
    test.write_text(harness)
    subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(test), "-o", program], check=True)
    subprocess.run([program], check=True)
print("Host KLOG2 decoder: lengths, ranges and corruption rejected")
print("Production console ring: head/tail gaps, wrap, future/empty cursor and busy writer passed")
