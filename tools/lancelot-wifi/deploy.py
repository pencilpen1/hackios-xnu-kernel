#!/usr/bin/env python3
"""Stage/check a native Wi-Fi pair; deployment and boot are explicit phases."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("phase", choices=("preflight", "deploy", "boot"))
ap.add_argument("--candidate", type=Path, required=True)
ap.add_argument("--accepted-pair", type=Path, required=True)
ap.add_argument("--adb", type=Path, required=True)
ap.add_argument("--serial", required=True)
ap.add_argument("--out", type=Path, required=True)
ap.add_argument("--tests-result", type=Path, required=True)
ap.add_argument("--gpu-tests-result", type=Path)
ap.add_argument("--regressions-result", type=Path, help="Explicit regression receipt for non-Wi-Fi rounds")
ap.add_argument("--hfs-result", type=Path, help="HFS verification receipt for this linked kernel")
args = ap.parse_args()
C, O = args.candidate.resolve(), args.out.resolve()
assert O.is_relative_to(C.parent) and O != C
assert re.fullmatch(r"[a-zA-Z0-9_-]+", O.name)
ADB, FB = args.adb.resolve(), args.adb.resolve().with_name("fastboot.exe")
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
read = lambda p: json.loads(Path(p).read_text(encoding="utf-8"))

def host_path(value):
    if value.startswith("/mnt/") and len(value) > 7 and value[6] == "/":
        return Path(value[5].upper() + ":/" + value[7:])
    return Path(value)

candidate = read(C / "result.json")
old = read(args.accepted_pair)
assert candidate["passed"] and candidate["device"] == old["device"] == "lancelot"
for key in ("kernel", "root", "loader", "boot", "mu", "board"):
    item = candidate[key]
    path = host_path(item["path"])
    assert path.stat().st_size == item["bytes"] and sha(path) == item["sha256"], key
for name, digest in candidate["source_inputs"].items():
    assert sha(ROOT / "xnu/files" / name) == digest, name
for name, digest in candidate.get("source_controls", {}).items():
    assert sha(ROOT / name) == digest, name
proof_files = (args.tests_result.resolve(),
               (args.regressions_result or C.parent / "regressions-w1/result.json").resolve(),
               (args.hfs_result or C.parent / "static-hfs-w1/result.json").resolve())
if args.gpu_tests_result:
    proof_files += (args.gpu_tests_result.resolve(),)
    for name, digest in read(args.gpu_tests_result)["sources"].items():
        assert sha(ROOT / name) == digest, ("GPU regression source changed", name)
for proof in proof_files:
    assert read(proof)["passed"], proof
proofs = {str(proof): sha(proof) for proof in proof_files}
scripts = {name: sha(HERE / name) for name in ("deploy.py", "android-state.sh", "install-android.sh")}

def save(name, value):
    with (O / name).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")

def call(log, *command, timeout=120):
    command = list(map(str, command))
    log.write((repr(command) + "\n").encode())
    log.flush()
    result = subprocess.run(command, capture_output=True, timeout=timeout)
    log.write(result.stdout + b"\n[stderr]\n" + result.stderr)
    log.flush()
    text = (result.stdout + result.stderr).decode(errors="replace").strip()
    if result.returncode or (command[0] == str(FB) and "FAILED" in text):
        raise RuntimeError(text)
    return text

def android(log, pair):
    adb = lambda *parts: call(log, ADB, "-s", args.serial, *parts)
    root = lambda command: adb("exec-out", "su -c " + shlex.quote(command))
    assert adb("get-state") == "device" and adb("shell", "getprop", "sys.boot_completed") == "1"
    assert adb("shell", "getprop", "ro.serialno") == args.serial
    assert adb("shell", "getprop", "ro.product.device") == "lancelot" and "uid=0(" in root("id")
    boot_id = adb("shell", "cat", "/proc/sys/kernel/random/boot_id")
    for name, node, size in (("boot", "/dev/block/mmcblk0p33", 67108864), ("esp", "/dev/block/mmcblk0p48", 1073741824),
                             ("Ubuntu", "/dev/block/mmcblk0p49", 33285963776)):
        assert root("readlink -f /dev/block/by-name/" + name) == node
        assert int(root("blockdev --getsize64 /dev/block/by-name/" + name)) == size
    mounts = root("cat /proc/mounts")
    assert not any(line.split()[0] in {"/dev/block/mmcblk0p48", "/dev/block/mmcblk0p49",
                                      "/dev/block/by-name/esp", "/dev/block/by-name/Ubuntu"} for line in mounts.splitlines())
    remote = "/data/local/tmp/hackios-wifi-state-" + O.name + ".sh"
    adb("push", HERE / "android-state.sh", remote)
    state = root("sh " + shlex.quote(remote) + " " + shlex.quote(old["storage"]["root_backup"]))
    assert "READ_ONLY_STATE_COMPLETE" in state
    for key, suffix in (("root", "/root.apfs"), ("loader", "/EFI/HackiOS/loader.efi"),
                        ("loader", "/EFI/BOOT/BOOTAA64.EFI"), ("boot", "/EFI/HackiOS/boot.xnup")):
        assert any(line.split()[:1] == [pair[key]["sha256"]] and line.endswith(suffix) for line in state.splitlines()), suffix
    assert f"ROOT_BYTES {pair['root']['bytes']}" in state
    storage = old["storage"]
    assert any(line.split()[:1] == [storage["root_backup_sha256"]] and line.endswith(storage["root_backup"]) for line in state.splitlines())
    assert any(line.split()[:1] == [storage["map_sha256"]] and line.endswith("/ios-data.map") for line in state.splitlines())
    assert any(line.startswith(str(storage["data_bytes"]) + " ") and line.endswith("/ios-data.hfs") for line in state.splitlines())
    return {"boot_id": boot_id, "state": state, "boot_sha256": root("sha256sum /dev/block/by-name/boot").split()[0]}

if args.phase == "preflight":
    O.mkdir(parents=True, exist_ok=False)
    with (O / "preflight.log").open("xb") as log:
        before = android(log, old)
    free = re.search(r"UBUNTU_FREE (\d+) (\d+)", before["state"])
    assert free
    growth = candidate["root"]["bytes"] - old["root"]["bytes"]
    if growth > 0:
        reserved = re.search(r"EXT4_RESERVED_CLUSTERS (\d+)", before["state"])
        assert reserved and (int(free[1]) - int(reserved[1])) * int(free[2]) >= growth + 2097152
    free = re.search(r"ESP_FREE (\d+) (\d+)", before["state"])
    # Keep this round's ESP backup on Android /data and pull it to the host;
    # retain every earlier ESP backup without consuming another payload's space.
    esp_needed = candidate["loader"]["bytes"] + 2097152
    esp_needed += max(0, candidate["boot"]["bytes"] - old["boot"]["bytes"])
    esp_needed += 2 * max(0, candidate["loader"]["bytes"] - old["loader"]["bytes"])
    assert free and int(free[1]) * int(free[2]) >= esp_needed
    native = O / "native-Android-boot.img"
    with native.open("xb") as stream:
        result = subprocess.run([str(ADB), "-s", args.serial, "exec-out", "su -c 'cat /dev/block/by-name/boot'"],
                                stdout=stream, stderr=subprocess.PIPE, timeout=180)
    assert result.returncode == 0 and native.stat().st_size == 67108864 and sha(native) == before["boot_sha256"]
    save("preflight.json", {"candidate_sha256": sha(C / "result.json"), "accepted_pair_sha256": sha(args.accepted_pair),
                           "scripts": scripts, "proofs": proofs, "Android": before, "read_only_partition_checks": True,
                           "native_boot_backup_sha256": sha(native), "deployed": False})
    print("PREFLIGHT PASSED: exact device/pair, space and data map verified; native Android boot backed up; no system image or firmware writes")
else:
    plan = read(O / "preflight.json")
    assert plan["candidate_sha256"] == sha(C / "result.json") and plan["accepted_pair_sha256"] == sha(args.accepted_pair)
    assert plan["scripts"] == scripts and sha(O / "native-Android-boot.img") == plan["native_boot_backup_sha256"]
    assert plan["proofs"] == proofs
    if args.phase == "deploy":
        with (O / "deploy-check.log").open("xb") as log:
            now = android(log, old)
        assert now["boot_id"] == plan["Android"]["boot_id"] and now["boot_sha256"] == plan["Android"]["boot_sha256"]
        stage = "/data/local/tmp/hackios-" + O.name
        assert re.fullmatch(r"[a-zA-Z0-9_-]+", O.name)
        with (O / "install.log").open("xb") as log:
            call(log, ADB, "-s", args.serial, "shell", "mkdir " + stage)
            for key, name in (("root", "root.apfs"), ("loader", "loader.efi"), ("boot", "boot.xnup")):
                call(log, ADB, "-s", args.serial, "push", host_path(candidate[key]["path"]), stage + "/" + name)
            call(log, ADB, "-s", args.serial, "push", HERE / "install-android.sh", stage + "/install.sh")
            storage = old["storage"]
            parameters = [stage, O.name, *[old[k]["sha256"] for k in ("root", "loader", "boot")],
                          *[candidate[k]["sha256"] for k in ("root", "loader", "boot")], storage["root_backup"],
                          storage["map_sha256"], str(storage["data_bytes"]), storage["root_backup_sha256"],
                          str(old["root"]["bytes"]), str(candidate["root"]["bytes"])]
            command = "sh " + shlex.quote(stage + "/install.sh") + " " + " ".join(map(shlex.quote, parameters))
            response = call(log, ADB, "-s", args.serial, "exec-out", "su -c " + shlex.quote(command), timeout=300)
            assert "COMPLETE_PAIR_READONLY_VERIFIED_OLD_BACKUPS_RETAINED" in response
            call(log, ADB, "-s", args.serial, "pull", stage + "/root.before-pair.apfs", O / "root.before-wifi.apfs")
            assert sha(O / "root.before-wifi.apfs") == old["root"]["sha256"]
            call(log, ADB, "-s", args.serial, "pull", stage + "/esp.before", O / "esp.before")
            for key, rel in (("loader", "EFI/HackiOS/loader.efi"), ("boot", "EFI/HackiOS/boot.xnup"),
                             ("loader", "EFI/BOOT/BOOTAA64.EFI")):
                backup = O / "esp.before" / rel
                assert backup.stat().st_size == old[key]["bytes"] and sha(backup) == old[key]["sha256"]
        with (O / "post-install.log").open("xb") as log:
            post = android(log, candidate)
        save("deployment-receipt.json", {"candidate_sha256": plan["candidate_sha256"], "post": post,
             "root_loader_boot_readonly_verified": True, "data_map_sha256": storage["map_sha256"],
             "data_bytes": storage["data_bytes"], "ESP_backup_on_host_verified": True, "UEFI_flash_performed": False})
        print("ANDROID INSTALL VERIFIED: root, payload and both loaders; original data map and backups retained")
    else:
        receipt = read(O / "deployment-receipt.json")
        assert receipt["root_loader_boot_readonly_verified"] and receipt["candidate_sha256"] == plan["candidate_sha256"]
        with (O / "boot.log").open("xb") as log:
            now = android(log, candidate)
            assert now["boot_sha256"] == plan["native_boot_backup_sha256"]
            call(log, ADB, "-s", args.serial, "reboot", "bootloader")
            deadline = time.monotonic() + 90
            while True:
                devices = call(log, FB, "devices", "-l", timeout=10)
                if any(line.split()[:2] == [args.serial, "fastboot"] for line in devices.splitlines()): break
                if time.monotonic() >= deadline: raise RuntimeError("Native Fastboot did not appear; no flash performed")
                time.sleep(1)
            assert "product: lancelot" in call(log, FB, "-s", args.serial, "getvar", "product")
            assert re.search(r"partition-size:boot:\s*(?:0x)?0*4000000\b", call(log, FB, "-s", args.serial, "getvar", "partition-size:boot"))
            output = call(log, FB, "-s", args.serial, "flash", "boot", host_path(candidate["mu"]["path"]), timeout=300)
            assert re.search(r"Sending[^\r\n]*OKAY", output) and re.search(r"Writing[^\r\n]*OKAY", output)
            save("native-handoff.json", {"Mu_sha256": candidate["mu"]["sha256"], "flash_verified": True,
                                        "reboot_host_monotonic_ns": time.monotonic_ns()})
            call(log, FB, "-s", args.serial, "reboot")
        print("MU/UEFI ACTUALLY FLASHED; device rebooted into the Wi-Fi test pair")
