#!/usr/bin/env python3
"""Host checks of the native WLAN ABI, actual firmware layout and WPA2 code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if os.name == "nt":
    def linux(p):
        return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/") if len(p) > 2 and p[1] == ":" else p.replace("\\", "/")
    raise SystemExit(subprocess.call(["wsl", "-d", "Ubuntu", "--exec", "python3", "-B",
                                    linux(str(Path(__file__).resolve())), *map(linux, sys.argv[1:])]))
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--firmware", type=Path, required=True)
ap.add_argument("--board", type=Path)
ap.add_argument("--out", type=Path, required=True)
args = ap.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1"}
commands = []

def run(command):
    command = list(map(str, command))
    commands.append(command)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    with (out / "tests.log").open("ab") as log:
        log.write((repr(command) + "\n").encode() + result.stdout)
    if result.returncode:
        print(result.stdout.decode(errors="replace"))
        raise SystemExit(result.returncode)
    return result.stdout

firmware = [args.firmware / n for n in ["WIFI_RAM_CODE_soc1_0_1a_1.bin", "soc1_0_ram_mcu_1a_1_hdr.bin",
                                        "soc1_0_ram_bt_1a_1_hdr.bin", "soc1_0_ram_wifi_1a_1_hdr.bin"]]
for p in firmware:
    if not p.is_file():
        raise SystemExit("Missing owner's firmware: " + str(p))
test = (ROOT / "tools/wpa-test/test_wpa.c").read_text()
test = test.replace('"lx_wpa.c"', '"lancelot_wifi_wpa.c"').replace("lx_wpa", "lancelot_wpa").replace("LX_WPA", "LANCELOT_WPA")
(out / "test-native-wpa.c").write_text(test)
for cc in ["clang", "gcc"]:
    flags = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-I" + str(ROOT / "xnu/files/bsd/dev"),
             "-I" + str(ROOT / "xnu/files/bsd"), "-I" + str(out)]
    binary = out / (cc + "-wire")
    run([cc, *flags, ROOT / "tools/lancelot-wifi/test_wire.c", "-o", binary])
    print(run([binary, *firmware]).decode(), end="", flush=True)
    binary = out / (cc + "-wmt")
    run([cc, *flags, ROOT / "tools/lancelot-wifi/test_wmt.c", "-o", binary])
    print(run([binary]).decode(), end="", flush=True)
    binary = out / (cc + "-caps")
    run([cc, *flags, ROOT / "tools/lancelot-wifi/test_caps.c", "-o", binary])
    print(run([binary]).decode(), end="", flush=True)
    binary = out / (cc + "-domain")
    run([cc, *flags, ROOT / "tools/lancelot-wifi/test_domain.c", "-o", binary])
    print(run([binary]).decode(), end="", flush=True)
    binary = out / (cc + "-dmashdl")
    run([cc, *flags, ROOT / "tools/lancelot-wifi/test_dmashdl.c", "-o", binary])
    print(run([binary]).decode(), end="", flush=True)
    binary = out / (cc + "-disconnect")
    run([cc, *flags, ROOT / "tools/lancelot-wifi/test_disconnect.c", "-o", binary])
    print(run([binary]).decode(), end="", flush=True)
    if args.board:
        binary = out / (cc + "-bundle")
        run([cc, *flags, ROOT / "tools/lancelot-wifi/test_bundle.c", "-o", binary])
        print(run([binary, args.board, args.firmware]).decode(), end="", flush=True)
        binary = out / (cc + "-loader")
        run([cc, *flags, "-I" + str(ROOT / "xnuloader"), ROOT / "tools/lancelot-wifi/test_loader.c", "-o", binary])
        print(run([binary, args.board]).decode(), end="", flush=True)
        binary = out / (cc + "-firmware")
        run([cc, *flags, ROOT / "tools/lancelot-wifi/test_firmware.c", "-o", binary])
        print(run([binary, args.board, args.firmware]).decode(), end="", flush=True)
    for seed in [1, 6768]:
        cases = subprocess.check_output([sys.executable, ROOT / "tools/wpa-test/reference.py", str(seed)])
        (out / "cases.inc").write_bytes(cases)
        binary = out / (cc + "-wpa-" + str(seed))
        run([cc, *flags, out / "test-native-wpa.c", "-o", binary])
        print(run([binary]).decode(), end="", flush=True)
record = {"passed": True, "hardware_verified": False, "commands": commands,
          "firmware_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in firmware}}
(out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
