"""Add the Metal admission test to an already isolated persistent-data init."""
from pathlib import Path
import argparse
ap=argparse.ArgumentParser();ap.add_argument('source',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
text=a.source.read_text(encoding='utf-8');assert not a.output.exists()
anchor='''	check(bind_dir(in_image("/hal"), "/flash/System/Library/Audio/Plug-Ins/HAL"),
	    "our audio device, not Apple's plug-ins");'''
assert text.count(anchor)==1
text=text.replace(anchor,anchor+'''
	const char *metal = boot_arg("metal");
	if (metal != NULL && strcmp(metal, "1") == 0) {
		check(mount("bindfs", "/flash/System/Library/Extensions", MNT_RDONLY | MNT_UNION,
		    (void *)in_image("/extensions")) == 0, "overlay the native Metal bundle on the read-only iOS extensions");
	}''',1)
anchor='int\nmain(int argc, char **argv, char **envp)'
assert text.count(anchor)==1
text=text.replace(anchor,'#include "lancelot_metal_init.inc"\n\n'+anchor,1)
anchor='\tconfigure_vnodes();\n\trun_rc(in_image("/etc/rc"));'
assert text.count(anchor)==1
text=text.replace(anchor,'\tconfigure_vnodes();\n\tif (!lancelot_metal_prepare()) {\n'
    '\t\tprintf("init: Metal admission failed; discovery remains off for this boot\\n");\n\t}\n'
    '\trun_rc(in_image("/etc/rc"));',1)
a.output.write_text(text,encoding='utf-8',newline='\n')
print('Metal init admission added:',a.output)
