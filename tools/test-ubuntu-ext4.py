#!/usr/bin/env python3
"""Read real mkfs.ext4 fixtures with the loader's C code under ASan/UBSan."""
from pathlib import Path
import hashlib
import os
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    reader = tmp / "reader"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", str(root / "tools/test-ubuntu-ext4.c"),
                    "-o", str(reader)], check=True)
    source = tmp / "tree"
    dest = source / "hackios/20H392"
    dest.mkdir(parents=True)
    data = bytes(range(256)) * 16384
    (dest / "root.apfs").write_bytes(data)
    (dest / "empty").touch()
    os.symlink("root.apfs", dest / "link")
    sparse = dest / "sparse"
    with sparse.open("wb") as file:
        for i in range(64):
            file.seek(i * 65536)
            file.write(bytes([i + 1]) * 4096)
    sparse_data = sparse.read_bytes()
    for size in (0, 1, 55, 56, 63, 64, 65, 127, 128, 4097, len(data)):
        test = tmp / "hash-input"
        test.write_bytes(data[:size])
        actual = subprocess.check_output([str(reader), "--sha", str(test)], text=True).strip()
        assert actual == hashlib.sha256(data[:size]).hexdigest(), size
    print("SHA256 boundary vectors and 4 MiB image match hashlib", flush=True)

    def run(image, path, *args, good=True):
        result = subprocess.run([str(reader), str(image), path, *map(str, args)], capture_output=True)
        assert result.returncode == (0 if good else 1), (path, result.returncode, result.stderr)
        assert not result.stderr, result.stderr
        return result.stdout

    for block_size, legacy in ((1024, False), (2048, False), (4096, False), (4096, True)):
        image = tmp / f"ext4-{block_size}-{legacy}.img"
        with image.open("wb") as file:
            file.truncate(64 << 20)
        features = ["-O", "^64bit,^metadata_csum"] if legacy else []
        subprocess.run(["mkfs.ext4", "-q", "-F", "-b", str(block_size), *features, "-d", str(source), str(image)], check=True)
        assert run(image, "/hackios/20H392/root.apfs") == data
        assert run(image, "/hackios/20H392/sparse") == sparse_data
        assert run(image, "/hackios/20H392/empty") == b""
        for start, count in ((1, 55), (1023, 4099), (len(data) - 19, 19)):
            assert run(image, "/hackios/20H392/root.apfs", start, count) == data[start:start + count]
        run(image, "/hackios/20H392/root.apfs", len(data), 1, good=False)
        for path in ("/hackios/missing", "/hackios/20H392/link", "/hackios/../hackios/20H392/root.apfs"):
            run(image, path, good=False)
        # Reject a dirty/recovery-required volume and unsupported allocation mode.
        for offset, flag in ((1024 + 96, 4), (1024 + 100, 0x200)):
            with image.open("r+b") as file:
                file.seek(offset); original = file.read(4)
                file.seek(offset); file.write(struct.pack("<I", struct.unpack("<I", original)[0] | flag))
            run(image, "/hackios/20H392/root.apfs", good=False)
            with image.open("r+b") as file:
                file.seek(offset); file.write(original)
        print(f"ext4 {block_size} legacy={legacy}: contents, sparse extents, partial reads and rejection checks passed", flush=True)
    # Corrupt external extent counts/depth must stop without out-of-range reads.
    stat = subprocess.check_output(["debugfs", "-R", "stat /hackios/20H392/sparse", str(image)], stderr=subprocess.DEVNULL, text=True)
    match = re.search(r"\(ETB0\):(\d+)", stat)
    assert match, "fixture must exercise external extent nodes"
    extent_node = int(match[1]) * block_size
    for field, value in ((2, 65535), (6, 1)):
        with image.open("r+b") as file:
            file.seek(extent_node + field); original = file.read(2)
            file.seek(extent_node + field); file.write(struct.pack("<H", value))
        run(image, "/hackios/20H392/sparse", good=False)
        with image.open("r+b") as file:
            file.seek(extent_node + field); file.write(original)
    print("External extent tree exercised; all reads remained inside the partition", flush=True)
