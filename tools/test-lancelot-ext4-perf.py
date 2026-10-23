#!/usr/bin/env python3
"""Verify ext4 request amplification against the initial native-driver revision.

Counts actual C reader callbacks/512-byte equivalents on a valid synthetic
8 GiB extent tree. These figures are not device throughput or boot timings.
"""
from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
results = {}
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    old = tmp / "initial-ext4.h"
    old.write_bytes(subprocess.check_output([
        "git", "-C", str(root), "show", "e4b4fec:xnu/files/bsd/dev/lancelot_ext4.h"
    ]))
    for name, header in (("initial", old), ("cached", root / "xnu/files/bsd/dev/lancelot_ext4.h")):
        binary = tmp / name
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-fsanitize=address,undefined", '-DEXT4_HEADER="' + str(header) + '"',
                        str(root / "tools/bench-lancelot-ext4.c"), "-o", str(binary)], check=True)
        output = subprocess.check_output([str(binary)], text=True)
        rows = {}
        for line in output.splitlines():
            label, *fields = line.split()
            rows[label] = {key: int(value) for key, value in (field.split("=") for field in fields)}
        results[name] = rows
    for label in ("cold-128KiB", "warm-128KiB", "extent-boundary-128KiB"):
        initial, cached = results["initial"][label], results["cached"][label]
        assert cached["requests"] <= 2 and cached["metadata_reads"] <= 1, cached
        assert cached["cmd17_equivalent"] <= 264 < initial["cmd17_equivalent"], (initial, cached)
    assert results["cached"]["warm-128KiB"]["metadata_reads"] == 0
print(json.dumps(results, indent=2))
print("Byte correctness, extent-boundary isolation and reduced read amplification verified")
