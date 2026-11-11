"""Adapt the existing 20H392-checked injection without touching its source tree."""
from pathlib import Path
import argparse
ap=argparse.ArgumentParser();ap.add_argument('source',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
text=a.source.read_text(encoding='utf-8').replace('kern.redwood.say','kern.lancelot_hakios.say');assert not a.output.exists()
assert 'ca_20h392' in text and '.metal_device_array = 0x1e1d8a970UL' in text
anchor='''		say("redwood: camtl=1: CoreAnimation may have the Metal device (it needs "
		    "QuartzCore.framework/default.metallib in the image)\\n");
		return;'''
assert text.count(anchor)==1
replacement='''		void *helper = dlopen("/redwood/lib/liblancelot-metal-ca.dylib", RTLD_NOW | RTLD_LOCAL);
		int (*prepare)(void) = helper ? (int (*)(void))dlsym(helper, "lancelot_metal_ca_prepare") : NULL;
		if (prepare != NULL && prepare()) {
			say("lancelot-metal-ca: 20H392 hooks matched; CoreAnimation may use Metal\\n");
			return;
		}
		say("lancelot-metal-ca: preparation failed; keeping the validated OpenGL ES renderer\\n");'''
text=text.replace(anchor,replacement,1)
a.output.write_text(text,encoding='utf-8',newline='\n')
print('20H392 Metal resource gate added:',a.output)
