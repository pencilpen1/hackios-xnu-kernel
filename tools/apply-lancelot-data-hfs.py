#!/usr/bin/env python3
"""Register public HFS+ as a static native data filesystem in a generated XNU tree."""
from pathlib import Path
import argparse
ap=argparse.ArgumentParser();ap.add_argument('tree',type=Path);a=ap.parse_args();X=a.tree
core=X/'bsd/hfs';assert (core/'lancelot_hfs_config.h').is_file()
p=X/'bsd/conf/files';s=p.read_text();lines=[]
for f in sorted(core.glob('*.c')):
 line='bsd/hfs/'+f.name+'\t\tstandard\n'
 if line.strip() not in s:lines.append(line)
if lines:p.write_text(s+'\n# lancelot native HFS+ data volume\n'+''.join(lines))
p=X/'bsd/conf/Makefile.template';s=p.read_text()
if '# Lancelot HFS+ compile configuration' not in s:
 additions=['\n# Lancelot HFS+ compile configuration\n']
 for f in sorted(core.glob('*.c')):
  additions.append(f.stem+'.o_CFLAGS_ADD += -include ${SRCROOT}/bsd/hfs/lancelot_hfs_config.h -I${SRCROOT}/bsd/hfs -D__APPLE_API_PRIVATE\n')
 p.write_text(s+''.join(additions))
p=X/'bsd/vfs/vfs_conf.c';s=p.read_text()
anchor='enum fs_type_num {'
if 'extern struct vfsops hfs_vfsops;' not in s:
 s=s.replace(anchor,'extern struct vfsops hfs_vfsops;\n\n'+anchor,1)
 anchor='static struct vfstable vfstbllist[] = {'
 assert anchor in s
 entry='''
    { .vfc_vfsops=&hfs_vfsops, .vfc_name="hfs", .vfc_typenum=17,
      .vfc_refcount=0, .vfc_flags=MNT_LOCAL, .vfc_mountroot=NULL,
      .vfc_next=NULL, .vfc_reserved1=0, .vfc_reserved2=0,
      .vfc_vfsflags=VFC_VFSLOCALARGS|VFC_VFS64BITREADY|VFC_VFSNATIVEXATTR|VFC_VFSREADDIR_EXTENDED|VFC_VFSVNOP_PAGEOUTV2|VFC_VFSVNOP_PAGEINV2,
      .vfc_descptr=NULL, .vfc_descsize=0, .vfc_sysctl=NULL },
'''
 s=s.replace(anchor,anchor+entry,1)
 anchor='const struct vnodeopv_desc *vfs_opv_descs[] = {';assert anchor in s
 s=s.replace(anchor,'extern const struct vnodeopv_desc hfs_vnodeop_opv_desc;\nextern const struct vnodeopv_desc hfs_specop_opv_desc;\nextern const struct vnodeopv_desc hfs_fifoop_opv_desc;\n\n'+anchor+'\n &hfs_vnodeop_opv_desc, &hfs_specop_opv_desc, &hfs_fifoop_opv_desc,',1)
 p.write_text(s)
print('Registered static lancelot HFS+ sources and VFS:',X)
