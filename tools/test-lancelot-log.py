#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("reader", Path(__file__).with_name("read-lancelot-log.py"))
reader = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reader
spec.loader.exec_module(reader)


class LogTest(unittest.TestCase):
    HEADER = b"(bootloader) LLOG1 0000000000000001 000000000000000a 000000000000000d\r\n"

    def test_windows_line_endings(self):
        b = reader.parse(self.HEADER + b"(bootloader) LLOGD 41000a\r\nOKAY [0.1s]\r\n")
        self.assertEqual((b.boot, b.start, b.end, b.data), (1, 10, 13, b"A\0\n"))

    def test_truncated_data_not_accepted(self):
        with self.assertRaises(ValueError):
            reader.parse(self.HEADER + b"(bootloader) LLOGD 41\n")

    def test_extra_data_not_accepted(self):
        with self.assertRaises(ValueError):
            reader.parse(self.HEADER + b"(bootloader) LLOGD 41424344\n")

    def test_plain_fastboot_output_is_not_success(self):
        with self.assertRaises(ValueError):
            reader.parse(b"OKAY\nFinished. Total time: 0.01s\n")


if __name__ == "__main__":
    unittest.main()
