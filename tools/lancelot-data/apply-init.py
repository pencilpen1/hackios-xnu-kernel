"""Apply only persistent-data changes to an isolated upstream static init."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('output');a=p.parse_args()
source=Path(a.source);out=Path(a.output);s=source.read_text();assert not out.exists()
def change(old,new):
 global s
 assert s.count(old)==1,(old[:100],s.count(old));s=s.replace(old,new,1)
change('static char args[1024], value[128];','static char args[4096], value[128];')
change('static void\nmount_writable(void)', '#include "lancelot_data_init.inc"\n\nstatic void\nmount_writable(void)')
change('ok = mount("ramfs", "/private/var", 0, NULL) == 0;', 'ok = lancelot_mount_data();')
change('check(ok, "mount a memory filesystem on /private/var");',
 'check(ok, lancelot_data_persistent ? "mount the persistent iOS data volume on /private/var" : "mount the configured writable filesystem on /private/var");\n\tif (ok && lancelot_data_persistent) {\n\t\tok = lancelot_data_prepare_log_cache();\n\t\tcheck(ok, "mount the rebuilt log archives in memory");\n\t}')
change('ok = mkdir(f[4], mode) == 0 && chmod(f[4], mode) == 0 &&',
 'ok = lancelot_data_directory(f[4], mode) && chmod(f[4], mode) == 0 &&')
change('\t\t\tsymlink(f[1], f[2]);','\t\t\tok = lancelot_data_link(f[1], f[2]);')
change('if (!copy_file(in_image(f[5]), f[4], (int)parse_number(f[1], 8), (int)parse_number(f[2], 10),\n\t\t\t    (int)parse_number(f[3], 10))) {',
 'int seed = lancelot_data_seed_template(f[4], in_image(f[5]));\n\t\t\tif (seed < 0 || (seed > 0 && !copy_file(in_image(f[5]), f[4], (int)parse_number(f[1], 8), (int)parse_number(f[2], 10),\n\t\t\t    (int)parse_number(f[3], 10)))) {\n\t\t\t\tok = 0;')
change('\t\t\tfiles++;', '\t\t\tif (ok && seed > 0 && !lancelot_data_sync_seed(f[4], in_image(f[5]))) ok = 0;\n\t\t\tfiles++;')
change('check(ok && made > 50, "create the directories of the data volume");',
 'check(ok && made > 50, "create or verify the directories of the data volume");\n\tif (ok && lancelot_data_persistent) check(lancelot_data_probe(), "synchronize and read back the persistent data volume probe");')
change('symlink("/private/preboot/jb-rw/procursus", "/private/var/jb") == 0)',
 'lancelot_data_link("/private/preboot/jb-rw/procursus", "/private/var/jb"))')
change('static void\nrestore_var(void)\n{','static void\nrestore_var(void)\n{\n\tif (lancelot_data_persistent) return;')
with out.open('x') as f:f.write(s)
print('Persistent Lancelot init overlay applied:',out)
