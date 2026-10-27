#!/usr/bin/env python3
"""Offline tests of passive collection guards and partial-evidence retention."""
from pathlib import Path
import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'sspm_collector', Path(__file__).with_name('collect-lancelot-sspm-android.py'))
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class CollectorTests(unittest.TestCase):
    def run_main(self, out, replies):
        argv = ['collector', '--adb', 'adb-fixture', '--out', str(out)]
        with patch('sys.argv', argv), patch.object(collector.subprocess, 'run', side_effect=replies) as mock:
            result = collector.main()
        return result, mock

    def test_wrong_device_no_root(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            result, mock = self.run_main(out, [subprocess.CompletedProcess([], 0, b'other\n', b'')])
            self.assertEqual(result, 1)
            self.assertEqual(mock.call_count, 1)
            self.assertNotIn('su', mock.call_args.args[0])
            manifest = json.loads((out / 'manifest.json').read_text())
            self.assertFalse(manifest['complete'])
            self.assertIn('Not confirmed', manifest['error'])

    def test_missing_root_no_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            result, mock = self.run_main(Path(directory), [
                subprocess.CompletedProcess([], 0, b'lancelot\n', b''),
                subprocess.CompletedProcess([], 0, b'uid=2000(shell)\n', b''),
            ])
            self.assertEqual(result, 1)
            self.assertEqual(mock.call_count, 2)

    def test_timeout_retains_partial_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = collector.Capture('adb-fixture', 'fixture', Path(directory))
            error = subprocess.TimeoutExpired('fixture', 40, output=b'partial', stderr=b'diagnostic')
            with patch.object(collector.subprocess, 'run', side_effect=error):
                status, stdout = capture.run('data.txt', 'cat /proc/iomem')
            self.assertIsNone(status)
            self.assertEqual(stdout, b'partial')
            manifest = json.loads((Path(directory) / 'manifest.json').read_text())
            record = manifest['records'][0]
            self.assertTrue(record['timed_out'])
            self.assertEqual(record['sha256'], hashlib.sha256(b'partial').hexdigest())
            self.assertEqual(record['stderr_sha256'], hashlib.sha256(b'diagnostic').hexdigest())
            self.assertFalse(manifest['complete'])

    def test_existing_evidence_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / 'evidence').write_bytes(b'keep')
            with self.assertRaises(SystemExit):
                self.run_main(out, [])
            self.assertEqual((out / 'evidence').read_bytes(), b'keep')

    def test_success_is_passive_and_hashed(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            replies = [subprocess.CompletedProcess([], 0, b'lancelot\n', b''),
                       subprocess.CompletedProcess([], 0, b'uid=0(root)\n', b'')]
            replies += [subprocess.CompletedProcess([], 0, b'data', b'') for _ in collector.COMMANDS]
            result, mock = self.run_main(out, replies)
            self.assertEqual(result, 0)
            self.assertEqual(mock.call_count, len(collector.COMMANDS) + 2)
            manifest = json.loads((out / 'manifest.json').read_text())
            self.assertTrue(manifest['complete'])
            for key in ['phone_writes', 'raw_mmio', 'ipi_requests', 'read_sspm_alive',
                        'running_firmware_identity_proven', 'xnu_mailbox_handoff_proven']:
                self.assertFalse(manifest[key])
            for record in manifest['records']:
                self.assertEqual(hashlib.sha256((out / record['file']).read_bytes()).hexdigest(), record['sha256'])
            # Review the exact allowlist; future new commands require explicit review.
            commands = dict(collector.COMMANDS)
            self.assertEqual(len(commands), 12)
            for command in commands.values():
                for unsafe in ['devmem', 'sspm_alive', 'echo ', 'dd ', 'reboot', 'flash', 'mount ', '>']:
                    self.assertNotIn(unsafe, command)
            self.assertEqual(commands['sspm-attribute-names.txt'], 'find /sys -maxdepth 5 -iname "*sspm*"')


if __name__ == '__main__':
    unittest.main()
