#!/usr/bin/env python3
"""Install MT6768 idle capacity hints without changing scheduler topology."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / "osfmk/kern/sched_prim.c"
source = path.read_text()
marker = "/* Lancelot observed-capacity idle preference. */"
if marker not in source:
    anchor = "processor_t\nchoose_processor("
    assert source.count(anchor) == 1
    source = source.replace(anchor, marker + "\n#if defined(GENERIC_PSCI_SMP)\n"
        "#include <kern/lancelot_sched.h>\n#include <kern/lancelot_sched_hint.inc>\n#endif\n\n" + anchor)
    anchor = "\t/*\n\t * At this point, we may have a processor hint, and we may have"
    assert source.count(anchor) == 1
    source = source.replace(anchor, "#if defined(GENERIC_PSCI_SMP)\n"
        "\tprocessor = lancelot_sched_idle_hint(pset, processor, thread);\n#endif\n\n" + anchor)
    path.write_text(source)
assert source.count("processor = lancelot_sched_idle_hint(pset, processor, thread);") == 1
print("Applied observed-capacity idle scheduler hint")
