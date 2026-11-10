#!/usr/bin/env python3
"""Windows-authoritative GPU lease overlay; NEVER edits the source Mu tree.
Apply fixed old-SHA transformations to a newly materialized isolated tree.
--dependencies copies original dependency worktrees (including user changes),
without .git, rather than symlinking writable build inputs back to originals.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,tarfile,io
ROOT=Path(__file__).resolve().parents[1]
PIN='83f641fd2a642968aac4fb36986956db7076118e'
P='Silicon/MediaTek/MediaTekPkg/'
OLD={P+'Drivers/ClockDxe/ClockDxe.c':'b092711660685fb402a8b6f5d2341d557c71d0abc0c8ab3210673eddea2a5a26',
 P+'Drivers/ClockDxe/ClockDxe.inf':'e06b8f4222ff85562ed5550f2ad3d18fce813d47ad79d45a5475d5ce3a93c93a',
 P+'Drivers/PmicWrapperDxe/PmicWrapperDxe.c':'b01c970fe6a42b09877cb2d0c0e7cdf6bdd104987620341877d65a657718b1cc',
 P+'Drivers/PmicWrapperDxe/PmicWrapperDxe.inf':'b3a5b291c3a33e9cc973655da875f71fd0678837130bd5c5501d9a8006f5bf2c',
 P+'MediaTekPkg.dec':'cddae8a6c9eb3198809beec2b50ee936d84b882315af3196e12550745c63845e'}
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def change(text,old,new):
    assert text.count(old)==1,repr(old);return text.replace(old,new)
def transform(name,t):
    if name.endswith('ClockDxe.c'):
        t=change(t,'#include <Protocol/MtkClock.h>', '#include <Protocol/MtkClock.h>\nSTATIC BOOLEAN mGpuClockWriteFailed;\nSTATIC VOID GpuObserveClockWrite(MTK_CLOCK_CONTROLLER_TYPE,UINT32,UINT32);\n#define ClockSetEnable ClockSetEnableImpl\n#define ClockSetFrequency ClockSetFrequencyImpl\n#define ClockSetParent ClockSetParentImpl')
        t=change(t,'  MmioWrite32 (mClockControllersRegions[Controller].Address + Reg, Value);','  MmioWrite32 (mClockControllersRegions[Controller].Address + Reg, Value);\n  GpuObserveClockWrite (Controller, Reg, Value);')
        t=change(t,'STATIC MTK_CLOCK_PROTOCOL mClock = {','#undef ClockSetEnable\n#undef ClockSetFrequency\n#undef ClockSetParent\n#include "ClockGpuLease.h"\n\nSTATIC MTK_CLOCK_PROTOCOL mClock = {')
        t=change(t,'&gMediaTekClockProtocolGuid, &mClock, NULL','&gMediaTekClockProtocolGuid, &mClock, &gMediaTekGpuLeaseProtocolGuid, &mGpuLease, NULL')
    elif name.endswith('PmicWrapperDxe.c'):
        t=change(t,'PmicWrapperImplRead (','#include "PwrapGpuLease.h"\nPmicWrapperImplRead (')
        # Move include outside the function declaration prefix.
        t=change(t,'STATIC\nEFI_STATUS\nEFIAPI\n#include "PwrapGpuLease.h"\nPmicWrapperImplRead (','#include "PwrapGpuLease.h"\nSTATIC\nEFI_STATUS\nEFIAPI\nPmicWrapperImplRead (')
        for marker,mask in [('PmicWrapperImplWrite (','0xffffu'),('PmicWrapperImplUpdateBits (','Mask')]:
            start=t.index(marker);end=t.index('\n}\n',start)+3;body=t[start:end]
            body=change(body,'  EFI_STATUS Status = EfiAcquireLockOrFail (&mTransactionLock);',
                '  UINT64 GpuToken;\n  EFI_STATUS Status = PwrapGpuBegin (Address, '+mask+', &GpuToken);\n  if (EFI_ERROR (Status)) return Status;\n  Status = EfiAcquireLockOrFail (&mTransactionLock);')
            body=change(body,'  if (EFI_ERROR (Status)) {\n    return Status;\n  }',
                '  if (EFI_ERROR (Status)) {\n    PwrapGpuEnd (GpuToken, TRUE);\n    return Status;\n  }')
            body=change(body,'  EfiReleaseLock (&mTransactionLock);\n  return Status;',
                '  EfiReleaseLock (&mTransactionLock);\n  PwrapGpuEnd (GpuToken, !mPoisoned);\n  return Status;')
            t=t[:start]+body+t[end:]
    elif name.endswith('ClockDxe.inf'):
        t=change(t,'  ClockImplLib','  ClockImplLib\n  UefiLib\n  ArmLib\n  ArmSmcLib\n  BootLogLib')
        t=change(t,'  gMediaTekClockProtocolGuid','  gMediaTekClockProtocolGuid\n  gMediaTekGpuLeaseProtocolGuid')
    elif name.endswith('PmicWrapperDxe.inf'):
        t=change(t,'  gMediaTekGpuMapProtocolGuid','  gMediaTekGpuMapProtocolGuid\n  gMediaTekGpuLeaseProtocolGuid')
    elif name.endswith('.dec'):
        t=change(t,'[Protocols]','[Protocols]\n  gMediaTekGpuLeaseProtocolGuid = {0x583f0279,0x6ae2,0x4f41,{0xa8,0x44,0x67,0x68,0x47,0x50,0x55,0x31}}')
    return t
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path,required=True);ap.add_argument('--dest',type=Path,required=True);ap.add_argument('--dependencies',action='store_true');ap.add_argument('--dependencies-source',type=Path);a=ap.parse_args()
    src=a.source.resolve();dst=a.dest.resolve();assert not dst.exists() and src!=dst and src not in dst.parents
    dependency_source=(a.dependencies_source or src).resolve()
    assert dependency_source!=dst and dst not in dependency_source.parents and dependency_source not in dst.parents
    dependency_manifest=ROOT/'uefi/lancelot-dependency-overlays.json'
    dependency_overlays=json.loads(dependency_manifest.read_text(encoding='utf-8')) if dependency_manifest.is_file() else {'files':{},'base_heads':{}}
    before={n:sha(src/n) for n in OLD};assert before==OLD,'unexpected GPU-target source SHA; never overwrite dirty work'
    head=subprocess.check_output(['git','-C',str(src),'rev-parse','HEAD'],text=True).strip();assert head==PIN
    dst.mkdir(parents=True,exist_ok=False)
    archive_args=['git','-C',str(src),'archive',PIN]
    if not a.dependencies:
        archive_args+=['Silicon/MediaTek/MT6768Pkg','Silicon/MediaTek/MediaTekPkg','Platforms/Xiaomi/lancelotPkg','Tests/PmicWrapper']
    data=subprocess.check_output(archive_args)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for item in archive:
            assert not item.name.startswith('/') and '..' not in Path(item.name).parts
            archive.extract(item,dst,filter='data')
    preserved={};port_inputs={}
    # Repository copies capture the reviewed lancelot firmware behavior. Source
    # worktrees remain untouched; their original hashes are still audited.
    for name in ['Platforms/Xiaomi/lancelotPkg/DeviceBuild.py',P+'Drivers/MsdcDxe/MsdcDxe.c',
                 'Platforms/Xiaomi/lancelotPkg/Library/MemoryMapLib/MemoryMapLib.c']:
        preserved[name]=sha(src/name)
        overlay=ROOT/'uefi/files'/name
        selected=overlay if overlay.is_file() else src/name
        port_inputs[name]=sha(selected);shutil.copy2(selected,dst/name)
    if a.dependencies:
        for name,pin in dependency_overlays['base_heads'].items():
            actual=subprocess.check_output(['git','-C',str(dependency_source/name),'rev-parse','HEAD'],text=True).strip()
            assert actual==pin,'unexpected firmware dependency base: '+name
        for name in ['Mu_Basecore','Binaries','Common/Mu','Common/Mu_OEM_Sample','Silicium-ACPI','Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl']:
            dest=dst/name
            if dest.exists(): assert dest.is_dir() and not list(dest.iterdir())
            shutil.copytree(dependency_source/name,dest,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.git','Build','__pycache__'))
    for name,pin in dependency_overlays['files'].items():
        assert name.startswith('Mu_Basecore/') and not Path(name).is_absolute() and '..' not in Path(name).parts
        overlay=ROOT/'uefi/files'/name;assert sha(overlay)==pin
        target=dst/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(overlay,target)
    for name in OLD:
        assert sha(dst/name)==OLD[name];(dst/name).write_text(transform(name,(dst/name).read_text()))
    files=[P+'Drivers/ClockDxe/ClockGpuLease.h',P+'Drivers/PmicWrapperDxe/PwrapGpuLease.h',P+'Drivers/PmicWrapperDxe/GpuRailMasks.h',P+'Include/Protocol/MtkGpuLease.h']
    for name in files:shutil.copy2(ROOT/'uefi/files'/name,dst/name)
    common=dst/P/'Include/Library/LancelotGpuHandoff.h';common.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/'xnu/files/bsd/dev/lancelot_gpu_handoff.h',common)
    assert before=={n:sha(src/n) for n in OLD}
    assert all(sha(src/n)==h for n,h in preserved.items())
    assert all(sha(dst/n)==h for n,h in port_inputs.items())
    output={n:sha(dst/n) for n in [*OLD,*files,P+'Include/Library/LancelotGpuHandoff.h',*dependency_overlays['files']]}
    (dst/'gpu-lease-source-audit.json').write_text(json.dumps({'base_pin':PIN,'old_SHA':before,'new_SHA':output,'preserved_user_inputs':preserved,'repository_port_inputs':port_inputs,
        'source_root_unchanged':True,'authoritative_generator':sha(Path(__file__)),'peer_permissions_verified':False,'device_access':False},indent=2))
    print('GPU AP_ONLY overlay applied to NEW isolated source tree; originals preserved:',dst)
if __name__=='__main__':main()
