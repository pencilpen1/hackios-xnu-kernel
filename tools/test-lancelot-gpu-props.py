#!/usr/bin/env python3
"""Reject merged diagnostics, malformed envelope/TLV and incomplete features."""
import struct
from lancelot_gpu_props import decode

keys=set([25,26,27,28,29,30,31,32,33,34,51,52,53,54,55,56,57,58,59,81,83]+list(range(35,51)))
def pack(items):
    body=b''.join(struct.pack('<IQ',(key<<2)|3,value) for key,value in items)
    return struct.pack('<4I',0x3150474c,len(body),11|(46<<16),0)+body
items=[(key,0x74021000 if key==55 else key) for key in sorted(keys)]
valid=pack(items);assert len(decode(valid)[2])==len(keys)
def reject(raw):
    try:decode(raw)
    except ValueError:return
    raise AssertionError('Invalid binary accepted')
for cut in range(len(valid)):reject(valid[:cut])
reject(b'JM public ABI 11.46\n'+valid)
reject(valid+b'logging\n')
for offset in [0,4,8,12]:
    changed=bytearray(valid);changed[offset]^=1;reject(changed)
reject(pack(items+items[:1]))
reject(pack(items+[(999,1)]))
reject(pack(items+[(0,1)]))
for missing in keys:reject(pack([(key,value) for key,value in items if key!=missing]))
reject(pack([(key,0x74021001 if key==55 else value) for key,value in items]))
print('Cached JM GPUPROPS: every truncation, merged text, envelope/version, duplicate/key range, missing fields and changed GPU_ID rejected; RAM ONLY')
