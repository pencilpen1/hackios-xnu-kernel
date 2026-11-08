#!/usr/bin/env python3
"""Read-only boot (240s) or desktop/single-issue (180 live seconds) capture."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid


REPO = Path(__file__).resolve().parent.parent
READER = Path(__file__).with_name("read-lancelot-log.py")
DURATIONS = {"boot": 240, "issue": 180}
LIVE_MARKER = re.compile(rb"\[capture\] live window started: boot=([0-9a-f]{16}) cursor=([0-9a-f]{16}) duration=180s;[^\r\n]*\r?\n")
LEGACY = REPO / "local/lancelot-gpu-20261026/private-dma/attempt46/desktop-live-15min-20261030"


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    with path.open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, ensure_ascii=False)
        output.write("\n")


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=DURATIONS)
    ap.add_argument("--issue", help="one specific problem and the operation used to reproduce it")
    ap.add_argument("--desktop-confirmed", action="store_true", help="operator confirms iOS desktop is ready")
    ap.add_argument("--fastboot", default="fastboot")
    ap.add_argument("--serial", default="lancelot-uefi")
    ap.add_argument("--out-dir", type=Path, help="NEW evidence directory; existing paths are never reused")
    ap.add_argument("--io-on-stall",action="store_true",help="request one read-only I/O snapshot if runtime logging stops")
    args = ap.parse_args()
    if args.mode == "issue":
        if not args.desktop_confirmed or not args.issue or not args.issue.strip():
            ap.error("issue requires --desktop-confirmed and a nonempty --issue describing ONE problem/operation")
        args.issue = args.issue.strip()
    elif args.issue is not None or args.desktop_confirmed:
        ap.error("--issue/--desktop-confirmed are only valid in issue mode")
    if not args.serial.strip():
        ap.error("serial must not be empty")
    return args


def reader_command(args, directory):
    command = [sys.executable, "-u", str(READER), "--runtime", "--quiet", "--fastboot", args.fastboot,
               "--serial", args.serial, "--out", str(directory / "runtime.log"),
               "--duration", str(DURATIONS[args.mode])]
    if args.mode == "issue":
        command += ["--live-window", "--startup-wait", "1800"]
    if getattr(args,'io_on_stall',False):command += ['--io-on-stall']
    return command


def check_legacy(directory=LEGACY):
    # The already-running attempt46 wrapper predates our lock. Do not change it
    # or its hashed reader; refuse a second transport until its receipt is closed.
    if (directory / "capture-manifest.json").exists():
        if not (directory / "capture.exit").exists() or not (directory / "verify.log").exists():
            raise RuntimeError("Existing 15-minute capture is not closed/verified; wait for it to finish. "
                               "If it failed, confirm no reader is running before archiving that session.")


@contextmanager
def capture_lock(serial, directory=None):
    directory = directory or REPO / "local/log-capture-locks"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / (hashlib.sha256(serial.encode()).hexdigest()[:16] + ".lock")
    try:
        with path.open("x", encoding="utf-8") as output:
            json.dump({"pid": os.getpid(), "serial": serial, "started_utc": utc_now()}, output)
    except FileExistsError:
        raise RuntimeError(f"A capture owns {path}; do not start two readers on one endpoint.") from None
    try:
        yield
    finally:
        path.unlink()


def finalize(directory, args, returncode, elapsed, interrupted, reader_unchanged):
    log = directory / "runtime.log"
    data = log.read_bytes() if log.exists() else b""
    markers = list(LIVE_MARKER.finditer(data))
    selected = data
    window = {"kind": "reader-start", "seconds": DURATIONS[args.mode], "byte_start": 0}
    if args.mode == "issue" and len(markers) == 1:
        marker = markers[0]
        selected = data[marker.end():]
        window.update(kind="post-live-tail", byte_start=marker.end(),
                      boot=marker[1].decode(), cursor=marker[2].decode())
        # Initial replay is kept in runtime.log, never mixed into issue-window.log.
        with (directory / "issue-window.log").open("xb") as output:
            output.write(selected)
    blockers = [message for message in [b"boot changed during live window", b"KLOG2 command timed out",
                b"live window did not start", b"ring overrun:"] if message in selected]
    checks = {
        "reader_exit_zero": returncode == 0,
        "not_interrupted": not interrupted,
        "reader_unchanged": reader_unchanged,
        "full_host_window": elapsed is not None and elapsed >= DURATIONS[args.mode],
        "runtime_bytes": bool(re.search(rb"\[capture\] saved \d+ bytes overall, [1-9]\d* XNU runtime bytes", data)),
        "live_marker": args.mode != "issue" or len(markers) == 1,
        "no_window_loss_or_restart": not blockers,
    }
    result = {"ended_utc": utc_now(), "mode": args.mode, "issue": args.issue,
              "reader_returncode": returncode, "measured_window_host_seconds": elapsed,
              "window": window, "checks": checks, "complete": all(checks.values()),
              "blockers": [m.decode() for m in blockers],
              "backlog_ring_overrun": b"ring overrun:" in data[:window["byte_start"]],
              "files": {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)}
                        for p in sorted(directory.glob("*.log*"))},
              "scope": "Host capture duration, not device uptime or scanout FPS. No automatic desktop detection. "
                       "Issue data emitted after catch-up may still include delayed userland log records; inspect device timestamps."}
    write_json(directory / "capture-result.json", result)
    with (directory / "capture.exit").open("x") as output:
        output.write("0\n" if result["complete"] else "1\n")
    return result


def capture(args):
    check_legacy()
    with capture_lock(args.serial):
        check_legacy()
        directory = args.out_dir or REPO / "local/log-captures" / (
            datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + args.mode + "-" + uuid.uuid4().hex[:8])
        directory = directory.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        reader_sha = sha(READER)
        command = reader_command(args, directory)
        write_json(directory / "capture-manifest.json", {
            "started_utc": utc_now(), "mode": args.mode, "issue": args.issue,
            "duration_seconds": DURATIONS[args.mode], "desktop_confirmed": args.desktop_confirmed,
            "reboot_performed": False, "deployment_performed": False, "serial": args.serial,
            "reader_sha256": reader_sha, "wrapper_sha256": sha(Path(__file__)), "command": command})
        print(f"Evidence: {directory}", flush=True)
        if args.mode == "boot":
            print("BOOT: capture 240 seconds from reader launch. Run alongside the iOS boot; no reboot/flash is performed.", flush=True)
        else:
            print("ISSUE: catching up to live logs. Wait for READY before reproducing ONLY the named problem.", flush=True)
        process = None
        window_start = time.monotonic() if args.mode == "boot" else None
        returncode, interrupted = 1, False
        with (directory / "capture-console.log").open("x", encoding="utf-8") as console:
            try:
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           text=True, encoding="utf-8", errors="replace",
                                           env={**os.environ, "PYTHONIOENCODING": "utf-8"})
                for line in process.stdout:
                    console.write(line)
                    console.flush()
                    print(line, end="", flush=True)
                    if args.mode == "issue" and window_start is None and LIVE_MARKER.fullmatch(line.strip().encode() + b"\n"):
                        window_start = time.monotonic()
                        write_json(directory / "issue-ready.json", {"utc": utc_now(), "issue": args.issue, "seconds": 180})
                        print(f"READY: reproduce now for 180 seconds: {args.issue}", flush=True)
                returncode = process.wait()
            except KeyboardInterrupt:
                interrupted = True
                console.write("\n[wrapper] interrupted; not a complete capture\n")
            except OSError as error:
                console.write(f"\n[wrapper] failed to start/read: {error}\n")
            finally:
                if process is not None:
                    # Let the current bounded DATA reply finish before any cleanup.
                    try:
                        process.wait(timeout=25)
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        process.wait()
                        interrupted = True
                    process.stdout.close()
        elapsed = time.monotonic() - window_start if window_start is not None else None
        result = finalize(directory, args, returncode, elapsed, interrupted, sha(READER) == reader_sha)
        print("COMPLETE" if result["complete"] else "INCOMPLETE: retain logs and inspect capture-result.json", flush=True)
        return 0 if result["complete"] else 1


def main():
    args = parser()
    try:
        return capture(args)
    except (OSError, RuntimeError) as error:
        print(f"Capture refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
