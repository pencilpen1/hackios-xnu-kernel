#!/usr/bin/env python3
"""Read-only Android board/driver snapshot; raw output stays outside tracked files."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    commands = {
        "identity.txt": "id; uname -a; getprop ro.product.device; getprop ro.product.model; "
                        "getprop ro.board.platform; getprop ro.hardware; cat /proc/cpuinfo",
        "memory.txt": "cat /proc/iomem; cat /proc/meminfo; cat /proc/interrupts",
        "partitions.txt": "ls -l /dev/block/by-name; cat /proc/partitions; "
                          "for p in /sys/class/block/mmcblk0p48 /sys/class/block/mmcblk0p49; "
                          "do echo $p; cat $p/start $p/size $p/uevent; done",
        "usb.txt": "getprop sys.usb.config; ls -l /sys/class/udc; "
                   "for p in /sys/class/udc/*; do echo $p; "
                   "cat $p/state $p/current_speed $p/maximum_speed $p/uevent; "
                   "readlink -f $p/device/driver; done; "
                   "cat /sys/class/usb_role/*/role /sys/class/typec/*/data_role "
                   "/sys/class/typec/*/power_role /sys/class/extcon/*/state; "
                   "ls -l /config/usb_gadget/g1; "
                   "cat /config/usb_gadget/g1/UDC /config/usb_gadget/g1/idVendor "
                   "/config/usb_gadget/g1/idProduct",
        "drivers.txt": "cat /proc/modules; cat /proc/bus/input/devices; "
                       "for bus in platform i2c spi; do "
                       "for p in /sys/bus/$bus/devices/*; do "
                       "if [ -L $p/driver ]; then echo $p; readlink -f $p/driver; fi; done; done",
        "display.txt": "for p in /sys/class/graphics/fb*; do echo $p; "
                       "cat $p/name $p/virtual_size $p/bits_per_pixel $p/stride; done; "
                       "for p in /sys/class/backlight/*; do echo $p; "
                       "cat $p/brightness $p/max_brightness; done",
        "dmesg.txt": "dmesg",
        "kernel.config.gz": "cat /proc/config.gz",
        "device.dtb": "cat /sys/firmware/fdt",
    }
    manifest = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "files": {}}
    for name, command in commands.items():
        # One quoted remote command, no PTY or PowerShell text conversion for binary files.
        remote = "su -c '" + command.replace("'", "'\\''") + "'"
        result = subprocess.run([args.adb, "-s", args.serial, "exec-out", remote],
                                capture_output=True, timeout=60)
        (args.out / name).write_bytes(result.stdout)
        (args.out / (name + ".stderr")).write_bytes(result.stderr)
        manifest["files"][name] = {
            "command": command, "returncode": result.returncode,
            "bytes": len(result.stdout), "sha256": hashlib.sha256(result.stdout).hexdigest(),
        }
        print(f"{name}: {len(result.stdout)} bytes, exit {result.returncode}")
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if (args.out / "device.dtb").read_bytes()[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit("FDT capture failed: check root authorization and device.dtb.stderr")


if __name__ == "__main__":
    main()
