#!/usr/bin/env python3
"""Stream the new 20H392 boot profile onto Ubuntu and verify it after a read-only remount."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import socket
import subprocess
import tarfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--metadata", type=Path, required=True)
    ap.add_argument("--log", type=Path, required=True)
    args = ap.parse_args()
    metadata = json.loads(args.metadata.read_text())
    if metadata["build"] != "20H392" or args.image.stat().st_size != 4194304:
        raise SystemExit("unexpected bootstrap profile")
    files = {"root.apfs": args.image, "profile.json": args.metadata}
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}
    if hashes["root.apfs"] != "8089e522be3d774cb568b853e380421c02ed3defacffd23e4a3b263f20c5ae64":
        raise SystemExit("profile image does not match the loader's pinned hash")
    base = [args.adb, "-s", args.serial]
    def call(*command, timeout=60):
        return subprocess.check_output([*base, *command], stderr=subprocess.STDOUT, timeout=timeout)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    if args.log.exists(): raise SystemExit("log exists; refusing to replace it")
    script = Path(__file__).resolve().parent / "android/deploy-lancelot-profile.sh"
    remote = "/data/local/tmp/hackios-profile-deploy.sh"
    call("push", str(script), remote)
    port = int(call("forward", "tcp:0", "tcp:5561").strip())
    process = None
    try:
        with args.log.open("xb") as log:
            process = subprocess.Popen([*base, "exec-out", "su -c 'sh " + remote + "'"], stdout=subprocess.PIPE, stderr=log)
            ready = process.stdout.readline()
            log.write(ready); log.flush()
            if not ready.startswith(b"READY"):
                raise RuntimeError("profile receiver not ready: " + repr(ready))
            with socket.create_connection(("127.0.0.1", port), timeout=60) as stream:
                class Writer:
                    def write(self, data):
                        stream.sendall(data)
                        return len(data)
                with tarfile.open(fileobj=Writer(), mode="w|") as archive:
                    for name, path in files.items():
                        info = tarfile.TarInfo(name); info.size = path.stat().st_size; info.mode = 0o644
                        with path.open("rb") as file: archive.addfile(info, file)
                    data = "".join(value + "  " + name + "\n" for name, value in hashes.items()).encode()
                    info = tarfile.TarInfo("SHA256SUMS"); info.size = len(data); info.mode = 0o644
                    archive.addfile(info, io.BytesIO(data))
                stream.shutdown(socket.SHUT_WR)
            output = process.communicate(timeout=300)[0]
            log.write(output); log.flush()
            print(output.decode(errors="replace"))
            if process.returncode or b"PROFILE VERIFIED AND UNMOUNTED" not in output:
                raise RuntimeError("profile deployment/verification failed")
    finally:
        if process is not None and process.poll() is None:
            process.terminate(); process.wait(timeout=15)
        call("forward", "--remove", f"tcp:{port}")
    report = {"partition": "Ubuntu", "path": "/hackios/20H392/service-log-20261023", "verified": True, "files": hashes}
    args.log.with_suffix(args.log.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
