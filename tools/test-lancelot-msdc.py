#!/usr/bin/env python3
"""Run counted-PIO and verified SDR-mode negotiation simulations under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / "msdc-test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", str(root / "tools/test-lancelot-msdc.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
