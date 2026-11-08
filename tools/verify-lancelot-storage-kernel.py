"""Verify the linked data geometry and exact native FIFO/control barriers."""
from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, os, re, struct, subprocess, sys

P = Path(__file__).resolve().parent
R = P.parent
if os.name == 'nt':
    script = '/mnt/' + P.drive[0].lower() + P.as_posix()[2:] + '/' + Path(__file__).name
    raise SystemExit(subprocess.call(['wsl', '-d', 'Ubuntu', '--exec', 'python3', '-B', script, *sys.argv[1:]]))
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--kernel', required=True, help='Kernel build result relative to repository root')
a = ap.parse_args()
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
receipt = R / a.kernel
build = json.loads(receipt.read_bytes())
assert build['passed'] and build['full_kernel_compile_link']
kernel = R / build['kernel']['path']
assert sha(kernel) == build['kernel']['sha256']
assert all(sha(R / 'xnu/files' / n) == h for n, h in build['inputs'].items())
E = R / 'out/lancelot/tests'
E.mkdir(parents=True, exist_ok=True)
O = E / ('kernel-adapter-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
O.mkdir()
objdump = Path('/usr/bin/llvm-objdump')
command = [objdump, '-d', '--disassemble-symbols=_mmc_fifo_write_words,_mmc_write_register',
           '--no-show-raw-insn', kernel]
run = subprocess.run(list(map(str, command)), capture_output=True, timeout=90)
log = O / 'linked-adapter.log'
log.write_bytes(run.stdout + run.stderr)
assert run.returncode == 0
asm = run.stdout.decode('utf-8')
parts = {}
for name in ['mmc_fifo_write_words', 'mmc_write_register']:
    match = re.search(r'^([0-9a-f]+) <_' + name + r'>:\n(.*?)(?=\n\n|\Z)', asm, re.M | re.S)
    assert match, name
    lines = []
    for row in match[2].splitlines():
        instruction = re.match(r'^([0-9a-f]+):\s+(\S+)\s*(.*)$', row)
        assert instruction, row
        lines.append((int(instruction[1], 16), instruction[2], instruction[3]))
    barriers = [pc for pc, op, operands in lines if op == 'dsb' and operands == 'sy']
    stores = [pc for pc, op, operands in lines if op == 'str' and operands.startswith('w')]
    assert len(barriers) == 1 and stores and max(stores) < barriers[0]
    assert not any(op == 'bl' for _, op, _ in lines)
    parts[name] = {'instructions': lines, 'barrier': barriers[0]}
fifo = parts['mmc_fifo_write_words']
backedges = []
for pc, op, operands in fifo['instructions']:
    if op not in ('b.ne', 'cbnz'):
        continue
    target = re.search(r'0x([0-9a-f]+)', operands)
    if target and int(target[1], 16) < pc:
        backedges.append(pc)
assert backedges and max(backedges) < fifo['barrier']
raw = kernel.read_bytes()
assert struct.unpack_from('<I', raw)[0] == 0xfeedfacf
sections, symbols = [], {}
offset = 32
for _ in range(struct.unpack_from('<I', raw, 16)[0]):
    cmd, size = struct.unpack_from('<II', raw, offset)
    assert size >= 8 and offset + size <= len(raw)
    if cmd == 0x19:
        count = struct.unpack_from('<I', raw, offset + 64)[0]
        assert size == 72 + 80 * count
        for index in range(count):
            _, _, addr, length, fileoff = struct.unpack_from('<16s16sQQI', raw, offset + 72 + 80 * index)
            sections.append((addr, length, fileoff))
    elif cmd == 2:
        symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', raw, offset + 8)
        assert symoff + nsyms * 16 <= len(raw) and stroff + strsize <= len(raw)
        for index in range(nsyms):
            nameoff, kind, sect, _, value = struct.unpack_from('<IBBHQ', raw, symoff + 16 * index)
            if not nameoff or not sect or kind & 0xe0:
                continue
            nameend = raw.index(b'\0', stroff + nameoff, stroff + strsize)
            name = raw[stroff + nameoff:nameend].decode('utf-8')
            if name in ['_data_block_size', '_lancelot_data_request_lock',
                        '___startup_lck_mtx_spec_lancelot_data_request_lock']:
                assert name not in symbols
                symbols[name] = value
    offset += size
assert len(symbols) == 3, symbols
address = symbols['_data_block_size']
mapped = [(fileoff + address - base) for base, length, fileoff in sections
          if fileoff and base <= address and address + 4 <= base + length]
assert len(mapped) == 1
logical_size = struct.unpack_from('<I', raw, mapped[0])[0]
assert logical_size == 4096
sources = {receipt.relative_to(R).as_posix(): sha(receipt),
           Path(__file__).relative_to(R).as_posix(): sha(Path(__file__)), str(objdump): sha(objdump)}
for name in ['bsd/dev/lancelot_emmc.c', 'bsd/dev/lancelot_data_disk.inc']:
    sources['xnu/files/' + name] = build['inputs'][name]
result = {'passed': True, 'sources': sources, 'kernel': build['kernel'],
          'linked_default_logical_block_bytes': logical_size, 'FIFO_barrier_outside_loop': True,
          'control_register_write_keeps_barrier': True, 'request_mutex_and_startup_record_linked': True,
          'disassembly': {'log': log.name, 'sha256': sha(log)}, 'device_access': False,
          'hardware_performance_verified': False, 'GPU_complete': False}
with (O / 'result.json').open('x', encoding='utf-8') as f:
    json.dump(result, f, indent=2)
print('PASS linked kernel: 4096-byte logical geometry, initialized request mutex, one TX barrier outside loop, per-control-write barrier; ' + str(O), flush=True)
