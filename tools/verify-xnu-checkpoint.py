#!/usr/bin/env python3
"""Check the linked XNU, not just source, contains the checkpoint teardown fix.

Uses LLVM tools from the Linux build environment. Refuses the old prebuilt.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kernel", type=Path)
    args = ap.parse_args()
    symbols = subprocess.check_output(["llvm-nm", "-n", str(args.kernel)], text=True)
    match = re.search(r"^([0-9a-f]+) \w _start_first_cpu$", symbols, re.M)
    if not match:
        raise SystemExit("kernel has no start_first_cpu symbol")
    start = int(match[1], 16)
    disasm = subprocess.check_output(["llvm-objdump", "-d", f"--start-address={start:#x}",
                                     f"--stop-address={start + 0x1000:#x}", str(args.kernel)], text=True)
    instructions = []
    for line in disasm.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(.+)", line)
        if m:
            instructions.append((int(m[1], 16), m[2].split(";")[0].strip()))
    checked = []
    for i, (address, instruction) in enumerate(instructions):
        match = re.fullmatch(r"cmp\s+x10, #(0x[0-9a-f]+|\d+)", instruction)
        if not match:
            continue
        number = int(match[1], 0)
        if number not in range(1, 6):
            continue
        seq = instructions[i + 1:i + 18]
        texts = [text for _, text in seq]
        branch = re.match(r"b.ne\s+(0x[0-9a-f]+)", texts[0])
        target_load = next((j for j, text in enumerate(texts)
                            if re.fullmatch(r"ldr\s+x9, \[x9, #(0x8|8)\]", text)), None)
        disable = next((j for j, text in enumerate(texts)
                        if re.fullmatch(r"msr\s+SCTLR_EL1, x11", text)), None)
        return_branch = next((j for j, text in enumerate(texts)
                             if re.fullmatch(r"br\s+x9", text)), None)
        if not branch or target_load is None or disable is None or return_branch is None:
            raise SystemExit(f"checkpoint {number}: missing teardown instructions")
        if not target_load < disable < return_branch or int(branch[1], 16) <= seq[return_branch][0]:
            raise SystemExit(f"checkpoint {number}: target load order or normal-path branch is unsafe")
        teardown = "\n".join(texts[:disable])
        # LLVM canonicalizes BIC-immediate to AND with the complement mask.
        for mask in ("0xfffffffffffffffe", "0xfffffffffffffffb", "0xffffffffffffefff"):
            if not re.search(r"and\s+x11, x11, #" + mask, teardown):
                raise SystemExit(f"checkpoint {number}: missing MMU/cache mask {mask}")
        if not re.search(r"dsb\s+sy", teardown):
            raise SystemExit(f"checkpoint {number}: missing synchronization before teardown")
        checked.append(number)
    if sorted(checked) != [1, 2, 3, 4, 5]:
        raise SystemExit("not all early checkpoints found: " + str(checked))
    digest = hashlib.sha256()
    with args.kernel.open("rb") as file:
        for data in iter(lambda: file.read(4 << 20), b""):
            digest.update(data)
    print("Linked kernel verified: checkpoints 1..5 repaired; normal boot bypasses teardown")
    print("SHA256", digest.hexdigest())


if __name__ == "__main__":
    main()
