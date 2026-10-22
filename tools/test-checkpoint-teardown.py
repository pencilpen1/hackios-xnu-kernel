#!/usr/bin/env python3
"""Compile the actual XNU checkpoint macro and verify teardown is not the boot path."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

text = Path(sys.argv[1]).read_text()
macro = re.search(r"\.macro LOADER_CHECKPOINT\n.*?\.endmacro", text, re.S)
assert macro, "missing checkpoint macro"
macro = macro[0].replace("EXT(loader_stub_slot)", "_loader_stub_slot")
assert macro.index("b.ne\t1f") < macro.index("mrs\t\tx11, SCTLR_EL1")
assert macro.index("ldr\t\tx9, [x9, #8]") < macro.index("msr\t\tSCTLR_EL1, x11")
for bit in ("#1", "#4", "#0x1000"):
    assert f"bic\t\tx11, x11, {bit}" in macro
assert macro.index("dsb\t\tsy") < macro.index("msr\t\tSCTLR_EL1, x11")
exception = text.split("Lloader_early_exception:\n", 1)[1].split("\t.globl EXT(loader_stub_slot)", 1)[0]
assert exception.index("ldr\t\tx9, [x9, #8]") < exception.index("msr\t\tSCTLR_EL1, x10")
for bit in ("#1", "#4", "#0x1000"):
    assert f"bic\t\tx10, x10, {bit}" in exception
fixture = ".text\n" + macro + "\n.globl _test\n_test:\nLOADER_CHECKPOINT 5\nret\n.data\n_loader_stub_slot:\n.space 32\n"
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.s").write_text(fixture)
    subprocess.run(["clang", "--target=arm64-apple-ios16.5", "-c", str(path / "test.s"),
                    "-o", str(path / "test.o")], check=True)
    result = subprocess.check_output(["llvm-objdump", "-d", str(path / "test.o")], text=True)
    # b.ne must bypass all teardown instructions and land on the normal return.
    branch = re.search(r"b.ne\s+(0x[0-9a-f]+)", result)
    normal = re.search(r"\n\s*([0-9a-f]+):\s+[0-9a-f]+\s+ret", result)
    assert branch and normal and int(branch[1], 16) == int(normal[1], 16), result
print("Checkpoint macro compiled: normal path bypasses MMU/cache teardown; target loaded before teardown")
