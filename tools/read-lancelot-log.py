#!/usr/bin/env python3
"""Capture MTK firmware LLOG1 and XNU KLOG2 using Windows or Linux fastboot.

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


def log_version(data):
    match = re.search(rb"(?:^|\n)(?:\(bootloader\)\s*)?log-version:\s*(LLOG1|KLOG2)\b", data)
    if not match:
        raise ValueError("device did not advertise a supported log protocol")
    return match[1].decode("ascii")

def parse_io_status(data):
    match=re.search(rb"lancelot-io:\s*([0-9a-fA-F]+(?:,[0-9a-fA-F]+){7,8})(?=\s|$)",data)
    if not match:raise ValueError("missing or malformed read-only I/O response")
    values=[int(x,16) for x in match[1].split(b',')]
    if any(x>0xffffffffffffffff for x in values) or values[0]>1 or values[1]>0x0fffffff or values[2]>4 or values[6]>0xffffffff or values[7]>0xffffffff:
        raise ValueError("read-only I/O response outside schema bounds")
    keys=['consistent','owner_compact_tid','operation','offset','request_bytes','native_bytes','hold_us','raw_command']
    if len(values)==9:
        if values[8]>6:raise ValueError("read-only I/O phase outside schema bounds")
        keys.append('phase')
    return dict(zip(keys,values))


def parse_irq_status(data):
    match=re.search(rb"lancelot-irq:\s*([0-9a-fA-F]+(?:,[0-9a-fA-F]+){9})(?=\s|$)",data)
    if not match:raise ValueError("missing or malformed read-only IRQ response")
    values=[int(x,16) for x in match[1].split(b',')]
    if any(x>0xffffffffffffffff for x in values) or values[0]>1 or values[1]>0x0fffffff or any(values[i]>0xff for i in [2,3,9]) or values[4] not in [*range(8),0xff] or any(values[i]>0xffffffff for i in [5,6]) or values[8]>0x3ff:
        raise ValueError("read-only IRQ response outside schema bounds")
    if values[9]&8 and (values[4]>=8 or not values[2]&(1<<values[4])):
        raise ValueError("read-only IRQ owner/CPU mismatch")
    return dict(zip(['consistent','owner_compact_tid','owner_cpu_mask','irq_cpu_mask','cpu','pending_ast','cpu_signal','interrupted_pc','irq_id','flags'],values))


def parse_sgi_status(data,expected_cpu):
    match=re.search(rb"lancelot-sgi:([0-7]):\s*([0-9a-fA-F]+(?:,[0-9a-fA-F]+){7})(?=\s|$)",data)
    if not match or len(match[2])>60:raise ValueError("missing or malformed read-only SGI response")
    values=[int(x,16) for x in match[2].split(b',')]
    if int(match[1])!=expected_cpu or values[1]!=expected_cpu:
        raise ValueError("read-only SGI CPU mismatch")
    if any(x>0xffffffffffffffff for x in values) or values[0]>1 or values[2]>15 or values[3] not in [*range(10),255] or values[4]>0xffffffff or values[7]>0xffffffff:
        raise ValueError("read-only SGI response outside schema bounds")
    if values[0] and (values[2]>8 or values[3]==255):
        raise ValueError("read-only SGI overflow cannot be consistent")
    if not values[2] and any(values[i] for i in [3,4,5,6,7]):
        raise ValueError("read-only SGI idle frame has callback state")
    return dict(zip(['consistent','cpu','depth','stage','serviced_signal','callback','argument','age_us'],values))


def parse_debug_status(data):
    match=re.search(rb"lancelot-debug:\s*([0-9a-fA-F]+(?:,[0-9a-fA-F]+){5})(?=\s|$)",data)
    if not match or len(match[1])>60:raise ValueError("missing or malformed read-only debugger response")
    values=[int(x,16) for x in match[1].split(b',')]
    if values[0]>1 or values[1] not in [*range(8),255] or values[2]>5 or values[3]>0xffffffff or values[4]>256*1024 or values[5]>127:
        raise ValueError("read-only debugger response outside schema bounds")
    return dict(zip(['consistent','cpu','operation','entries','panic_bytes','flags'],values))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fastboot", default="fastboot")
    ap.add_argument("--serial", default="lancelot-uefi")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--duration", type=float, default=120)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--runtime", action="store_true", help="automatically follow firmware LLOG1 into XNU KLOG2")
    ap.add_argument("--quiet", action="store_true", help="save all bytes without echoing the phone's log to the terminal")
    ap.add_argument("--live-window", action="store_true", help="start duration only after catching up to the live KLOG2 tail; call after desktop readiness is confirmed")
    ap.add_argument("--startup-wait", type=float, default=1800, help="maximum seconds to wait for KLOG2/live-tail catch-up when using --live-window")
    ap.add_argument("--io-on-stall",action="store_true",help="one read-only I/O query after the runtime log stops growing")
    ap.add_argument("--io-stall-after",type=float,default=15,help="idle seconds before that single I/O query")
    ap.add_argument("--sgi-on-stall",action="store_true",help="at most one callback observation per CPU after a confirmed SGI restoration stall")
    ap.add_argument("--debug-on-stall",action="store_true",help="one read-only debugger/log-layout observation after a suspended DebuggerXCall")
    args = ap.parse_args()
    if not 0 < args.duration < float("inf"):
        ap.error("duration must be finite and positive")
    if args.live_window and (not args.runtime or args.once):
        ap.error("live-window requires runtime and cannot be combined with once")
    if not 0 < args.startup_wait < float("inf"):
        ap.error("startup-wait must be finite and positive")
    if args.io_on_stall and (not args.runtime or not 0<args.io_stall_after<float('inf')):
        ap.error("io-on-stall requires runtime and a finite positive idle threshold")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cursor, boot, total = 0, None, 0
    protocol, runtime_total, probe_at, progress_at = None, 0, 0, 0
    last_error, error_at = None, 0
    transport_failed = False
    live_started = False
    io_queried=False;last_growth=time.monotonic();last_progress=None
    deadline = time.monotonic() + (args.startup_wait if args.live_window else args.duration)
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
                    # Duration limits starting another snapshot, not finishing
                    # its DATA reply. Cancelling get_staged at the deadline
                    # leaves payload bytes queued ahead of the next command.
                    try:
                        result = subprocess.run([args.fastboot, "-s", args.serial, *parts],
                                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
                    except subprocess.TimeoutExpired as error:
                        raw.write(error.stdout or b"")
                        raw.flush()
                        raise
                    raw.write(result.stdout)
                    raw.flush()
                    if result.returncode:
                        raise ValueError("fastboot command failed: " + result.stdout.decode(errors="replace").strip())
                    return result.stdout
                now = time.monotonic()
                if args.runtime and now >= probe_at:
                    detected = log_version(command("getvar", "log-version"))
                    probe_at = now + 2
                    if detected != protocol:
                        protocol = detected
                        note("XNU runtime KLOG2 connected" if protocol == "KLOG2" else
                             "firmware LLOG1 connected; waiting for XNU handoff")
                binary = args.runtime and protocol == "KLOG2"
                if binary:
                    snapshot = Path(staging) / "snapshot.bin"
                    snapshot.unlink(missing_ok=True)
                    command("oem", "klog", f"{cursor:016x}")
                    command("get_staged", str(snapshot))
                    batch = parse_klog(snapshot.read_bytes())
                else:
                    batch = parse(command("oem", "log", f"{cursor:016x}"))
                if boot != batch.boot:
                    old = boot
                    if args.live_window and live_started:
                        note("boot changed during live window; stopped rather than mixing a new boot into desktop measurements")
                        transport_failed = True
                        break
                    boot, cursor = batch.boot, 0
                    io_queried=False;last_growth=time.monotonic()
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
                if binary:
                    runtime_total += len(chunk)
                if chunk:last_growth=time.monotonic()
                elif binary and args.io_on_stall and not io_queried and runtime_total and time.monotonic()-last_growth>=args.io_stall_after:
                    io_queried=True
                    try:
                        status=parse_io_status(command('getvar','lancelot-io'))
                        note('I/O snapshot after idle, once: '+' '.join(f'{k}={v}' for k,v in status.items()))
                        if status.get('phase')==4 and status['owner_compact_tid']:
                            try:
                                irq=parse_irq_status(command('getvar','lancelot-irq'))
                                note('IRQ snapshot after stalled restoration, once: '+' '.join(f'{k}={v:#x}' if k in ['interrupted_pc','pending_ast','cpu_signal','irq_id','flags'] else f'{k}={v}' for k,v in irq.items()))
                                if irq['owner_compact_tid']!=status['owner_compact_tid']:
                                    note('MMC owner changed between the I/O and IRQ observations')
                                elif args.sgi_on_stall and status['consistent'] and irq['consistent'] and irq['irq_id']==0 and irq['cpu']<8 and irq['flags']&9==9:
                                    order=[irq['cpu']]+[cpu for cpu in range(8) if cpu!=irq['cpu']]
                                    debug_queried=False
                                    for cpu in order:
                                        try:
                                            sgi=parse_sgi_status(command('getvar',f'lancelot-sgi:{cpu}'),cpu)
                                            note('SGI callback snapshot, once per CPU: '+' '.join(f'{k}={v:#x}' if k in ['serviced_signal','callback','argument'] else f'{k}={v}' for k,v in sgi.items()))
                                            if args.debug_on_stall and sgi['stage']==2 and not debug_queried:
                                                debug_queried=True
                                                try:
                                                    debug=parse_debug_status(command('getvar','lancelot-debug'))
                                                    note('Debugger state after paused callback, once: '+' '.join(f'{k}={v:#x}' if k=='flags' else f'{k}={v}' for k,v in debug.items()))
                                                except ValueError as error:
                                                    note('Debugger snapshot unavailable, not retried: '+str(error))
                                        except ValueError as error:
                                            note('SGI snapshot unavailable; sequence stopped, not retried: '+str(error))
                                            break
                            except ValueError as error:
                                note('IRQ snapshot unavailable, not retried: '+str(error))
                    except ValueError as error:
                        note('I/O snapshot unavailable, not retried: '+str(error))
                last_error = None
                cursor = max(cursor, batch.end)
                if args.runtime and time.monotonic() >= progress_at:
                    progress=(runtime_total,total,cursor)
                    if progress!=last_progress:
                        note(f"received {runtime_total} runtime bytes, {total} bytes overall; cursor={cursor:016x}")
                        last_progress=progress
                    progress_at = time.monotonic() + 15
                if len(batch.data) < (16384 if binary else 4096):
                    if args.live_window and binary and not live_started:
                        live_started = True
                        deadline = time.monotonic() + args.duration
                        note(f"live window started: boot={boot:016x} cursor={cursor:016x} duration={args.duration:g}s; initial backlog excluded from window")
                    if args.once and (not args.runtime or binary):
                        return 0
                    time.sleep(0.2)
            except (ValueError, subprocess.TimeoutExpired) as error:
                if isinstance(error, subprocess.TimeoutExpired) and protocol == "KLOG2":
                    note("KLOG2 command timed out; stopped to avoid consuming an incomplete reply. Reconnect USB before retrying.")
                    transport_failed = True
                    break
                message = "USB unavailable during handoff; retrying" if isinstance(error, subprocess.TimeoutExpired) else str(error)
                if message != last_error or time.monotonic() >= error_at:
                    note(message)
                    last_error, error_at = message, time.monotonic() + 15
                probe_at = 0
                if args.once:
                    return 1
                time.sleep(0.5)
        if args.live_window and not live_started:
            note("live window did not start before startup-wait deadline; no post-desktop measurement claimed")
        note(f"saved {total} bytes overall" + (f", {runtime_total} XNU runtime bytes" if args.runtime else ""))
    return 0 if not transport_failed and (runtime_total if args.runtime else total) and (not args.live_window or live_started) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
