#!/usr/bin/env python3
"""Compare production eMMC IRQ scopes with controlled late completions."""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    def linux(s):return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=['xnu/files/bsd/dev/lancelot_msdc.h','xnu/files/bsd/dev/lancelot_emmc.c',
       'tools/mali-tests/msdc_before_tail.h','tools/lancelot-data/tests/tail_wait_test.c','tools/test-lancelot-emmc-tail.py']
sources={n:sha(ROOT/n) for n in names}
adapter=(ROOT/names[1]).read_text();begin=adapter.index('static void mmc_phase(');pos=adapter.index('{',begin)+1;depth=1
while depth:depth+=(adapter[pos]=='{')-(adapter[pos]=='}');pos+=1
for variant,header in [('before',names[2]),('current',names[0])]:
    d=out/variant;d.mkdir();shutil.copy2(ROOT/header,d/'lancelot_msdc.h')
    (d/'actual-mmc-phase.inc').write_text(adapter[begin:pos]+'\n')
runs=[]
for cc in ['clang','gcc']:
    for variant in ['before','current']:
        exe=out/(cc+'-'+variant);flags=['-DBEFORE_TAIL'] if variant=='before' else []
        build=[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',*flags,'-I'+str(out/variant),str(ROOT/names[3]),'-o',str(exe)]
        for stage,command in [('build',build),('run',[str(exe)])]:
            log=out/(exe.name+'-'+stage+'.log')
            with log.open('xb') as stream:r=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,timeout=90,
                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
            runs.append({'name':exe.name,'stage':stage,'exit':r.returncode,'log':log.name,'sha256':sha(log)})
            (out/'progress.json').write_text(json.dumps(runs,indent=2)+'\n')
            print(exe.name,stage,r.returncode,flush=True)
            if r.returncode:print(log.read_text());raise SystemExit(r.returncode)
assert all(sha(ROOT/n)==h for n,h in sources.items())
(out/'result.json').write_text(json.dumps({'passed':True,'sources':sources,'runs':runs,'hardware_access':False,
    'G4_completion_tail_masks_IRQ_reproduced':True,'only_read_bursts_remain_masked':True,
    'complete_payload_wait_yields':True,'late_errors_not_accepted':True,
    'timings_are_register_model_not_hardware':True},indent=2)+'\n')
print('PASS',out,flush=True)
