#!/usr/bin/env python3
"""Check the actual generated MTK payload, including the boot-CPU-only policy."""
import argparse
from pathlib import Path
import struct

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("payload", type=Path)
ap.add_argument("--expect-boot", action="store_true", help="require uninterrupted boot, without diagnostic stops")
args = ap.parse_args()
payload = args.payload.read_bytes()
magic, size = struct.unpack_from("<II", payload)
assert magic == 0x50554e58 and size == len(payload) - 8
entries = {}
offset = 8
terminated = False
while offset + 24 <= len(payload):
    name, length, reserved = struct.unpack_from("<16sII", payload, offset)
    offset += 24
    if name == bytes(16):
        terminated = True
        break
    assert b"\0" in name and not reserved
    key = name.split(b"\0", 1)[0].decode("ascii")
    assert key not in entries and offset + length <= len(payload)
    entries[key] = payload[offset:offset + length]
    offset += (length + 15) & ~15
assert terminated
assert entries["kernel"][:4] == bytes.fromhex("cffaedfe")
cmdline = entries["cmdline"].rstrip(b"\0").decode()
assert "cpus=1" in cmdline.split()
for word in cmdline.split():
    assert not word.startswith(("bootcpu=", "cpumask=", "ufs=", "ufs_native=", "ufs_write=")), word
checkpoint = entries.get("checkpoint")
if args.expect_boot:
    assert checkpoint is None, "continuous boot must not include a checkpoint entry"
assert "mmu-test" not in entries, "payload must not auto-run the MMU teardown probe"
if checkpoint is None:
    assert "usb" not in entries, "continuous boot must not wait for loader commands"
    for word in cmdline.split():
        assert not word.startswith(("loader_return_after=", "return_panic=", "return_cpu=")), word
    mode = "continuous boot (no checkpoint or timed return)"
else:
    assert checkpoint in (b"1\0", b"2\0", b"3\0", b"4\0", b"5\0", b"10\0", b"998\0")
    mode = "checkpoint " + checkpoint.rstrip(b"\0").decode()
print("XNUP validated: Mach-O kernel, bounded entries, boot-CPU-only arguments,", mode)
