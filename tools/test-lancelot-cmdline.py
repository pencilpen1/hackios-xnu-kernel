#!/usr/bin/env python3
"""Check the effective Lancelot Ubuntu-boot line against the actual loader ABI limit."""
from pathlib import Path
import argparse
import re
import struct

ROOT=Path(__file__).resolve().parents[1]
LIMIT=int(re.search(r'^#define BOOT_LINE_LENGTH (\d+)$',(ROOT/'xnuloader/loader.c').read_text(),re.M)[1])

def effective_line(cmdline):
    tokens=cmdline.split()
    if any(t.startswith(('battery=','ufs=','ufs_native=','ufs_shared_pa=','battery_shared_pa=','hv_wdt=')) for t in tokens):
        raise ValueError('Additional firmware prefixes must be modeled explicitly')
    prefix='debug=0x8 '
    if not any(t.startswith(('cpus=','cpumask=')) for t in tokens):prefix+='cpus=1 '
    if not any(t.startswith('rd=') for t in tokens):prefix+='rd=md0 '
    line=prefix+cmdline
    if len(line.encode())>=LIMIT:
        raise ValueError(f'Effective boot line is {len(line.encode())} bytes; loader allows at most {LIMIT-1}, including automatic prefix')
    return line

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('payload',type=Path);args=parser.parse_args()
    data=args.payload.read_bytes();assert data[:4]==b'XNUP'
    offset=8;entries={}
    while offset+24<=len(data):
        tag,size,reserved=struct.unpack_from('<16sII',data,offset);offset+=24
        if not tag.strip(b'\0'):break
        assert not reserved and offset+size<=len(data)
        entries[tag.rstrip(b'\0').decode()]=data[offset:offset+size];offset+=(size+15)&~15
    assert entries.get('ubuntu-boot')==b'20H392\0'
    cmdline=entries['cmdline'].rstrip(b'\0').decode()
    line=effective_line(cmdline)
    print(f'PASS: effective loader line {len(line.encode())}/{LIMIT-1} bytes; no token truncation; {args.payload.name}')

if __name__=='__main__':main()
