#!/usr/bin/env python3
"""Stream the 20H392 bootstrap directly onto Ubuntu; verify after a read-only remount."""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import uuid

PROFILES = {
    "service-log-20261023": "8089e522be3d774cb568b853e380421c02ed3defacffd23e4a3b263f20c5ae64",
    "media-prewarm-20261024": "48dde0f5296da5a4fb65a64cd281ad60586875e07e686ae89af4235b99fd125c",
    "sb-startup-20261024": "393a9d99172bc270dc85505b85b3eaf6704b2526b2b9d1730028c4547cbd8c43",
    "sb-activation-20261024": "86fbf42a6cef3610a260420394f9c0dd9eeecb43bc21089bab2b69d984e867a0",
    "touch-hardware-20261024": "81a86d265596387f227c7b8a3e021f991293c2900b69a89a38b0f29be6436763",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--metadata", type=Path, required=True)
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--profile", choices=PROFILES, default="service-log-20261023")
    args = ap.parse_args()
    metadata = json.loads(args.metadata.read_text())
    if metadata["build"] != "20H392" or args.image.stat().st_size != 4194304:
        raise SystemExit("unexpected bootstrap profile")
    files = {"root.apfs": args.image, "profile.json": args.metadata}
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}
    if hashes["root.apfs"] != PROFILES[args.profile]:
        raise SystemExit("profile image does not match the loader's pinned hash")
    if args.profile != "service-log-20261023" and metadata.get("profile") != args.profile:
        raise SystemExit("metadata/profile name mismatch")
    archive_data = io.BytesIO()
    with tarfile.open(fileobj=archive_data, mode="w") as archive:
        for name, path in files.items():
            info = tarfile.TarInfo(name); info.size = path.stat().st_size; info.mode = 0o644
            with path.open("rb") as file: archive.addfile(info, file)
        sums = "".join(value + "  " + name + "\n" for name, value in hashes.items()).encode()
        info = tarfile.TarInfo("SHA256SUMS"); info.size = len(sums); info.mode = 0o644
        archive.addfile(info, io.BytesIO(sums))
    encoded = base64.encodebytes(archive_data.getvalue())
    lines = encoded.count(b"\n")
    base = [args.adb, "-s", args.serial]
    args.log.parent.mkdir(parents=True, exist_ok=True)
    script = Path(__file__).resolve().parent / "android/deploy-lancelot-profile.sh"
    token = uuid.uuid4().hex
    remote = f"/data/local/tmp/hackios-profile-{token}.sh"
    with args.log.open("xb") as log:
        def call(*command, data=None, timeout=120):
            log.write(("COMMAND " + repr(command) + "\n").encode())
            log.flush()
            try:
                result = subprocess.run([*base, *command], stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, input=data, timeout=timeout)
            except subprocess.TimeoutExpired as error:
                log.write(error.stdout or b"")
                log.write(b"\nHOST TIMEOUT; remote mount cleanup will be attempted\n")
                log.flush()
                raise
            log.write(result.stdout); log.flush()
            print(result.stdout.decode(errors="replace"), end="", flush=True)
            result.check_returncode()
            return result.stdout

        def control(action):
            return call("exec-out", f"su -c 'sh {remote} {action} {token}'")

        call("push", str(script), remote)
        try:
            output = call("shell", "-T", f"su -c 'sh {remote} receive {token} {lines} {args.profile}'",
                          data=encoded, timeout=180)
            if b"PROFILE VERIFIED AND UNMOUNTED" not in output:
                raise RuntimeError("profile verification did not complete")
            for name, value in hashes.items():
                if output.count((value + "  " + name).encode()) != 2:
                    raise RuntimeError("missing write/read-only hash evidence: " + name)
        finally:
            control("cleanup")
    report = {"partition": "Ubuntu", "path": "/hackios/20H392/" + args.profile,
              "transport": "bounded base64 over ADB stdin directly to Ubuntu", "verified": True, "files": hashes}
    args.log.with_suffix(args.log.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
