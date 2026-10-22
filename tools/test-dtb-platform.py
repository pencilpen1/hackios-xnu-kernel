#!/usr/bin/env python3
"""Regression: MTK cpu@ unit names differ from their actual MPIDR reg values."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("dtb_platform", Path(__file__).resolve().parents[1] / "common/dtb-platform.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PlatformTest(unittest.TestCase):
    def extract(self, names, regs, capacities, method="smc"):
        cpus = "\n".join(f'cpu@{name:x} {{ device_type="cpu"; reg=<{reg}>; capacity-dmips-mhz=<{cap}>; }};'
                         for name, reg, cap in zip(names, regs, capacities))
        dts = f'''/dts-v1/;
/ {{ #address-cells=<2>; #size-cells=<2>;
cpus {{ #address-cells=<1>; #size-cells=<0>; {cpus} }};
intc {{ compatible="vendor,gic", "arm,gic-v3";
reg=<0 0x0c000000 0 0x40000 0 0x0c040000 0 0x200000>; }};
psci {{ compatible="arm,psci-1.0"; method="{method}"; }};
}};'''
        with tempfile.TemporaryDirectory() as directory:
            dtb = Path(directory) / "test.dtb"
            subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(dtb)], input=dts, text=True, check=True)
            return module.inspect(str(dtb))

    def test_mtk_reg_not_unit_name(self):
        self.assertEqual(self.extract([0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103],
                                     [i << 8 for i in range(8)], [495] * 6 + [1024] * 2),
                         (0xc000000, 0xc040000, 8, 8, 0x3f, 0))

    def test_contiguous_aff0_hvc(self):
        self.assertEqual(self.extract(range(4), range(4), [100] * 4, "hvc"),
                         (0xc000000, 0xc040000, 4, 0, 0, 1))

    def test_unrepresentable_topology(self):
        with self.assertRaisesRegex(ValueError, "explicit topology"):
            self.extract([0, 1, 2], [0, 0x100, 0x20000], [100] * 3)


if __name__ == "__main__":
    unittest.main()
