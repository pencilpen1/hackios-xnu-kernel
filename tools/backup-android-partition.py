#!/usr/bin/env python3
"""Stream a named Android partition through gzip, then verify decompressed SHA-256.

Raw imaging preserves filesystem metadata, xattrs, capabilities and every sector.
Keep the phone in Android until this command finishes. No device files are written.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--partition", required=True)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.partition):
        ap.error("invalid partition name")
    if args.out.exists() or args.out.with_suffix(args.out.suffix + ".partial").exists():
        ap.error("destination already exists")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    base = [args.adb, "-s", args.serial, "exec-out"]
    device = "/dev/block/by-name/" + args.partition

    def remote(command, timeout=300):
        return subprocess.check_output(base + ["su -c '" + command + "'"], timeout=timeout).decode().strip()

    node = remote("readlink -f " + device)
    size = int(remote("blockdev --getsize64 " + device))
    mounts = remote("cat /proc/mounts")
    for line in mounts.splitlines():
        parts = line.split()
        if parts[0] in (node, device) and "rw" in parts[3].split(","):
            raise SystemExit("partition is mounted writable: " + parts[1])
    print(f"Hashing source {args.partition}: {size} bytes", flush=True)
    before = remote("sha256sum " + device).split()[0]
    partial = args.out.with_suffix(args.out.suffix + ".partial")
    print("Streaming gzip backup to", partial, flush=True)
    with partial.open("xb") as out:
        result = subprocess.run(base + ["su -c 'gzip -1 -c " + device + "'"],
                                stdout=out, stderr=subprocess.PIPE, timeout=3600)
    if result.returncode:
        raise SystemExit("backup failed: " + result.stderr.decode(errors="replace"))
    digest, compressed = hashlib.sha256(), hashlib.sha256()
    count = 0
    print("Verifying gzip and the complete decompressed image", flush=True)
    with gzip.open(partial, "rb") as image:
        while chunk := image.read(4 << 20):
            digest.update(chunk)
            count += len(chunk)
    after = remote("sha256sum " + device).split()[0]
    if count != size or digest.hexdigest() != before or after != before:
        raise SystemExit("source changed or backup differs; partial file retained, not verified")
    with partial.open("rb") as image:
        while chunk := image.read(4 << 20):
            compressed.update(chunk)
    partial.rename(args.out)
    report = {"partition": args.partition, "bytes": size, "sha256_raw": before,
              "sha256_gzip": compressed.hexdigest(), "compressed_bytes": args.out.stat().st_size,
              "verified": True}
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
