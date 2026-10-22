#!/usr/bin/env python3
"""Check the actual generated MTK payload, including the boot-CPU-only policy."""
from pathlib import Path
import struct
import sys

payload = Path(sys.argv[1]).read_bytes()
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
assert entries["checkpoint"] in (b"1\0", b"2\0", b"3\0", b"4\0", b"5\0", b"10\0", b"998\0")
print("XNUP validated: Mach-O kernel, bounded entries, boot-CPU-only arguments, checkpoint", entries["checkpoint"].rstrip(b"\0").decode())
