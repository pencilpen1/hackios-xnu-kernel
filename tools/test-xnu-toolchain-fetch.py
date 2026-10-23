#!/usr/bin/env python3
"""Offline tests: mirror ordering, strict ranges and rejection of altered archives."""
import argparse
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("fetch_xnu", Path(__file__).with_name("fetch-xnu-toolchain.py"))
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)
data = b"\xfd7zXZ\x00test archive bytes"
origin = "https://github.com/example/repo/releases/download/tag/test.tar.xz"
artifact = ("test.tar.xz", origin, len(data), hashlib.sha256(data).hexdigest())
mirror = "https://mirror.example"


class Response(io.BytesIO):
    def __init__(self, payload=data, status=206, content_range=None):
        super().__init__(payload)
        self.status = status
        self.headers = {"Content-Range": content_range or f"bytes 0-{len(data) - 1}/{len(data)}"}


with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    seen = []

    def mirror_ok(request, timeout):
        seen.append(request.full_url)
        return Response()

    with patch.object(fetch.urllib.request, "urlopen", mirror_ok):
        result = fetch.download(directory, artifact, 1, [mirror])
    assert seen == [mirror + "/" + origin]
    assert (directory / artifact[0]).read_bytes() == data
    assert result["reference_hash_verified"] and not result["published_hash_verified"]

for invalid_response in (
    lambda: Response(status=200),
    lambda: Response(content_range="bytes 1-24/24"),
    lambda: Response(data[:-1]),
    lambda: Response(data + b"!"),
):
    with tempfile.TemporaryDirectory() as tmp:
        seen = []

        def wrong_range(request, timeout):
            seen.append(request.full_url)
            return invalid_response() if request.full_url != origin else Response()

        with patch.object(fetch.urllib.request, "urlopen", wrong_range):
            fetch.download(Path(tmp), artifact, 1, [mirror])
        assert seen == [mirror + "/" + origin, origin]

with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    with patch.object(fetch.urllib.request, "urlopen", lambda request, timeout: Response(data[:-1] + b"!")):
        try:
            fetch.download(directory, artifact, 1, [mirror])
        except ValueError as error:
            assert "SHA-256 mismatch" in str(error)
        else:
            raise AssertionError("altered archive was accepted")
    assert not (directory / artifact[0]).exists()
    assert (directory / (artifact[0] + ".partial")).exists()

for value in ("http://mirror.example", "https://user:pass@mirror.example", "https://mirror.example?q=x"):
    try:
        fetch.mirror_prefix(value)
    except argparse.ArgumentTypeError:
        pass
    else:
        raise AssertionError("unsafe mirror prefix accepted: " + value)

assert fetch.mirror_prefix("https://mirror.example/") == mirror
print("Mirror-first fetch tests passed; invalid ranges and reference hash mismatches rejected")
