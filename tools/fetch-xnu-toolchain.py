#!/usr/bin/env python3
"""Fetch pinned XNU build dependencies, also usable from Windows before WSL builds.

Only host tools and SDKs; this does not download an iOS firmware or access a phone.
The output directory must be new. Parallel range downloads validate every byte range.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import time
import urllib.request

ARTIFACTS = (
    ("ios-toolchain.tar.xz", "https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-x86_64.tar.xz", 86163280, None),
    ("ios-sdk.tar.xz", "https://github.com/theos/sdks/releases/download/master-146e41f/iPhoneOS16.5.sdk.tar.xz", 17446920, None),
    ("macos-sdk.tar.xz", "https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz", 59089188,
     "518e35eae6039b3f64e8025f4525c1c43786cc5cf39459d609852faf091e34be"),
)


def download(directory, artifact, workers):
    name, url, size, expected = artifact
    ranges = [(start, min(size - 1, start + (2 << 20) - 1)) for start in range(0, size, 2 << 20)]

    def chunk(bounds):
        start, end = bounds
        for attempt in range(4):
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "hackios-xnu-build",
                    "Range": f"bytes={start}-{end}"})
                with urllib.request.urlopen(req, timeout=60) as response:
                    if response.status != 206 or response.headers.get("Content-Range") != f"bytes {start}-{end}/{size}":
                        raise ValueError("server did not honor the requested range")
                    data = response.read(end - start + 2)
                if len(data) != end - start + 1:
                    raise ValueError("truncated or oversized range")
                return start, data
            except Exception:
                if attempt == 3:
                    raise
                time.sleep(attempt + 1)

    path = directory / (name + ".partial")
    with path.open("xb") as file, ThreadPoolExecutor(max_workers=workers) as pool:
        file.truncate(size)
        for start, data in pool.map(chunk, ranges):
            file.seek(start)
            file.write(data)
    digest = hashlib.sha256()
    with path.open("rb") as file:
        if file.read(6) != b"\xfd7zXZ\x00":
            raise ValueError("not an XZ archive: " + name)
        file.seek(0)
        for data in iter(lambda: file.read(4 << 20), b""):
            digest.update(data)
    actual = digest.hexdigest()
    if expected and actual != expected:
        raise ValueError("published SHA-256 mismatch: " + name)
    path.rename(directory / name)
    print(name, size, actual, flush=True)
    return {"file": name, "url": url, "bytes": size, "sha256": actual,
            "published_hash_verified": bool(expected)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--workers", type=int, default=4)
    args = ap.parse_args()
    if not 1 <= args.workers <= 8:
        ap.error("workers must be 1..8")
    args.out.mkdir(parents=True, exist_ok=False)
    # Each artifact has independent range workers; finish all before reporting success.
    with ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(lambda artifact: download(args.out, artifact, args.workers), ARTIFACTS))
    (args.out / "manifest.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
