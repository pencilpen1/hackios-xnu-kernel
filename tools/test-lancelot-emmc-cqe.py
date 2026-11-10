#!/usr/bin/env python3
"""Exercise actual HS400 negotiation and CQHCI DMA under ASan/UBSan."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    def linux(s):return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=['xnu/files/bsd/dev/'+n for n in ['lancelot_msdc.h','lancelot_mmc_mode.h','lancelot_mmc_hs400.h','lancelot_cqhci.h','lancelot_mmc_cqe.inc']]
names+=['tools/lancelot-data/tests/'+n+'_test.c' for n in ['cqhci','hs400']]+['tools/test-lancelot-emmc-cqe.py']
sources={n:sha(ROOT/n) for n in names};runs=[]
for cc in ['clang','gcc']:
    for test in ['cqhci','hs400']:
        exe=out/(cc+'-'+test)
        build=[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'xnu/files/bsd/dev'),
               str(ROOT/'tools/lancelot-data/tests'/(test+'_test.c')),'-o',str(exe)]
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
    'HS400_tuning_and_switch_sequence':True,'CQE_real_descriptor_and_DMA_model':True,
    'cache_sync_and_error_quarantine':True,'hardware_timing_not_yet_verified':True},indent=2)+'\n')
print('PASS',out,flush=True)
