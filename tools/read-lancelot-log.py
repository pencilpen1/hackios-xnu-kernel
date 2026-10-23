#!/usr/bin/env python3
"""Capture the existing MTK UEFI LLOG1 protocol using Windows or Linux fastboot.

This is fastboot INFO transport, not CDC ACM. No libusb/WSL USB forwarding is
needed when --fastboot points to the Windows platform-tools executable.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import struct
import time
import tempfile
import zlib


@dataclass
class Batch:
    boot: int
    start: int
    end: int
    data: bytes


def parse(data):
    header, payload = None, bytearray()
    for line in data.decode("utf-8", errors="replace").splitlines():
        line = re.sub(r"^\s*(?:\(bootloader\)\s*|INFO\s*)?", "", line).strip()
        if line.startswith("LLOG1"):
            match = re.fullmatch(r"LLOG1 ([\da-fA-F]{16}) ([\da-fA-F]{16}) ([\da-fA-F]{16})", line)
            if not match or header is not None:
                raise ValueError("invalid/duplicate log header")
            header = tuple(int(v, 16) for v in match.groups())
            if not 0 <= header[2] - header[1] <= 4096:
                raise ValueError("invalid log range")
        elif line.startswith("LLOGD"):
            match = re.fullmatch(r"LLOGD ((?:[\da-fA-F]{2}){1,24})", line)
            if not match or header is None:
                raise ValueError("invalid data packet")
            payload.extend(bytes.fromhex(match[1]))
    if header is None or len(payload) != header[2] - header[1]:
        raise ValueError("missing or incomplete snapshot")
    return Batch(*header, bytes(payload))


def parse_klog(data):
    if len(data) < 40:
        raise ValueError("truncated KLOG2 header")
    magic, boot, start, end, checksum, size = struct.unpack_from("<8sQQQII", data)
    payload = data[40:]
    if magic != b"KLOG2\0\0\0" or not 0 <= end - start <= 16384 or size != end - start or len(payload) != size:
        raise ValueError("invalid KLOG2 range/length")
    if zlib.crc32(payload) != checksum:
        raise ValueError("KLOG2 checksum mismatch")
    return Batch(boot, start, end, payload)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fastboot", default="fastboot")
    ap.add_argument("--serial", default="lancelot-uefi")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--duration", type=float, default=120)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--runtime", action="store_true", help="capture XNU KLOG2 via fastboot upload; keep retrying through handoff")
    ap.add_argument("--quiet", action="store_true", help="save all bytes without echoing the phone's log to the terminal")
    args = ap.parse_args()
    if not 0 < args.duration < float("inf"):
        ap.error("duration must be finite and positive")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cursor, boot, total = 0, None, 0
    deadline = time.monotonic() + args.duration
    with tempfile.TemporaryDirectory(prefix="klog-", dir=args.out.parent) as staging, \
            args.out.open("ab") as output, args.out.with_suffix(args.out.suffix + ".fastboot").open("ab") as raw:
        def note(text):
            line = f"\n[capture] {text}\n"
            print(line.strip(), flush=True)
            output.write(line.encode())
            output.flush()
        note("starting " + ("XNU KLOG2" if args.runtime else "LLOG1") + " capture")
        while time.monotonic() < deadline:
            try:
                def command(*parts):
                    result = subprocess.run([args.fastboot, "-s", args.serial, *parts],
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            timeout=min(10, max(0.1, deadline - time.monotonic())))
                    raw.write(result.stdout)
                    raw.flush()
                    if result.returncode:
                        raise ValueError("fastboot command failed: " + result.stdout.decode(errors="replace").strip())
                    return result.stdout
                if args.runtime:
                    snapshot = Path(staging) / "snapshot.bin"
                    snapshot.unlink(missing_ok=True)
                    command("oem", "klog", f"{cursor:016x}")
                    command("get_staged", str(snapshot))
                    batch = parse_klog(snapshot.read_bytes())
                else:
                    batch = parse(command("oem", "log", f"{cursor:016x}"))
                if boot != batch.boot:
                    old = boot
                    boot, cursor = batch.boot, 0
                    note(f"boot {boot:016x}")
                    if old is not None:
                        continue
                if batch.start > cursor:
                    note(f"ring overrun: {batch.start - cursor} bytes lost")
                chunk = batch.data[max(0, cursor - batch.start):]
                output.write(chunk)
                output.flush()
                if chunk and not args.quiet:
                    print(chunk.decode("utf-8", errors="replace"), end="", flush=True)
                total += len(chunk)
                cursor = max(cursor, batch.end)
                if len(batch.data) < (16384 if args.runtime else 4096):
                    if args.once:
                        return 0
                    time.sleep(0.2)
            except (ValueError, subprocess.TimeoutExpired) as error:
                note(str(error))
                if args.once:
                    return 1
                time.sleep(0.5)
        note(f"saved {total} {'kernel/userland' if args.runtime else 'firmware'} bytes")
    return 0 if total else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
