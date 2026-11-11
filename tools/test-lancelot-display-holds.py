#!/usr/bin/env python3
"""Exercise actual display lifetime publication, independently of GPU timing."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
 def linux(s):return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
name='xnu/files/iokit/Kernel/MiniIOSurface.cpp';text=(R/name).read_text();begin=text.index('bool MiniIOSurfaceDisplayRetain(');end=text.index('void\nMiniIOSurfaceDrop(',begin)
code='''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static bool held;static void *gSurfaceLock;
static void IOLockLock(void *){assert(!held);held=true;}
static void IOLockUnlock(void *){assert(held);held=false;}
struct Shared{uint64_t useCount=0;};
struct MiniIOSurface{uint32_t useCount[2]={7,5},displayUseCount=0;Shared state;Shared *shared(){return &state;}};
'''+text[begin:end]+'''
int main(){
 MiniIOSurface s;
 assert(MiniIOSurfaceDisplayRetain(&s));assert(MiniIOSurfaceDisplayRetain(&s));assert(MiniIOSurfaceDisplayRetain(&s));
 assert(s.displayUseCount==3&&s.useCount[0]==7&&s.useCount[1]==8&&s.shared()->useCount==((8ull<<32)|7));
 for(unsigned i=0;i<3;i++)MiniIOSurfaceDisplayRelease(&s);
 assert(!s.displayUseCount&&s.useCount[1]==5&&s.shared()->useCount==((5ull<<32)|7));
 MiniIOSurfaceDisplayRelease(&s);assert(s.useCount[1]==5);
 s.useCount[1]=UINT32_MAX;assert(!MiniIOSurfaceDisplayRetain(&s)&&!s.displayUseCount);
 puts("PASS actual display use count: queued/copying holds remain visible, release balances, unrelated user counts preserved, overflow rejected");
}
'''
(O/'test.cpp').write_text(code);runs=[]
for cc in ['clang++','g++']:
 exe=O/cc
 for stage,cmd in [('build',[cc,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(O/'test.cpp'),'-o',str(exe)]),('run',[str(exe)])]:
  r=subprocess.run(cmd,capture_output=True);(O/(cc+'-'+stage+'.log')).write_bytes(r.stdout+r.stderr);assert not r.returncode,r.stderr
  runs.append({'compiler':cc,'stage':stage,'exit':r.returncode})
fb=(R/'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
assert fb.index('MiniIOSurfaceDisplayRetain(fSwapSurfaces[slot][layer])')<fb.index('fSwapsWaiting++;')
start=fb.index('show(swap, surfaces, submitted);');assert start<fb.index('MiniIOSurfaceDisplayRelease(surfaces[layer]);',start)<fb.index('MiniIOSurfaceDrop(surfaces[layer]);',start)
(O/'result.json').write_text(json.dumps({'passed':True,'sources':{n:sha(R/n) for n in [name,'xnu/files/iokit/Kernel/MiniIOSurface.h','xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp','tools/test-lancelot-display-holds.py']},'runs':runs},indent=2)+'\n')
print('PASS display lifecycle and actual presenter call ordering')
