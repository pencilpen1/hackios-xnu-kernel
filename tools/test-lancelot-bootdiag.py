#!/usr/bin/env python3
"""Test actual file-status reporter: missing, unreadable, empty and populated."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name("lancelot-bootdiag.c").read_text()
begin = source.index("static void log_status(")
end = source.index("static void tail(", begin)
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define O_RDONLY 0
struct stat { long long st_size; unsigned st_uid, st_gid, st_mode; };
static int mode, opens, closes;
static char output[512];
static int fake_open(const char *path, int flags, int permissions) {
 assert(!strcmp(path,"/log") && flags==O_RDONLY && permissions==0); opens++;
 if(mode==0){errno=2;return -1;}return 42;
}
static int fake_fstat(int fd, struct stat *st) {
 assert(fd==42);if(mode==1){errno=13;return -1;}
 st->st_size=mode==2 ? 0 : 123456;st->st_uid=501;st->st_gid=501;st->st_mode=0100644;return 0;
}
static int fake_close(int fd) { assert(fd==42);closes++;return 0; }
static int fake_printf(const char *fmt, ...) {
 va_list args;va_start(args,fmt);int n=vsnprintf(output,sizeof(output),fmt,args);va_end(args);return n;
}
#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define printf fake_printf
''' + source[begin:end] + r'''
int main(void) {
 for(mode=0;mode<4;mode++) {
  opens=closes=0;log_status("/log");assert(opens==1 && closes==(mode!=0));
  if(mode==0)assert(strstr(output,"open_errno=2"));
  if(mode==1)assert(strstr(output,"stat_errno=13"));
  if(mode==2)assert(strstr(output,"bytes=0"));
  if(mode==3)assert(strstr(output,"bytes=123456 uid=501 gid=501 mode=0x81a4"));
 }
 puts("4 bootdiag file-status fixtures passed");
}
'''
with tempfile.TemporaryDirectory(prefix="bootdiag-fixture-") as directory:
    path = Path(directory)
    (path / "fixture.c").write_text(fixture)
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                    str(path / "fixture.c"), "-o", str(path / "fixture")], check=True)
    subprocess.run([str(path / "fixture")], check=True)
