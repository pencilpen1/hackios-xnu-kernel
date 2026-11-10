#!/usr/bin/env python3
"""Prepare private lancelot WLAN inputs from an authenticated Android capture.

This does not access, deploy to, or reboot the phone. Output belongs in local/.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import secrets
import shutil
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
FILES = ["soc1_0_ram_mcu_1a_1_hdr.bin", "soc1_0_ram_bt_1a_1_hdr.bin", "soc1_0_ram_wifi_1a_1_hdr.bin",
         "soc1_0_patch_mcu_1a_1_hdr.bin", "WIFI_RAM_CODE_soc1_0_1a_1.bin", "WIFI"]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--capture", type=Path, required=True)
    ap.add_argument("--firmware", type=Path, required=True)
    ap.add_argument("--identity", type=Path, required=True, help="Persistent private address record in local/")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    evidence = json.loads((args.capture / "manifest.json").read_text())
    if evidence["device"] != "lancelot" or not evidence["read_only"]:
        raise SystemExit("Expected the rooted lancelot read-only capture")
    inventory = json.loads((args.firmware / "manifest.json").read_text())
    if inventory["device"] != "lancelot":
        raise SystemExit("Wrong firmware device")
    pins = {entry["name"]: entry for entry in inventory["files"]}
    data = {}
    for name in [*FILES, "WMT_SOC.cfg"]:
        blob = (args.firmware / name).read_bytes()
        if name not in pins or len(blob) != pins[name]["size"] or hashlib.sha256(blob).hexdigest() != pins[name]["sha256"]:
            raise SystemExit("Captured file changed: " + name)
        data[name] = blob
    nvram = data["WIFI"]
    if len(nvram) != 2050 or struct.unpack_from("<HH", nvram) != (1, 9) or nvram[-2:] != b"\xaa\xac":
        raise SystemExit("Unexpected lancelot NVRAM layout")
    settings = dict(re.findall(r"^\s*([a-zA-Z0-9_]+)\s*=\s*([0-9]+)\s*$", data["WMT_SOC.cfg"].decode(), re.M))
    wanted = {"bt_tssi_target": "3719", "bt_tssi_from_wifi": "3", "coex_wmt_ant_mode": "1",
              "co_clock_flag": "1", "wifi_ant_swap_mode": "0", "wifi_main_ant_polarity": "0"}
    if any(settings.get(k) != v for k, v in wanted.items()):
        raise SystemExit("WMT board configuration requires review")
    match = re.search(r"Wifi Country Code\s*=\s*([A-Z]{2})", (args.capture / "wlan-status.txt").read_text())
    country = match.group(1) if match and match.group(1) == "CN" else "00"
    if args.identity.exists():
        identity = json.loads(args.identity.read_text())
        if identity["device"] != "lancelot":
            raise SystemExit("Wrong private identity")
        mac = bytes.fromhex(identity["mac_hex"])
    else:
        mac = nvram[4:10]
        origin = "factory NVRAM"
        if not any(mac) or mac[0] & 1:
            mac = bytearray(secrets.token_bytes(6))
            mac[0] = (mac[0] | 2) & 0xfe
            mac = bytes(mac)
            origin = "persistent locally administered address; factory NVRAM address is absent"
        args.identity.parent.mkdir(parents=True, exist_ok=True)
        args.identity.write_text(json.dumps({"device": "lancelot", "mac_hex": mac.hex(), "source": origin}, indent=2) + "\n")
    if len(mac) != 6 or not any(mac) or mac[0] & 1:
        raise SystemExit("Invalid private interface address")
    prefix = struct.pack("<II6s2sH6BI", 0x3142574c, 1, mac, country.encode(), 3719, 3, 1, 1, 0, 0, 0, 2048)
    record = prefix + b"".join(hashlib.sha256(data[name]).digest() for name in FILES)
    record += struct.pack("<I", zlib.crc32(record))
    if len(record) != 224:
        raise AssertionError("Board ABI size changed")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    firmware_out = out / "fw/connsys"
    firmware_out.mkdir(parents=True)
    for name in FILES:
        (firmware_out / name).write_bytes(data[name])
    (out / "wifi-board.bin").write_bytes(record)
    profile = json.loads((ROOT / "tools/lancelot/accepted-profile.json").read_text())
    words = [word for word in profile["cmdline"].split()
             if not word.startswith(("wifi=", "connsys_step=", "glslow=")) and word != "glenv=PAN_MESA_DEBUG=perf"]
    words.append("wifi=1")
    profile["cmdline"] = " ".join(words)
    profile["wifi_test"] = {"revision": "W1", "native_device": "MT6768 CONNAC AXI / MT6631",
                            "hardware_verified": False, "deployed": False, "country": country,
                            "reference_commit": "620c77507a17ad23ac5a40bad900abe9ed4d7740"}
    spec = importlib.util.spec_from_file_location("profile", ROOT / "tools/lancelot-profile.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cmdline = module.render(profile, {})
    (out / "wifi-profile.json").write_text(json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    manifest = {"device": "lancelot", "private": True, "hardware_verified": False,
                "board_bytes": len(record), "board_sha256": hashlib.sha256(record).hexdigest(),
                "country": country, "files": {name: pins[name]["sha256"] for name in FILES},
                "effective_cmdline_bytes": len((module.PREFIX + cmdline).encode()), "deployed": False}
    (out / "result.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"private_bundle": str(out), "firmware_files": len(FILES),
                      "board_bytes": len(record), "country": country,
                      "effective_cmdline_bytes": manifest["effective_cmdline_bytes"], "deployed": False}, indent=2))

if __name__ == "__main__":
    main()
