#!/usr/bin/env python3
"""Generate an audited loader header with extra exact root pins; never edit user source."""
from pathlib import Path
import argparse
import hashlib
import json
import re

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def render(text, registry):
    assert registry['build'] == '20H392' and registry['entry'] == 'ramdisk'
    assert registry['path'] == '/hackios/20H392/touch-hardware-20261024/root.apfs'
    capacity = re.search(r'struct lancelot_ramdisk_variant variants\[(\d+)\];', text)
    match = re.search(r'(\{"ramdisk", "' + re.escape(registry['path']) +
                      r'",\s*\{)((?:\s*\{\d+u,\s*"[0-9a-f]{64}"\},?)+\s*)(\}\},)', text)
    if not match or not capacity or len(re.findall(r'struct lancelot_ramdisk_variant variants\[', text)) != 1:
        raise ValueError('Unexpected bootstrap table; reconcile before building')
    old = re.findall(r'\{(\d+)u,\s*"([0-9a-f]{64})"\}', match[2])
    assert ('6373376', '537d98abb1a73234e62068f1e18dcc8ceffd3e5f0e55cb439423b4a1b9bc8536') in old
    assert len(old) >= 2 and len(set(old)) == len(old) and int(capacity[1]) >= len(old)
    ordered = old[:2]
    for item in registry['additional']:
        assert isinstance(item['bytes'], int) and item['bytes'] > 0 and item['bytes'] % 4096 == 0
        assert re.fullmatch('[0-9a-f]{64}', item['sha256'])
        pin = (str(item['bytes']), item['sha256'])
        if pin not in ordered:
            ordered.append(pin)
    # Preserve exact pins already present in a reviewed source header, including
    # any which are absent from the selected registry. Reapplication is stable.
    ordered += [pin for pin in old if pin not in ordered]
    output = text
    if ordered != old:
        entries = ',\n\t\t  '.join('{' + size + 'u, "' + digest + '"}' for size, digest in ordered)
        output = text[:match.start()] + match[1] + entries + match[3] + text[match.end():]
    output = re.sub(r'struct lancelot_ramdisk_variant variants\[\d+\];',
                    f'struct lancelot_ramdisk_variant variants[{len(ordered)}];', output, count=1)
    # Size is only an allocation/read bound. Different valid images may have
    # identical lengths (root44/root49); choose the identity by size AND hash.
    old_check = '''		static const char digits[] = "0123456789abcdef";
		for (unsigned j = 0; j < 32; j++) {
			if (digits[hash[j] >> 4] != found->hash[j * 2] || digits[hash[j] & 15] != found->hash[j * 2 + 1]) {
				print("ubuntu: SHA256 mismatch %s\\n", files[i].path);
				return 0;
			}
		}'''
    new_check = '''		static const char digits[] = "0123456789abcdef";
		/* The first size match above bounds the read, not the image identity.
		 * Hash once, then try every same-size pin in this file's whitelist. */
		found = 0;
		for (unsigned v = 0; v < sizeof(files[i].variants) / sizeof(files[i].variants[0]); v++) {
			const struct lancelot_ramdisk_variant *candidate = &files[i].variants[v];
			if (!candidate->hash || candidate->size != expected_size) continue;
			unsigned j;
			for (j = 0; j < 32; j++) {
				if (digits[hash[j] >> 4] != candidate->hash[j * 2] ||
				    digits[hash[j] & 15] != candidate->hash[j * 2 + 1]) break;
			}
			if (j == 32) { found = candidate; break; }
		}
		if (!found) {
			print("ubuntu: SHA256 mismatch %s\\n", files[i].path);
			return 0;
		}'''
    if output.count(new_check) == 1 and output.count(old_check) == 0:
        pass
    elif output.count(old_check) == 1 and output.count(new_check) == 0:
        output = output.replace(old_check, new_check, 1)
    else:
        raise ValueError('Unexpected bootstrap digest check; reconcile before building')
    assert 'sha256(data, expected_size, hash);' in output
    assert 'ubuntu: SHA256 mismatch' in output and 'ubuntu: wrong-size' in output
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert args.source.resolve() != args.output.resolve() and not args.output.exists()
    registry = json.loads((HERE / 'root-variants.json').read_text())
    for item in registry['additional']:
        image = REPO / item['image']
        assert image.stat().st_size == item['bytes'] and hashlib.sha256(image.read_bytes()).hexdigest() == item['sha256']
    before = args.source.read_bytes()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(before.decode().replace('\r\n', '\n'), registry), encoding='utf-8')
    assert args.source.read_bytes() == before
    print('PASS: exact size/SHA pins, all same-size candidates checked; source bytes untouched; old roots and digest rejection retained')


if __name__ == '__main__':
    main()
