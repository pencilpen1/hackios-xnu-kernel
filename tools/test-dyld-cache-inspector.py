#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("cache_inspector", Path(__file__).with_name("inspect-dyld-cache.py"))
reader = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reader
spec.loader.exec_module(reader)


class CacheTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="dyld-fixture-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "cache"
        self.data = bytearray(4096)
        self.cache = reader.Cache.__new__(reader.Cache)
        self.cache.main = self.path
        self.cache.selector_base = 0x1280
        self.cache.mappings = [reader.Mapping(0x1000, 4096, 0, self.path)]
        self.image = {"path": "/fake.dylib", "address": 0x1000, "size": 4096}
        self.cache.sections = lambda image: iter([("__objc_classlist", 0x1080, 8)])
        for offset, pointer in [(0x80, 0x1100), (0x100, 0x1180), (0x120, 0x1200), (0x1a0, 0x1240),
                                (0x218, 0x1320), (0x258, 0x1320), (0x220, 0x1400), (0x380, 0x1300)]:
            struct.pack_into("<Q", self.data, offset, pointer)
        self.data[0x300:0x305] = b"test\0"
        self.data[0x320:0x328] = b"Fixture\0"
        self.data[0x360:0x367] = b"v16@0:8"  # terminator is already zero
        self.write()

    def write(self):
        self.path.write_bytes(self.data)

    def small(self, direct):
        struct.pack_into("<II", self.data, 0x400, 0x8000000c | (0x40000000 if direct else 0), 1)
        name = 0x1300 - self.cache.selector_base if direct else 0x1380 - 0x1408
        struct.pack_into("<iii", self.data, 0x408, name, 0x1360 - 0x140c, 0x1600 - 0x1410)
        self.write()

    def test_small_direct_selectors_use_global_base(self):
        self.small(True)
        method = list(self.cache.methods(self.image, "Fixture"))[0]
        self.assertEqual((method["selector"], method["types"], method["imp"]), ("test", "v16@0:8", "0x1600"))

    def test_small_indirect_selectors_and_signed_offsets(self):
        self.small(False)
        self.assertEqual(list(self.cache.methods(self.image, "Fixture"))[0]["selector"], "test")

    def test_absolute_methods(self):
        struct.pack_into("<IIQQQ", self.data, 0x400, 24, 1, 0x1300, 0x1360, 0x1600)
        self.write()
        self.assertEqual(list(self.cache.methods(self.image, "Fixture"))[0]["imp"], "0x1600")

    def test_unknown_method_stride_rejected(self):
        struct.pack_into("<II", self.data, 0x400, 0x80000010, 1)
        self.write()
        with self.assertRaises(ValueError):
            list(self.cache.methods(self.image, "Fixture"))

    def test_rebase_delta_not_part_of_pointer(self):
        self.cache.mappings[0].delta_mask = 0xffff000000000000
        self.cache.mappings[0].value_add = 0x1000
        struct.pack_into("<Q", self.data, 0, 0xa123000000000300)
        self.write()
        self.assertEqual(self.cache.pointer(0x1000), 0x1300)
        self.assertEqual(self.cache.pointer(0x1008), 0)

    def test_unmapped_or_cross_boundary_reads_rejected(self):
        for address, size in [(0xfff, 1), (0x1fff, 2), (0x1000, -1)]:
            with self.assertRaises(ValueError):
                self.cache.read_vm(address, size)

    def test_truncated_backing_file_rejected(self):
        self.path.write_bytes(b"\0")
        with self.assertRaises(ValueError):
            self.cache.read_vm(0x1000, 8)

    def test_mismatched_symbols_never_accepted(self):
        symbols = Path(self.directory.name) / "wrong.symbols"
        header = bytearray(512)
        header[:16] = b"dyld_v1   arm64\0"
        header[88:104] = b"wrong-symbol-uid!"
        symbols.write_bytes(header)
        with self.assertRaises(ValueError):
            list(self.cache.local_symbols(symbols, self.image))


if __name__ == "__main__":
    unittest.main()
