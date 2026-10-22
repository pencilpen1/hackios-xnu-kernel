#!/usr/bin/env python3
"""Exercise the linked-code verifier with actual assembled good/bad macros."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).read_text()
macro = re.search(r"\.macro LOADER_CHECKPOINT\n.*?\.endmacro", source, re.S)[0]
macro = macro.replace("EXT(loader_stub_slot)", "_loader_stub_slot")
verifier = str(root / "tools/verify-xnu-checkpoint.py")
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for label, candidate, expected in (
        ("good", macro, 0),
        ("bad", macro.replace("\tbic\t\tx11, x11, #4\n", ""), 1),
    ):
        fixture = ".text\n" + candidate + "\n.globl _start_first_cpu\n_start_first_cpu:\n"
        fixture += "\n".join(f"LOADER_CHECKPOINT {n}" for n in range(1, 6))
        fixture += "\nret\n.data\n_loader_stub_slot:\n.space 32\n"
        path = directory / label
        path.with_suffix(".s").write_text(fixture)
        subprocess.run(["clang", "--target=arm64-apple-ios16.5", "-c", str(path.with_suffix(".s")),
                        "-o", str(path.with_suffix(".o"))], check=True)
        result = subprocess.run(["python3", verifier, str(path.with_suffix(".o"))], capture_output=True, text=True)
        if result.returncode != expected:
            raise RuntimeError(f"{label}: expected {expected}, got {result.returncode}: {result.stdout}{result.stderr}")
        print(label + ": " + (result.stdout + result.stderr).strip())
print("Verifier accepts repaired machine code and rejects missing cache teardown")
