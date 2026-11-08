#!/usr/bin/env python3
"""Verify HFS registration in the linked ARM64 kernel, including the old failure.

This reads real Mach-O startup/constructor tables and the native hfs_init BL
instructions. A source-only check cannot prove which registration was linked.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, struct, subprocess, sys

R = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    path = '/mnt/' + R.drive[0].lower() + R.as_posix()[2:] + '/tools/test-lancelot-static-hfs.py'
    args = [('/mnt/' + x[0].lower() + x[2:].replace('\\', '/'))
            if re.match(r'^[A-Za-z]:[\\/]', x) else x for x in sys.argv[1:]]
    raise SystemExit(subprocess.call(['wsl', '-d', 'Ubuntu', '--exec', 'python3', '-B', path, *args]))

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--kernel', type=Path, required=True)
ap.add_argument('--tree', type=Path, required=True)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--expect-broken', action='store_true')
a = ap.parse_args()
D = a.out.resolve()
D.mkdir(parents=True, exist_ok=False)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
raw = a.kernel.read_bytes()
assert len(raw) >= 32 and struct.unpack_from('<I', raw)[0] == 0xfeedfacf
ncmds = struct.unpack_from('<I', raw, 16)[0]
off = 32
sections = []
symtab = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', raw, off)
    assert size >= 8 and off + size <= len(raw)
    if cmd == 0x19:
        nsects = struct.unpack_from('<I', raw, off + 64)[0]
        assert size == 72 + 80 * nsects
        for i in range(nsects):
            q = off + 72 + 80 * i
            name, seg, addr, length, foff = struct.unpack_from('<16s16sQQI', raw, q)
            sections.append((name.rstrip(b'\0').decode(), seg.rstrip(b'\0').decode(), addr, length, foff))
    elif cmd == 2:
        assert symtab is None
        symtab = struct.unpack_from('<IIII', raw, off + 8)
    off += size
assert symtab is not None
symoff, nsyms, stroff, strsize = symtab
assert stroff + strsize <= len(raw) and symoff + nsyms * 16 <= len(raw)
names = {}
duplicate_local_names = 0
for i in range(nsyms):
    index, kind, sect, desc, value = struct.unpack_from('<IBBHQ', raw, symoff + i * 16)
    if not index or kind & 0xe0 or not sect:
        continue
    assert index < strsize
    end = raw.index(b'\0', stroff + index, stroff + strsize)
    name = raw[stroff + index:end].decode(errors='strict')
    if name in names:
        assert not name.startswith(('_hfs_', '_sysctl_', '___startup_SYSCTL')), name
        duplicate_local_names += 1
        continue
    names[name] = value

def vmbytes(addr, count):
    candidates = [s for s in sections if s[2] <= addr and addr + count <= s[2] + s[3] and s[4]]
    assert len(candidates) == 1, (hex(addr), count, candidates)
    _, _, base, _, fileoff = candidates[0]
    start = fileoff + addr - base
    assert start + count <= len(raw)
    return raw[start:start + count]

hooks = [s for s in sections if s[0] == '__init_entry_set']
assert len(hooks) == 1 and hooks[0][3] > 0 and hooks[0][3] % 24 == 0
hook = hooks[0]
entries = list(struct.iter_unpack('<IIQQ', vmbytes(hook[2], hook[3])))
early = names['_sysctl_register_oid_early']
constructors = []
for s in sections:
    if s[0] == '__mod_init_func':
        assert s[3] % 8 == 0
        constructors += [x[0] for x in struct.iter_unpack('<Q', vmbytes(s[2], s[3]))]

sysctl_header = a.tree / 'bsd/sys/sysctl.h'
sysctl_impl = a.tree / 'bsd/kern/kern_newsysctl.c'
h = sysctl_header.read_text()
c = sysctl_impl.read_text()
permanent = int(re.search(r'#define\s+CTLFLAG_PERMANENT\s+(0x[0-9a-fA-F]+)', h)[1], 16)
assert re.search(r'if\s*\(new_oidp->oid_kind\s*&\s*CTLFLAG_PERMANENT\)\s*\{\s*panic\("Use sysctl_register_oid_early', c)
assert re.search(r'oid_number;\s*int\s+oid_kind;', h)
sources = {str(sysctl_header): sha(sysctl_header), str(sysctl_impl): sha(sysctl_impl)}
nodes = []
for p in sorted((a.tree / 'bsd/hfs').glob('*.c')):
    for m in re.finditer(r'HFS_SYSCTL\(\s*(\w+)\s*,\s*(\w+)\s*,\s*([^,]+)\s*,\s*(\w+)', p.read_bytes().decode('latin-1')):
        _, parent, number, name = m.groups()
        symbol = '_sysctl_' + parent + '_' + name
        assert symbol in names, symbol
        addr = names[symbol]
        flags = struct.unpack_from('<I', vmbytes(addr, 24), 20)[0]
        matching = [e for e in entries if e[2] == early and e[3] == addr]
        ctor_name = '_hfs_sysctl_register_' + parent + '_' + name
        ctor = names.get(ctor_name)
        nodes.append({'symbol': symbol, 'permanent': bool(flags & permanent),
                      'early_registrations': len(matching),
                      'late_constructor_in_table': ctor is not None and ctor in constructors})
        sources[str(p)] = sha(p)
assert len(nodes) == 9 and len({n['symbol'] for n in nodes}) == 9, nodes
assert all(n['permanent'] and n['early_registrations'] == 1 for n in nodes), nodes

begin = names['_hfs_init']
end = min(v for v in names.values() if v > begin)
instructions = vmbytes(begin, end - begin)
calls = []
for i in range(0, len(instructions) - 3, 4):
    word = struct.unpack_from('<I', instructions, i)[0]
    if word & 0xfc000000 == 0x94000000:
        imm = word & 0x03ffffff
        if imm & (1 << 25):
            imm -= 1 << 26
        calls.append(begin + i + imm * 4)
manual = names.get('_hfs_sysctl_register')
duplicate_path = (manual is not None and manual in calls and
                  any(n['late_constructor_in_table'] for n in nodes))
checks = {'all_nine_permanent_nodes_have_one_early_registration': True,
          'duplicate_other_local_symbol_names': duplicate_local_names,
          'late_constructor_count': sum(n['late_constructor_in_table'] for n in nodes),
          'hfs_init_calls_late_registration': manual is not None and manual in calls,
          'native_dynamic_registration_rejects_permanent_nodes': True,
          'known_broken_path_detected': duplicate_path}
passed = duplicate_path if a.expect_broken else (manual is None and checks['late_constructor_count'] == 0)
result = {'passed': passed, 'expect_broken': a.expect_broken, 'checks': checks, 'nodes': nodes,
          'runs': [{'name': 'linked_HFS_registration', 'exit': 0 if passed else 1}],
          'recipe_sha256': sha(Path(__file__)),
          'sources': sources, 'kernel': {'path': str(a.kernel), 'sha256': sha(a.kernel), 'bytes': len(raw)},
          'runtime_boot_verified': False, 'GPU_complete': False}
with (D / 'result.json').open('x', encoding='utf-8') as f:
    json.dump(result, f, indent=2)
print(json.dumps({'passed': passed, 'checks': checks, 'evidence': str(D)}, indent=2))
raise SystemExit(0 if passed else 1)
