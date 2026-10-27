#!/usr/bin/env python3
"""Actual firmware adapter + shared private core, GCC/Clang ASan/UBSan RAM tests."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    path='/mnt/'+ROOT.drive[0].lower()+ROOT.as_posix()[2:]
    subprocess.run(['wsl','-d','Ubuntu','--','python3',path+'/tools/test-lancelot-uefi-dma.py'],check=True)
    sys.exit(0)
paths=[ROOT/'xnuloader/lancelot_gpu_dma.h',ROOT/'xnuloader/lancelot_gpu_prepare.h',
    ROOT/'xnu/files/bsd/dev/lancelot_mali_private_dma.h',ROOT/'xnu/files/bsd/dev/lancelot_mali_core.c']
hashes=lambda:[hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
before=hashes()
with tempfile.TemporaryDirectory(prefix='uefi-private-dma-',dir=ROOT/'local') as temp:
    for cc in ['cc','clang']:
        binary=Path(temp)/cc
        subprocess.run([cc,'-std=c11','-DLM_HOST_TEST','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-I'+str(ROOT/'xnuloader'),'-I'+str(ROOT/'xnu/files/bsd/dev'),
            str(ROOT/'tools/mali-tests/uefi_dma_test.c'),str(ROOT/'xnu/files/bsd/dev/lancelot_mali_core.c'),
            '-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=0'
        subprocess.run([str(binary),'quarantine'],check=True,env=env)
        lifecycle=Path(temp)/(cc+'-lifecycle')
        subprocess.run([cc,'-std=c11','-DLM_HOST_TEST','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-I'+str(ROOT/'xnuloader'),'-I'+str(ROOT/'xnu/files/bsd/dev'),
            str(ROOT/'tools/mali-tests/uefi_lifecycle_test.c'),str(ROOT/'xnu/files/bsd/dev/lancelot_mali_core.c'),
            '-o',str(lifecycle)],check=True)
        subprocess.run([str(lifecycle)],check=True)
        subprocess.run([str(lifecycle),'quarantine'],check=True,env=env)
assert before==hashes()
print('Firmware private DMA adapter + actual core GCC/Clang ASan/UBSan PASS; no hardware or ownership claim')
