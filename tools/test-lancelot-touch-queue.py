#!/usr/bin/env python3
"""Compile the real redwood_input enqueue/overflow code with a bounded host shim."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "xnu/files/bsd/dev/redwood_input.c"

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TOUCH_QUEUE 512
#define TOUCH_CONTACTS 10
#define REDWOOD_TOUCH_DOWN 1
#define REDWOOD_TOUCH_MOVE 2
#define REDWOOD_TOUCH_UP 3
#define REDWOOD_BUTTON_DOWN 4
#define REDWOOD_BUTTON_UP 5
struct redwood_touch { uint64_t time; uint32_t type, finger, x, y; };
static struct redwood_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;
static bool queued_contact[TOUCH_CONTACTS];
static uint32_t queued_x[TOUCH_CONTACTS], queued_y[TOUCH_CONTACTS];
static bool touch_lat;
static int lock_depth;
static int touch_lock;
static uint64_t clock_ticks;
static uint64_t mach_absolute_time(void) { return ++clock_ticks; }
static void lck_mtx_lock(void *unused) { (void)unused; assert(lock_depth++ == 0); }
static void lck_mtx_unlock(void *unused) { (void)unused; assert(--lock_depth == 0); }
static bool latency_lines(void) { return touch_lat; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
static void redwood_tl(const char *format, ...) { (void)format; }
static void wakeup(void *p) { (void)p; }
static void count_fingers(uint32_t type, uint32_t finger) { (void)type; (void)finger; }
'''

MAIN = r'''
static void drain_and_apply(bool state[10], uint32_t x[10], uint32_t y[10]) {
    while (touch_head != touch_tail) {
        struct redwood_touch t = touch_queue[touch_head++ % TOUCH_QUEUE];
        assert(t.finger < 10);
        if (t.type == REDWOOD_TOUCH_DOWN) {
            assert(!state[t.finger]); state[t.finger] = true;
        } else if (t.type == REDWOOD_TOUCH_MOVE) {
            assert(state[t.finger]);
        } else if (t.type == REDWOOD_TOUCH_UP) {
            /* Mirrors backboardd: an UP for an unknown slot is safely ignored. */
            if (state[t.finger]) state[t.finger] = false;
        } else assert(0);
        x[t.finger] = t.x; y[t.finger] = t.y;
    }
}
int main(void) {
    bool hid[10] = {0}; uint32_t hx[10] = {0}, hy[10] = {0};
    for (unsigned i=0; i<10; ++i) touch_post(REDWOOD_TOUCH_DOWN,i,100+i,200+i,true);
    for (unsigned i=0; i<502; ++i) touch_post(REDWOOD_TOUCH_MOVE,i%10,300+i,400+i,true);
    assert(touch_tail-touch_head == TOUCH_QUEUE);
    /* A release while full must be represented after an explicit synchronization barrier. */
    touch_post(REDWOOD_TOUCH_UP,9,999,888,true);
    assert(touch_tail-touch_head <= TOUCH_QUEUE);
    drain_and_apply(hid,hx,hy);
    for (unsigned i=0; i<9; ++i) assert(hid[i]);
    assert(!hid[9]);
    assert(hx[9] == 999 && hy[9] == 888);
    /* Fill again, drop a producer DOWN into an overflow and prove it is delivered. */
    for (unsigned i=0; i<TOUCH_QUEUE; ++i) touch_post(REDWOOD_TOUCH_MOVE,0,1000+i,2000+i,true);
    touch_post(REDWOOD_TOUCH_DOWN,9,1234,2345,true);
    drain_and_apply(hid,hx,hy);
    for (unsigned i=0; i<10; ++i) assert(hid[i]);
    assert(hx[9] == 1234 && hy[9] == 2345);
    /* A second overflow with an UP must leave no stuck contact. */
    for (unsigned i=0; i<TOUCH_QUEUE; ++i) touch_post(REDWOOD_TOUCH_MOVE,1,3000+i,4000+i,true);
    touch_post(REDWOOD_TOUCH_UP,0,4321,5432,true);
    drain_and_apply(hid,hx,hy);
    assert(!hid[0]); for (unsigned i=1; i<10; ++i) assert(hid[i]);
    puts("touch queue: 10-contact overflow barriers preserve DOWN/UP and coordinates");
}
'''


if os.name == "nt" and shutil.which("cc") is None:
    drive_root = ROOT.drive[0].lower()
    wsl_root = f"/mnt/{drive_root}{ROOT.as_posix()[2:]}"
    subprocess.run(["wsl", "-d", "Ubuntu", "--", "bash", "-lc",
                    f"cd '{wsl_root}' && python3 tools/test-lancelot-touch-queue.py --inside-wsl"], check=True)
    sys.exit(0)

source = SOURCE.read_text()
start = source.index("static void\ntouch_enqueue(")
end = source.index("\nvoid\nredwood_touch_post(", start)
actual = source[start:end]
with tempfile.TemporaryDirectory(prefix="lancelot-touch-queue-") as directory:
    root = Path(directory)
    c = root / "queue.c"
    binary = root / "queue"
    c.write_text(SHIM + "\n" + actual + "\n" + MAIN)
    subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
