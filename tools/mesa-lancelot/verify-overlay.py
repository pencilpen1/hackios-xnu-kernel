#!/usr/bin/env python3
"""Reproduce overlays from pinned archive (selected files only), then verify/rebase an existing source. No new full-tree snapshot."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,tarfile
root=Path(__file__).resolve().parent
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--host',type=Path);p.add_argument('--sync',action='store_true');p.add_argument('--verify-pristine',action='store_true');a=p.parse_args()
for target in [a.source,a.host]:
    if target is not None and root not in target.resolve().parents:raise SystemExit('all targets must be isolated tools/mesa children')
archive=root/'build/mesa-25.0.7.tar.xz';expected='592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6'
assert hashlib.sha256(archive.read_bytes()).hexdigest()==expected
fingerprint=subprocess.check_output(['python3',str(root/'fingerprint.py')],text=True)
key=hashlib.sha256(fingerprint.encode()).hexdigest()[:16]
selected=root/'build'/('selected-'+key)/'mesa-25.0.7';selected.mkdir(parents=True,exist_ok=True)
rels=['meson.build','src/compiler/meson.build','src/panfrost/lib/kmod/pan_kmod.h','src/panfrost/lib/kmod/pan_kmod.c','src/panfrost/lib/kmod/pan_kmod_backend.h','src/panfrost/lib/kmod/meson.build','src/gallium/drivers/panfrost/pan_bo.c','src/gallium/drivers/panfrost/meson.build','src/gallium/targets/osmesa/meson.build','src/gallium/targets/osmesa/osmesa_target.c','src/gallium/drivers/panfrost/pan_device.c','src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c','src/gallium/drivers/panfrost/pan_fence.c','src/gallium/drivers/panfrost/pan_context.c','src/gallium/drivers/panfrost/pan_screen.c']
with tarfile.open(archive,'r:xz') as tar:
    for rel in rels:
        data=tar.extractfile('mesa-25.0.7/'+rel).read();dest=selected/rel;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
for script in ['apply-lancelot.py','apply-lancelot-fences.py','apply-lancelot-screen.py']:
    subprocess.run(['python3',str(root/script),str(selected)],check=True)
report={'archive_sha256':expected,'fingerprint_key':key,'selected_overlay_files':{},'changed_existing':[],'pristine_mismatches':[]}
for item in sorted(selected.rglob('*')):
    if not item.is_file():continue
    rel=item.relative_to(selected);data=item.read_bytes();dest=a.source/rel
    report['selected_overlay_files'][str(rel)]=hashlib.sha256(data).hexdigest()
    if not dest.is_file() or dest.read_bytes()!=data:
        report['changed_existing'].append(str(rel))
        if a.sync:dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
# The rest of the existing source must still match the same official archive.
if a.verify_pristine:
    with tarfile.open(archive,'r:xz') as tar:
        checked=0
        for member in tar:
            if not member.isfile():continue
            rel=Path(member.name).relative_to('mesa-25.0.7')
            if str(rel) in report['selected_overlay_files']:continue
            dest=a.source/rel;raw=tar.extractfile(member).read();checked+=1
            if not dest.is_file() or dest.read_bytes()!=raw:report['pristine_mismatches'].append(str(rel))
        report['pristine_files_checked']=checked
if a.host:
    for item in (selected/'src/compiler/air').rglob('*'):
        if item.is_file():
            rel=item.relative_to(selected);dest=a.host/rel
            if a.sync:dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(item.read_bytes())
(a.source/'.lancelot-overlay').write_text(fingerprint) if a.sync else None
out=root/'build'/('overlay-verify-'+key+'.json');out.write_text(json.dumps(report,indent=2))
print('overlay key',key,'selected',len(report['selected_overlay_files']),'rebased files',len(report['changed_existing']),'pristine mismatches',len(report['pristine_mismatches']),'report',out)
if report['pristine_mismatches'] or (report['changed_existing'] and not a.sync):raise SystemExit(1)
