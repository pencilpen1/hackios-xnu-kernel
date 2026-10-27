#!/usr/bin/env python3
"""Test actual firmware-stage GPU adapter/shared platform in RAM, never a phone."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    path='/mnt/'+ROOT.drive[0].lower()+ROOT.as_posix()[2:]
    subprocess.run(['wsl','-d','Ubuntu','--','python3',path+'/tools/test-lancelot-uefi-gpu.py'],check=True)
    sys.exit(0)
files=[ROOT/'xnuloader/lancelot_gpu_prepare.h',ROOT/'xnuloader/lancelot_gpu_features.h',ROOT/'xnuloader/lancelot_pmic.h',
       ROOT/'xnu/files/bsd/dev/lancelot_mali_platform.c']
digest=lambda:[hashlib.sha256(p.read_bytes()).hexdigest() for p in files]
before=digest()
with tempfile.TemporaryDirectory(prefix='lancelot-gpu-uefi-') as temp:
    loader=(ROOT/'xnuloader/lancelot.h').read_text()
    receipt=loader[loader.index('static void lancelot_gpu_save_receipt('):loader.index('static uint64_t lancelot_gpu_now(')]
    probe=loader[loader.index('static bool lancelot_gpu_power_probe('):loader.index('static void\nlancelot_observe_pmic(')]
    (Path(temp)/'loader_probe.h').write_text(receipt+probe)
    for cc in ['cc','clang']:
        binary=Path(temp)/cc
        subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-I'+str(ROOT/'xnuloader'),str(ROOT/'tools/mali-tests/uefi_power_id_test.c'),
                        '-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        probe_binary=Path(temp)/(cc+'-loader')
        subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-Wno-unused-variable','-DLM_HOST_TEST','-fsanitize=address,undefined',
                        '-I'+str(ROOT/'xnuloader'),'-I'+str(ROOT/'xnu/files/bsd/dev'),'-I'+temp,
                        str(ROOT/'tools/mali-tests/loader_handoff_test.c'),
                        str(ROOT/'xnu/files/bsd/dev/lancelot_mali_core.c'),'-o',str(probe_binary)],check=True)
        subprocess.run([str(probe_binary)],check=True)
assert before==digest()
print('Firmware GPU power/ID adapter GCC+Clang ASan+UBSan PASS; no DMA, no hardware proof')
