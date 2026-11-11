#!/usr/bin/env python3
"""Compare actual prepared Mesa pack macros with the observed E4 overflow."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
 def linux(s):return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
 raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
O=a.out.resolve();O.mkdir(parents=True,exist_ok=False);sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
header=a.source/'src/panfrost/lib/genxml/pan_pack_helpers.h';text=header.read_text()
old='pan_pack((PREFIX2(T, PACKED_T) *)(dst), T, name)';old2='pan_pack_nodefaults((PREFIX2(T, PACKED_T) *)(dst), T, name)'
assert text.count(old)==text.count(old2)==1
before=text.replace(old,'pan_pack((PREFIX2(T, PACKED_T) *)dst, T, name)').replace(old2,'pan_pack_nodefaults((PREFIX2(T, PACKED_T) *)dst, T, name)')
for name,t in [('before',before),('fixed',text)]:
 (O/('macros-'+name+'.h')).write_text(t[t.index('#define PREFIX1('):t.index('#define pan_unpack(')])
runs=[]
for cc in ['clang','gcc']:
 for variant in ['before','fixed']:
  exe=O/(cc+'-'+variant)
  cmd=[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(O),
   '-DMACROS="macros-'+variant+'.h"',str(R/'tools/mali-tests/panfrost_pack_cast_test.c'),'-o',str(exe)]
  r=subprocess.run(cmd,capture_output=True);(O/(exe.name+'-build.log')).write_bytes(r.stdout+r.stderr);assert not r.returncode,r.stderr
  for mode in [[],['nodefaults']]:
   r=subprocess.run([str(exe),*mode],capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
   log=O/(exe.name+('-nodefaults' if mode else '')+'.log');log.write_bytes(r.stdout+r.stderr)
   if variant=='before':assert r.returncode and b'offset=256' in r.stdout and b'AddressSanitizer' in r.stderr
   else:assert not r.returncode and b'offset=16' in r.stdout
   runs.append({'compiler':cc,'variant':variant,'nodefaults':bool(mode),'exit':r.returncode,'expected':True})
(O/'result.json').write_text(json.dumps({'passed':True,'actual_prepared_header':str(header),'header_sha256':sha(header),'runs':runs,
 'sources':{n:sha(R/n) for n in ['tools/test-lancelot-panfrost-pack.py','tools/mali-tests/panfrost_pack_cast_test.c','tools/mesa-lancelot/apply-lancelot.py']},'hardware_verified':False},indent=2)+'\n')
print('PASS: actual prepared macros; both 256-byte overflow cases reproduced and fixed to 16 bytes')
