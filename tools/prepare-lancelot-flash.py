#!/usr/bin/env python3
"""Convert local IPSW DMGs into consecutive APFS containers for the Ubuntu partition.

Produces a local image only. It never accesses a device or writes a partition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--workspace", required=True, type=Path)
    ap.add_argument("--source", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    sys.path.insert(0, str(args.workspace.resolve() / "tools/apfs"))
    from dmg import Dmg, read_gpt, APFS_PART_TYPE
    metadata = json.loads((args.source / "source.json").read_text())
    metadata["containers"] = []
    args.out.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with args.out.open("xb") as out:
        for name in ("rootfs.dmg", "cryptex-os.dmg", "cryptex-app.dmg"):
            path = args.source / name
            with path.open("rb") as f:
                f.seek(-512, 2)
                udif = f.read(4) == b"koly"
            image = Dmg(path) if udif else None
            raw = None if image else path.open("rb")
            def read(offset, length):
                if image:
                    return image.read(offset, length)
                raw.seek(offset)
                return raw.read(length)
            start = 0
            if read(32, 4) != b"NXSB":
                parts = [p for p in read_gpt(image) if p.type_guid == APFS_PART_TYPE] if image else []
                if len(parts) != 1:
                    raise ValueError(f"{name}: expected one APFS container")
                start = parts[0].offset
            header = read(start, 4096)
            if header[32:36] != b"NXSB":
                raise ValueError(f"{name}: invalid APFS superblock")
            block_size, blocks = struct.unpack_from("<IQ", header, 36)
            if block_size != 4096 or not blocks:
                raise ValueError("unsupported APFS geometry")
            size = blocks * block_size
            available = image.size if image else path.stat().st_size
            if start + size > available:
                raise ValueError("APFS extends past its source image")
            offset = out.tell()
            print(f"{name}: {size} bytes -> offset {offset}", flush=True)
            container_hash = hashlib.sha256()
            for pos in range(0, size, 4 << 20):
                data = read(start + pos, min(4 << 20, size - pos))
                if len(data) != min(4 << 20, size - pos):
                    raise ValueError("short image read")
                out.write(data)
                digest.update(data)
                container_hash.update(data)
            (image.f if image else raw).close()
            metadata["containers"].append({"source": name, "offset": offset, "size": size,
                                            "sha256": container_hash.hexdigest()})
    metadata["image_size"] = args.out.stat().st_size
    metadata["image_sha256"] = digest.hexdigest()
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Prepared", args.out, metadata["image_sha256"], flush=True)


if __name__ == "__main__":
    main()
