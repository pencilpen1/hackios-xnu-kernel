#!/usr/bin/env python3
"""Deploy EFI/XNUP files through Android root, checking every phone-side SHA-256.

Does not flash boot or reboot. Push files individually: adb push DIRECTORY to
an existing destination can create a nested directory and leave old files active.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--files", type=Path, default=Path("out/lancelot/EFI/HackiOS"))
    args = ap.parse_args()
    base = [args.adb, "-s", args.serial]
    root = Path(__file__).resolve().parents[1]
    stage = "/data/local/tmp/hackios-stage"

    def call(*command):
        return subprocess.run([*base, *command], capture_output=True, timeout=120, check=True)

    call("shell", "mkdir -p " + stage)
    hashes = {}
    for name in ("loader.efi", "boot.xnup"):
        path = args.files / name
        hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        call("push", str(path), stage + "/" + name)
        result = call("exec-out", "su -c 'sha256sum " + stage + "/" + name + "'")
        actual = result.stdout.decode().split()[0]
        if actual != hashes[name]:
            raise SystemExit("staged hash mismatch: " + name)
    sums = args.files / "SHA256SUMS"
    sums.write_text("".join(h + "  " + name + "\n" for name, h in hashes.items()))
    call("push", str(sums), stage + "/SHA256SUMS")
    call("push", str(root / "tools/android/deploy-lancelot-esp.sh"), "/data/local/tmp/hackios-deploy.sh")
    result = call("exec-out", "su -c 'sh /data/local/tmp/hackios-deploy.sh " + stage + "'")
    print(result.stdout.decode(errors="replace"), end="")
    if "ESP deployment verified" not in result.stdout.decode():
        raise SystemExit("deployment script did not complete")
    for expected in hashes.values():
        if expected not in result.stdout.decode():
            raise SystemExit("ESP readback hash missing")


if __name__ == "__main__":
    main()
