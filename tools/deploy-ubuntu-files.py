#!/usr/bin/env python3
"""Stream iOS image files into the existing Ubuntu ext4, preserving its OS files.

Mountpoint is under /data, but all image bytes are stored on mmcblk0p49.
Uses Windows/Linux adb and loopback forwarding; no large staging file in userdata.
The phone remains in Android and the script unmounts the partition on completion.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import socket
import subprocess
import tarfile


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as file:
        for data in iter(lambda: file.read(4 << 20), b""):
            h.update(data)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--workspace", required=True, type=Path)
    ap.add_argument("--backup", required=True, type=Path)
    ap.add_argument("--log", required=True, type=Path)
    args = ap.parse_args()
    backup = json.loads(args.backup.with_suffix(args.backup.suffix + ".json").read_text())
    if not backup["verified"] or backup["partition"] != "Ubuntu" or backup["bytes"] != 33285963776:
        raise SystemExit("unverified/wrong partition backup")
    print("Checking compressed Ubuntu backup", flush=True)
    if digest(args.backup) != backup["sha256_gzip"]:
        raise SystemExit("backup archive hash mismatch")
    workspace = args.workspace
    files = {
        "ios-containers.img": workspace / "out/lancelot-ios-20H392-v1.img",
        "system.apfs": workspace / "userland/system.apfs",
        "root.apfs": workspace / "userland/root.apfs",
        "flashboot.apfs": workspace / "userland/flashboot.apfs",
    }
    source = json.loads(files["ios-containers.img"].with_suffix(".img.json").read_text())
    provenance = {"partition": "Ubuntu", "mountpoint": "/data/local/tmp/hackios-ubuntu",
                  "directory": "/hackios/20H392", "ios_version": source["version"],
                  "ios_build": source["build"], "backup": str(args.backup), "files": {}}
    for name, path in files.items():
        print("Hashing", name, flush=True)
        with path.open("rb") as file:
            file.seek(32)
            if file.read(4) != b"NXSB":
                raise SystemExit("not APFS: " + name)
        provenance["files"][name] = {"size": path.stat().st_size, "sha256": digest(path)}
    if provenance["files"]["ios-containers.img"]["sha256"] != source["image_sha256"]:
        raise SystemExit("original iOS container hash mismatch")
    base = [args.adb, "-s", args.serial]
    def call(*command, timeout=600):
        return subprocess.check_output([*base, *command], stderr=subprocess.STDOUT, timeout=timeout)
    script = Path(__file__).resolve().parent / "android/deploy-ubuntu-files.sh"
    remote_script = "/data/local/tmp/hackios-deploy-files.sh"
    call("push", str(script), remote_script)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    if args.log.exists():
        raise SystemExit("log already exists; not replacing it")
    # Check the pre-deployment source before mounting read/write.
    original = call("exec-out", "su -c 'sha256sum /dev/block/by-name/Ubuntu'").decode().split()[0]
    if original != backup["sha256_raw"]:
        raise SystemExit("Ubuntu changed since verified backup; refusing deployment")
    print(call("exec-out", "su -c 'sh " + remote_script + " inspect'").decode(), flush=True)
    local_port = int(call("forward", "tcp:0", "tcp:5558").strip())
    try:
        with args.log.open("xb") as log:
            process = subprocess.Popen([*base, "exec-out", "su -c 'sh " + remote_script + " deploy yes'"],
                                       stdout=subprocess.PIPE, stderr=log)
            try:
                ready = process.stdout.readline()
                log.write(ready); log.flush()
                if not ready.startswith(b"READY"):
                    raise RuntimeError("Ubuntu mount/receiver did not become ready: " + repr(ready))
                # Socket writer never buffers a whole image in RAM or Android storage.
                with socket.create_connection(("127.0.0.1", local_port), timeout=60) as stream:
                    class Writer:
                        def write(self, data):
                            stream.sendall(data)
                            return len(data)
                    with tarfile.open(fileobj=Writer(), mode="w|") as archive:
                        for name, path in files.items():
                            print("Streaming", name, provenance["files"][name]["size"], "bytes", flush=True)
                            info = tarfile.TarInfo(name)
                            info.size = path.stat().st_size
                            info.mode = 0o644
                            with path.open("rb") as file:
                                archive.addfile(info, file)
                        metadata = json.dumps(provenance, indent=2).encode() + b"\n"
                        sums = "".join(item["sha256"] + "  " + name + "\n" for name, item in provenance["files"].items()).encode()
                        for name, data in (("deployment-source.json", metadata), ("SHA256SUMS", sums)):
                            info = tarfile.TarInfo(name)
                            info.size = len(data); info.mode = 0o644
                            archive.addfile(info, io.BytesIO(data))
                    stream.shutdown(socket.SHUT_WR)
                    # Device stdout is the adb exec-out pipe, not this TCP socket.
                    # Close after the exact tar EOF so remote `nc | tar` exits.
                output = process.communicate(timeout=1200)[0]
                log.write(output); log.flush()
                print(output.decode(errors="replace"), flush=True)
                if process.returncode or b"FILES VERIFIED" not in output or b"Ubuntu partition unmounted" not in output:
                    raise RuntimeError("file deployment or unmount failed; inspect the retained log")
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
    finally:
        call("forward", "--remove", f"tcp:{local_port}")
    verify = call("exec-out", "su -c 'sh " + remote_script + " verify'", timeout=1200)
    with args.log.open("ab") as log:
        log.write(verify)
    if b"READ-ONLY REMOUNT VERIFIED" not in verify or b"Ubuntu partition unmounted" not in verify:
        raise SystemExit("read-only remount verification failed")
    provenance["verified"] = True
    provenance["verification"] = "every file SHA256, sync/unmount, read-only remount SHA256"
    args.log.with_suffix(args.log.suffix + ".json").write_text(json.dumps(provenance, indent=2) + "\n")
    print("All iOS files verified on Ubuntu; partition unmounted; Android remains running", flush=True)


if __name__ == "__main__":
    main()
