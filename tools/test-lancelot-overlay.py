#!/usr/bin/env python3
"""Verify the XNU overlay applies after redwood.patch and survives a round trip."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
tree = Path(sys.argv[1]).resolve()
base = "c54f35ca767986246321eb901baf8f5ff7923f6a"
original = subprocess.check_output(["git", "-C", str(tree), "show", base + ":osfmk/arm64/start.s"])
with tempfile.TemporaryDirectory() as directory:
    target = Path(directory)
    source = target / "osfmk/arm64/start.s"
    source.parent.mkdir(parents=True)
    source.write_bytes(original)
    subprocess.run(["git", "init", "-q", str(target)], check=True)
    subprocess.run(["git", "-C", str(target), "apply", "--include=osfmk/arm64/start.s",
                    str(root / "xnu/redwood.patch")], check=True)
    redwood = source.read_bytes()
    overlay = str(root / "xnu/lancelot.patch")
    subprocess.run(["git", "-C", str(target), "apply", "--check", overlay], check=True)
    subprocess.run(["git", "-C", str(target), "apply", overlay], check=True)
    modified = source.read_bytes()
    assert modified != redwood
    subprocess.run(["python3", str(root / "tools/test-checkpoint-teardown.py"), str(source)], check=True)
    subprocess.run(["git", "-C", str(target), "apply", "--reverse", overlay], check=True)
    assert source.read_bytes() == redwood
print("XNU overlay applies cleanly after the base patch and reverses without changing it")
