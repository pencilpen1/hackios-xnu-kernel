#!/usr/bin/env python3
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from lancelot_gpu_receipt import decode, private_mmu_fault_receipt_accepted

ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    p='/mnt/'+ROOT.drive[0].lower()+ROOT.as_posix()[2:]
    subprocess.run(['wsl','-d','Ubuntu','--','python3',p+'/tools/test-lancelot-gpu-receipt.py'],check=True)
    sys.exit(0)
with tempfile.TemporaryDirectory(prefix='gpu-receipt-',dir=ROOT/'local') as temp:
    for cc in ['cc','clang']:
        binary=Path(temp)/cc
        subprocess.run([cc,'-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-I'+str(ROOT/'xnu/files/bsd/dev'),str(ROOT/'tools/mali-tests/boot_receipt_test.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
words=[0]*96;words[9]=1;words[27]=0x4c4d4732
payload=struct.pack('<4I4Q96I',0x5244474c,1,432,0,0x12345678,0x42000000,1,0,*words)
crc=zlib.crc32(payload)
header=f'lancelot-gpu-receipt: source=XNU boot=0x12345678 pool=0x42000000 sequence=1 fault=0 size=432 crc={crc:x} verified=1\n'
lines=[f'lancelot-gpu-receipt: word={i} value={v:x}\n' for i,v in enumerate(words)]
text=header+''.join(lines);assert decode(text,0x12345678)['word']==words
def reject(text,boot=0x12345678):
    try:decode(text,boot)
    except ValueError:return
    raise AssertionError('Malformed receipt accepted')
reject(text,0x12345679)
reject(text.replace('value=4c4d4732','value=4c4d4733'))
reject(header+''.join(lines[:27]+lines[28:]))
reject(header+lines[0]+''.join(lines))
words[74]=1;words[75]=0x2408;words[76]=0x888d8f88;words[77]=0x4c8d
words[78]=0x888d8f80;words[79]=0x4c8d;words[80]=1;words[82]=27
payload=struct.pack('<4I4Q96I',0x5244474c,2,432,0,0x12345678,0x42000000,0,0,*words)
header2=f'lancelot-gpu-receipt: source=XNU boot=0x12345678 pool=0x42000000 sequence=0 fault=0 size=432 crc={zlib.crc32(payload):x} verified=1 version=2\n'
text2=header2+''.join(f'lancelot-gpu-receipt: word={i} value={v:x}\n' for i,v in enumerate(words))
assert decode(text2,0x12345678)['word']==words
reject(text2.replace('version=2','version=1'))
reject(text2.replace('value=888d8f80','value=888d8f81'))
words[83]=0xffffff92;words[84]=0x304;words[85]=3;words[86]=1;words[87]=1;words[88]=14
payload=struct.pack('<4I4Q96I',0x5244474c,3,432,0,0x12345678,0x42000000,0,0,*words)
header3=f'lancelot-gpu-receipt: source=XNU boot=0x12345678 pool=0x42000000 sequence=0 fault=0 size=432 crc={zlib.crc32(payload):x} verified=1 version=3\n'
text3=header3+''.join(f'lancelot-gpu-receipt: word={i} value={v:x}\n' for i,v in enumerate(words))
assert decode(text3,0x12345678)['word']==words
reject(text3.replace('version=3','version=2'))
words[92]=1;words[93]=1;words[94]=0x2003080;words[95]=0
payload=struct.pack('<4I4Q96I',0x5244474c,4,432,0,0x12345678,0x42000000,1,0x2003080,*words)
header4=f'lancelot-gpu-receipt: source=XNU boot=0x12345678 pool=0x42000000 sequence=1 fault=2003080 size=432 crc={zlib.crc32(payload):x} verified=1 version=4\n'
text4=header4+''.join(f'lancelot-gpu-receipt: word={i} value={v:x}\n' for i,v in enumerate(words))
assert decode(text4,0x12345678)['word']==words
# The capture reader may inject a progress note between a receipt prefix and
# its remaining fields. The decoder must rejoin the line without weakening CRC.
split_word=text4.replace('lancelot-gpu-receipt: word=82 value=',
    'lancelot-gpu-receipt: \n[capture] XNU runtime KLOG2 connected\nword=82 value=',1)
assert decode(split_word,0x12345678)['word']==words
split_header=text4.replace('lancelot-gpu-receipt: source=',
    'lancelot-gpu-receipt: \n[capture] XNU runtime KLOG2 connected\nsource=',1)
assert decode(split_header,0x12345678)['word']==words
reject(text4.replace('version=4','version=3'))
reject(text4.replace('value=2003080','value=2003081'))

def fault_frame(words,source='XNU',fault=0x02003080):
    payload=struct.pack('<4I4Q96I',0x5244474c,4,432,0,0x12345678,0x42000000,1,fault,*words)
    header=f'lancelot-gpu-receipt: source={source} boot=0x12345678 pool=0x42000000 sequence=1 fault={fault:x} size=432 crc={zlib.crc32(payload):x} verified=1 version=4\n'
    return header+''.join(f'lancelot-gpu-receipt: word={i} value={v:x}\n' for i,v in enumerate(words))
def fault_decode(words,fault=0x02003080):
    return decode(fault_frame(words,fault=fault)+fault_frame(words,'UEFI',fault),0x12345678)
fault_words=[0]*96;fault_words[0]=0xfffffffb
for i in [6,7,8,9,10,11,22,23,80]:fault_words[i]=1
fault_words[15]=6;fault_words[27]=0xb3b2b8cd;fault_words[30]=0x3c3;fault_words[74]=4
fault_words[92]=fault_words[93]=1;fault_words[94]=0x02003080
assert private_mmu_fault_receipt_accepted(fault_decode(fault_words))
assert not private_mmu_fault_receipt_accepted(decode(fault_frame(fault_words),0x12345678))
for index,value in [(0,0),(1,1),(11,0),(12,1),(14,1),(20,1),(21,0xfffffffb),(22,0),(23,0),
                    (26,1),(30,0x3cb),(30,0x2c3),(31,1),(74,3),(80,0),(81,1),(83,1),
                    (86,1),(87,1),(89,1),(91,2),(92,0),(93,0),(94,0x02004080)]:
    changed=fault_words.copy();changed[index]=value
    assert not private_mmu_fault_receipt_accepted(fault_decode(changed)),(index,value)
assert not private_mmu_fault_receipt_accepted(fault_decode(fault_words,0x02004080))
print('Same-boot C/Python receipt size, CRC, complete words, duplicate guards and controlled-fault rejection cases PASS; no MMIO')
