#!/usr/bin/env python3
"""Host-only validation of pinned profile dispatch and bounded Base64 payloads."""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("profile_deployer", Path(__file__).with_name("deploy-lancelot-profile.py"))
deployer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployer)


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="profile-fixture-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.image = self.root / "root.apfs"
        self.image.write_bytes(b"\0" * 4194304)
        self.metadata = self.root / "profile.json"
        self.metadata.write_text(json.dumps({"build": "20H392", "profile": "media-prewarm-20261024"}))
        self.log = self.root / "deploy.log"
        self.argv = ["deploy", "--adb", "unused-adb", "--serial", "fixture", "--image", str(self.image),
                     "--metadata", str(self.metadata), "--profile", "media-prewarm-20261024", "--log", str(self.log)]

    def test_wrong_image_rejected_before_adb(self):
        with patch.object(sys, "argv", self.argv), patch.object(deployer.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                deployer.main()
            run.assert_not_called()

    def test_wrong_size_rejected_before_adb(self):
        self.image.write_bytes(b"short")
        with patch.object(sys, "argv", self.argv), patch.object(deployer.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                deployer.main()
            run.assert_not_called()

    def test_wrong_name_rejected_before_adb(self):
        self.metadata.write_text(json.dumps({"build": "20H392", "profile": "wrong"}))
        hashes = {"media-prewarm-20261024": hashlib.sha256(self.image.read_bytes()).hexdigest()}
        with patch.object(sys, "argv", self.argv), patch.object(deployer, "PROFILES", hashes), \
                patch.object(deployer.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                deployer.main()
            run.assert_not_called()

    def test_new_profile_payload_and_cleanup(self):
        hashes = {"root.apfs": hashlib.sha256(self.image.read_bytes()).hexdigest(),
                  "profile.json": hashlib.sha256(self.metadata.read_bytes()).hexdigest()}
        commands = []

        def fake_run(command, **options):
            commands.append(command)
            output = b""
            if "receive" in command[-1]:
                self.assertIn("media-prewarm-20261024", command[-1])
                encoded = options["input"]
                line_count = int(command[-1].split()[-2])
                self.assertEqual(encoded.count(b"\n"), line_count)
                with tarfile.open(fileobj=io.BytesIO(base64.decodebytes(encoded))) as archive:
                    self.assertEqual(archive.getnames(), ["root.apfs", "profile.json", "SHA256SUMS"])
                    for name in hashes:
                        self.assertEqual(hashlib.sha256(archive.extractfile(name).read()).hexdigest(), hashes[name])
                output = ("".join(value + "  " + name + "\n" for name, value in hashes.items()) * 2).encode()
                output += b"PROFILE VERIFIED AND UNMOUNTED\n"
            return subprocess.CompletedProcess(command, 0, stdout=output)

        with patch.object(sys, "argv", self.argv), patch.object(deployer, "PROFILES", {"media-prewarm-20261024": hashes["root.apfs"]}), \
                patch.object(deployer.subprocess, "run", side_effect=fake_run):
            deployer.main()
        self.assertEqual(len(commands), 3)
        self.assertIn("cleanup", commands[-1][-1])
        self.assertEqual(json.loads(self.log.with_suffix(".log.json").read_text())["path"],
                         "/hackios/20H392/media-prewarm-20261024")


if __name__ == "__main__":
    unittest.main()
