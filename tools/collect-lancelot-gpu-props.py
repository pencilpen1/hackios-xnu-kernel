#!/usr/bin/env python3
"""Small temporary helper reads the existing JM driver's cached properties.
No device memory/MMIO, GPU job/VM, regulator writes, partitions or raw logs.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import shlex
import struct
import subprocess
from lancelot_gpu_props import decode

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--adb',required=True)
parser.add_argument('--serial',default='116745f30503')
parser.add_argument('--helper',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
args.out.mkdir(parents=True,exist_ok=True)
assert not (args.out/'manifest.json').exists(),'Existing evidence cannot be replaced'
records=[]
def run(name,tail):
    r=subprocess.run([args.adb,'-s',args.serial,*tail],capture_output=True,timeout=60)
    for suffix,data in [('',r.stdout),('.stderr',r.stderr)]:
        with (args.out/(name+suffix)).open('xb') as f:f.write(data)
    records.append({'command':tail,'file':name,'exit':r.returncode,
        'bytes':len(r.stdout),'sha256':hashlib.sha256(r.stdout).hexdigest()})
    assert r.returncode==0,('Command failed; logs retained',name)
    return r.stdout
def root(name,command):return run(name,['exec-out','su -c '+shlex.quote(command)])
assert run('device.txt',['shell','getprop','ro.product.device']).strip()==b'lancelot'
assert b'uid=0(' in root('root.txt','id')
root('android-gpu.txt','set -eu; uname -r; cat /sys/class/misc/mali0/device/gpuinfo; '
     'cat /sys/module/mali_kbase/version; sha256sum /dev/block/by-name/boot')
remote='/data/local/tmp/lancelot-gpu-cached-props-20261026'
root('temporary-guard.txt','set -eu; [ ! -e '+remote+' ]')
run('push.txt',['push',str(args.helper),remote])
try:
    root('chmod.txt','chmod 700 '+remote)
    raw=root('gpuprops.bin',remote)
finally:
    root('cleanup.txt','rm '+remote)
raw,version,values=decode(raw)
result={'captured_at':datetime.datetime.now().astimezone().isoformat(),
    'source':'Android driver cached GPUPROPS, not live loader MMIO',
    'jm_public_abi':str(version&65535)+'.'+str(version>>16),
    'raw_sha256':hashlib.sha256(raw).hexdigest(),'properties':values,
    'gpu_id':hex(values[55]['value']),'mmu_features':hex(values[32]['value']),
    'va_bits':values[32]['value']&255,'pa_bits':(values[32]['value']>>8)&255,
    'as_present':hex(values[33]['value']),'js_present':hex(values[34]['value']),
    'shader_present':hex(values[25]['value']),'tiler_present':hex(values[26]['value']),
    'l2_present':hex(values[27]['value']),'job_submitted':False,'driver_source_match_proven':False}
(args.out/'properties.json').write_text(json.dumps(result,indent=2)+'\n')
(args.out/'manifest.json').write_text(json.dumps({'records':records,
    'helper_sha256':hashlib.sha256(args.helper.read_bytes()).hexdigest(),
    'temporary_small_helper_removed':True,'partitions_written':False,'raw_mmio':False,
    'set_flags_or_gpu_context_requested':False,
    'driver_internal_open_handshake_bookkeeping_not_proven':True,
    'gpu_job_or_dma_started':False},indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='properties'},indent=2))
