#!/usr/bin/env python3
"""Host-only tests; never invoke a phone or fastboot."""
import argparse
import io
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("capture", Path(__file__).with_name("capture-lancelot-log.py"))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
MARKER = b"[capture] live window started: boot=8000000000000001 cursor=0000000000000400 duration=180s; initial backlog excluded from window\n"
SAVED = b"\n[capture] saved 1025 bytes overall, 1025 XNU runtime bytes\n"


def args(mode="issue", directory=None):
    return argparse.Namespace(mode=mode, issue="滑动桌面卡顿" if mode == "issue" else None,
                              desktop_confirmed=mode == "issue", fastboot="fastboot-test", serial="test",
                              out_dir=directory)


class CaptureTest(unittest.TestCase):
    def test_fixed_windows_and_no_destructive_commands(self):
        for mode, seconds in [("boot", "240"), ("issue", "180")]:
            command = capture.reader_command(args(mode), Path("evidence"))
            self.assertEqual(command[command.index("--duration") + 1], seconds)
            self.assertEqual("--live-window" in command, mode == "issue")
            self.assertIn("--runtime", command)
            self.assertFalse(set(command) & {"reboot", "flash", "boot", "erase"})

    def test_issue_requires_desktop_and_specific_problem(self):
        for cli in [["issue"], ["issue", "--desktop-confirmed"], ["issue", "--issue", "卡顿"],
                    ["issue", "--desktop-confirmed", "--issue", "  "], ["boot", "--issue", "卡顿"]]:
            with self.subTest(cli=cli), patch.object(sys, "argv", ["capture", *cli]), \
                    patch.object(sys, "stderr", io.StringIO()), self.assertRaises(SystemExit):
                capture.parser()
        with patch.object(sys, "argv", ["capture", "issue", "--desktop-confirmed", "--issue", " 滑动卡顿 "]):
            self.assertEqual(capture.parser().issue, "滑动卡顿")

    def test_lock_never_overwrites_or_removes_another_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            with capture.capture_lock("test", directory):
                path = next(directory.glob("*.lock"))
                original = path.read_bytes()
                with self.assertRaises(RuntimeError), capture.capture_lock("test", directory):
                    pass
                self.assertEqual(path.read_bytes(), original)
            self.assertFalse(list(directory.glob("*.lock")))

    def test_existing_legacy_capture_blocks_until_verified(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            capture.check_legacy(directory)
            (directory / "capture-manifest.json").write_text("{}")
            with self.assertRaises(RuntimeError):
                capture.check_legacy(directory)
            (directory / "capture.exit").write_text("0\n")
            with self.assertRaises(RuntimeError):
                capture.check_legacy(directory)
            (directory / "verify.log").write_text("PASS\n")
            capture.check_legacy(directory)

    def finish(self, data, mode="issue", elapsed=180.2, returncode=0, interrupted=False):
        directory = Path(self.tmp.name)
        (directory / "runtime.log").write_bytes(data)
        return capture.finalize(directory, args(mode), returncode, elapsed, interrupted, True)

    def setUp(self):
        preferred = Path.home() / "AppData/Local/Temp/opencode"
        self.tmp = tempfile.TemporaryDirectory(dir=preferred if preferred.is_dir() else None)

    def tearDown(self):
        self.tmp.cleanup()

    def test_live_slice_excludes_backlog_and_retains_original_bytes(self):
        data = b"boot-frame\n[capture] ring overrun: 20 bytes lost\n" + MARKER + b"issue-frame\n" + SAVED
        result = self.finish(data)
        directory = Path(self.tmp.name)
        self.assertTrue(result["complete"])
        self.assertTrue(result["backlog_ring_overrun"])
        self.assertEqual((directory / "runtime.log").read_bytes(), data)
        self.assertEqual((directory / "issue-window.log").read_bytes(), b"issue-frame\n" + SAVED)
        self.assertEqual(result["window"]["byte_start"], len(data) - len(b"issue-frame\n" + SAVED))
        self.assertEqual(result["files"]["runtime.log"]["sha256"], capture.sha(directory / "runtime.log"))

    def test_boot_duration_is_separate_and_not_live(self):
        result = self.finish(b"firmware\nkernel\n" + SAVED, "boot", 240.5)
        self.assertTrue(result["complete"])
        self.assertEqual(result["window"]["byte_start"], 0)
        self.assertFalse((Path(self.tmp.name) / "issue-window.log").exists())

    def test_short_capture_is_not_success_even_if_reader_returns_zero(self):
        result = self.finish(MARKER + SAVED, elapsed=35)
        self.assertFalse(result["complete"])
        self.assertFalse(result["checks"]["full_host_window"])

    def test_interruption_is_not_success_even_if_reader_returns_zero(self):
        self.assertFalse(self.finish(MARKER + SAVED, interrupted=True)["complete"])

    def test_missing_live_window_never_claims_issue_capture(self):
        result = self.finish(b"backlog\n" + SAVED, elapsed=None)
        self.assertFalse(result["complete"])
        self.assertFalse((Path(self.tmp.name) / "issue-window.log").exists())

    def test_live_overrun_marks_evidence_incomplete(self):
        self.assertFalse(self.finish(MARKER + b"[capture] ring overrun: 20 bytes lost\n" + SAVED)["complete"])

    def test_failed_transport_retains_log_and_failure_receipt(self):
        data = MARKER + b"[capture] KLOG2 command timed out\n" + SAVED
        result = self.finish(data, returncode=1)
        self.assertFalse(result["complete"])
        self.assertEqual((Path(self.tmp.name) / "runtime.log").read_bytes(), data)
        self.assertEqual((Path(self.tmp.name) / "capture.exit").read_text(), "1\n")

    def test_live_restart_is_incomplete(self):
        self.assertFalse(self.finish(MARKER + b"[capture] boot changed during live window\n" + SAVED)["complete"])

    def test_repeated_live_marker_is_rejected(self):
        self.assertFalse(self.finish(MARKER + MARKER + SAVED)["complete"])

    def test_no_runtime_bytes_is_not_a_complete_capture(self):
        self.assertFalse(self.finish(MARKER + b"[capture] saved 20 bytes overall, 0 XNU runtime bytes\n")["complete"])

    def test_capture_result_is_not_overwritten(self):
        original = self.finish(MARKER + SAVED)
        with self.assertRaises(FileExistsError):
            capture.finalize(Path(self.tmp.name), args(), 0, 180.5, False, True)
        self.assertEqual(json.loads((Path(self.tmp.name) / "capture-result.json").read_text(encoding="utf-8")), original)

    def test_wrapper_streams_ready_and_finalizes_without_real_transport(self):
        clock = [0.0]
        class Process:
            returncode = 0
            def __init__(self, command, **options):
                self.stdout = self.lines()
                self.log = Path(command[command.index("--out") + 1])
            def lines(self):
                yield "[capture] starting XNU KLOG2 capture\n"
                clock[0] = 10.0
                yield MARKER.decode()
                clock[0] = 190.5
                self.log.write_bytes(MARKER + b"issue-record\n" + SAVED)
                yield SAVED.decode()
            def wait(self, **options):
                return 0
        directory = Path(self.tmp.name) / "new"
        with patch.object(capture, "check_legacy"), patch.object(capture, "capture_lock") as lock, \
                patch.object(capture.subprocess, "Popen", side_effect=Process), \
                patch.object(capture.time, "monotonic", side_effect=lambda: clock[0]):
            self.assertEqual(capture.capture(args(directory=directory)), 0)
            lock.assert_called_once_with("test")
        self.assertTrue(json.loads((directory / "capture-result.json").read_text(encoding="utf-8"))["complete"])
        self.assertEqual(json.loads((directory / "issue-ready.json").read_text(encoding="utf-8"))["issue"], "滑动桌面卡顿")
        with patch.object(capture, "check_legacy"), patch.object(capture, "capture_lock"), \
                patch.object(capture.subprocess, "Popen") as popen, self.assertRaises(FileExistsError):
            capture.capture(args(directory=directory))
        popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
