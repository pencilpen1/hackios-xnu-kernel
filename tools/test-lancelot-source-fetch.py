#!/usr/bin/env python3
import hashlib
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("source_fetch", Path(__file__).with_name("fetch-lancelot-touch-source.py"))
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)


class SourceTest(unittest.TestCase):
    def test_git_blob_and_sha256(self):
        data = b"reference\n"
        blob = hashlib.sha1(b"blob 10\0" + data).hexdigest()
        self.assertEqual(fetcher.verify_blob(data, 10, blob), hashlib.sha256(data).hexdigest())

    def test_corruption_or_html_rejected(self):
        blob = hashlib.sha1(b"blob 10\0reference\n").hexdigest()
        for data, size, sha in [(b"reference", 10, blob), (b"<html>404!", 10, blob),
                                (b"reference\n", 10, "wrong"), (b"", fetcher.LIMIT + 1, "a" * 40)]:
            with self.assertRaises(ValueError):
                fetcher.verify_blob(data, size, sha)

    def test_unsafe_paths_rejected(self):
        for path in ("../outside", "/absolute", "x/../outside", "x/.git/config", "C:/file", "x\\file", "x//file", ""):
            with self.assertRaises(ValueError):
                fetcher.safe_path(path)

    def test_safe_reference_path(self):
        self.assertEqual(str(fetcher.safe_path("drivers/spi/spi-mt65xx.c")), "drivers/spi/spi-mt65xx.c")


if __name__ == "__main__":
    unittest.main()
