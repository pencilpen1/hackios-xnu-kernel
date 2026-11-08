#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import sys
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

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

    def test_snapshot_command_not_cancelled_at_capture_deadline(self):
        calls = []
        def run(command, **options):
            calls.append(command)
            self.assertEqual(options["timeout"], 10)
            data = b"log-version: KLOG2\n" if "getvar" in command else b"OKAY\n"
            if "get_staged" in command:
                payload = b"test\n"
                Path(command[-1]).write_bytes(struct.pack("<8sQQQII", b"KLOG2\0\0\0", 1, 0, len(payload),
                                                        zlib.crc32(payload), len(payload)) + payload)
            return subprocess.CompletedProcess(command, 0, stdout=data)
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, "argv", ["reader", "--runtime", "--quiet",
                "--once", "--duration", "1", "--out", str(Path(directory) / "log")]), \
                patch.object(reader.subprocess, "run", side_effect=run):
            self.assertEqual(reader.main(), 0)
        self.assertEqual(len(calls), 3)

    def test_runtime_timeout_stops_instead_of_retrying_pending_data(self):
        calls = []
        def run(command, **options):
            calls.append(command)
            if "getvar" in command:
                return subprocess.CompletedProcess(command, 0, stdout=b"log-version: KLOG2\n")
            raise subprocess.TimeoutExpired(command, 10, output=b"partial-reply")
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, "argv", ["reader", "--runtime", "--quiet",
                "--duration", "60", "--out", str(Path(directory) / "log")]), \
                patch.object(reader.subprocess, "run", side_effect=run):
            self.assertEqual(reader.main(), 1)
            self.assertIn(b"partial-reply", (Path(directory) / "log.fastboot").read_bytes())
        self.assertEqual(len(calls), 2)

    def test_live_window_starts_after_initial_backlog_not_at_boot(self):
        clock = [0.0]
        snapshots = []
        pending = [None]
        def run(command, **options):
            if 'getvar' in command:
                return subprocess.CompletedProcess(command, 0, stdout=b'log-version: KLOG2\n')
            if 'klog' in command:
                start = int(command[-1], 16)
                index = len(snapshots)
                payload = [b'A' * 16384, b'caught-up\n', b'live-data\n', b''][index]
                clock[0] = [8.0, 10.0, 13.0, 16.0][index]
                snapshots.append(clock[0])
                pending[0] = struct.pack('<8sQQQII', b'KLOG2\0\0\0', 1, start, start + len(payload), zlib.crc32(payload), len(payload)) + payload
            if 'get_staged' in command:
                Path(command[-1]).write_bytes(pending[0])
            return subprocess.CompletedProcess(command, 0, stdout=b'OKAY\n')
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, 'argv', ['reader', '--runtime', '--quiet',
                '--duration', '5', '--startup-wait', '12', '--live-window', '--out', str(Path(directory) / 'log')]), \
                patch.object(reader.time, 'monotonic', side_effect=lambda: clock[0]), patch.object(reader.time, 'sleep'), \
                patch.object(reader.subprocess, 'run', side_effect=run):
            self.assertEqual(reader.main(), 0)
            log = (Path(directory) / 'log').read_bytes()
            self.assertIn(b'live window started:', log)
            self.assertIn(b'live-data', log)
        self.assertEqual(snapshots, [8.0, 10.0, 13.0, 16.0])

    def test_live_window_stops_if_device_reboots(self):
        clock = [0.0]
        count = [0]
        pending = [None]
        def run(command, **options):
            if 'getvar' in command:
                return subprocess.CompletedProcess(command, 0, stdout=b'log-version: KLOG2\n')
            if 'klog' in command:
                count[0] += 1
                clock[0] = float(count[0])
                payload = b'desktop\n' if count[0] == 1 else b'new-boot-data\n'
                pending[0] = struct.pack('<8sQQQII', b'KLOG2\0\0\0', count[0], 0, len(payload), zlib.crc32(payload), len(payload)) + payload
            if 'get_staged' in command:
                Path(command[-1]).write_bytes(pending[0])
            return subprocess.CompletedProcess(command, 0, stdout=b'OKAY\n')
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, 'argv', ['reader', '--runtime', '--quiet',
                '--duration', '30', '--startup-wait', '10', '--live-window', '--out', str(Path(directory) / 'log')]), \
                patch.object(reader.time, 'monotonic', side_effect=lambda: clock[0]), patch.object(reader.time, 'sleep'), \
                patch.object(reader.subprocess, 'run', side_effect=run):
            self.assertEqual(reader.main(), 1)
            log = (Path(directory) / 'log').read_bytes()
            self.assertIn(b'boot changed during live window', log)
            self.assertNotIn(b'new-boot-data', log)


if __name__ == "__main__":
    unittest.main()
