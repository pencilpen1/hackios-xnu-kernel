#!/usr/bin/env python3
"""Stable source recipe fingerprint; includes the pinned LMAL UAPI header and every overlay file."""
from pathlib import Path
import hashlib
root=Path(__file__).resolve().parent
repo=root.parents[1]              # tools/mesa-lancelot -> repository root
paths=[]
for folder in ['air','lancelot-drm']:
    paths.extend(p for p in (root/folder).rglob('*') if p.is_file())
paths.extend(p for p in root.iterdir() if p.is_file() and p.suffix in ['.py','.meson','.patch','.sh'])
paths.extend([repo/'xnu/files/iokit/Kernel/lancelot_mali_uapi.h'])
for p in sorted(set(paths),key=lambda p:str(p.relative_to(repo))):
    print(hashlib.sha256(p.read_bytes()).hexdigest(),p.relative_to(repo).as_posix())
