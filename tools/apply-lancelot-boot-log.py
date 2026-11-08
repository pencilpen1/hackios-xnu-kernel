#!/usr/bin/env python3
"""Keep ordinary SMP boot intact and expose startup order for profile=2."""
from pathlib import Path
import argparse, hashlib, json

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('tree', type=Path)
a = ap.parse_args()
p = a.tree / 'osfmk/kern/startup.c'
s = p.read_text(encoding='utf-8')
marker = '/* Lancelot normal-boot startup diagnostics: no checkpoints. */'
if marker not in s:
    anchor = 'kernel_startup_bootstrap(void)\n{'
    assert s.count(anchor) == 1
    s = s.replace(anchor, anchor + '\n\t' + marker + '\n'
                  '\tuint32_t lancelot_profile = 0;\n'
                  '\tif (PE_parse_boot_argn("lancelot_profile", &lancelot_profile, sizeof(lancelot_profile)) && lancelot_profile == 2) {\n'
                  '\t\tstartup_debug |= STARTUP_DEBUG_VERBOSE;\n'
                  '\t}\n', 1)
    for call, label in [('PE_init_iokit();', 'IOKit'), ('bsd_init();', 'BSD')]:
        assert s.count('\t' + call) == 1
        s = s.replace('\t' + call, '\tkprintf("lancelot-boot: ' + label + ' begin\\n");\n\t' + call +
                      '\n\tkprintf("lancelot-boot: ' + label + ' returned\\n");', 1)
    p.write_text(s, encoding='utf-8')
print('Applied normal-boot startup logs; SMP/GPU/storage boot arguments retained:', p)
