#!/usr/bin/env python3
"""Ensure continuous-boot validation rejects checkpoint, USB-wait and timed-return payloads."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
kernel = Path(sys.argv[1]).resolve()
cmdline = "-v cpus=1 touchscreen=0 gpu=0 wifi=0 adsp=0"
with tempfile.TemporaryDirectory() as tmp:
    for label, extra, arguments, valid in (
        ("continuous", ["--ubuntu-boot"], cmdline, True),
        ("graphics", ["--ubuntu-boot"], cmdline.removeprefix("-v "), True),
        ("checkpoint", ["--checkpoint", "10"], cmdline, False),
        ("timer", [], cmdline + " loader_return_after=60", False),
        ("panic", [], cmdline + " return_panic=1", False),
        ("usb", ["--usb"], cmdline, False),
        ("probe", ["--mmu-test"], cmdline, False),
    ):
        path = Path(tmp) / (label + ".xnup")
        subprocess.run([sys.executable, str(root / "xnuloader/mkpayload.py"), "-o", str(path),
                        "kernel=" + str(kernel), "--cmdline", arguments, *extra],
                       check=True, stdout=subprocess.DEVNULL)
        result = subprocess.run([sys.executable, str(root / "tools/test-lancelot-payload.py"),
                                  str(path), "--expect-boot", *(["--expect-graphics"] if label == "graphics" else [])],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == valid, (label, result.stdout, result.stderr)
        print(label + (": accepted" if valid else ": rejected as required"))
    result = subprocess.run([sys.executable, str(root / "tools/test-lancelot-payload.py"),
                             str(Path(tmp) / "continuous.xnup"), "--expect-boot", "--expect-graphics"],
                            capture_output=True, text=True)
    assert result.returncode != 0, "verbose boot unexpectedly accepted as graphics mode"
    print("graphics policy: verbose framebuffer rejected")
print("Continuous boot policy verified against actual generated XNUP files")
