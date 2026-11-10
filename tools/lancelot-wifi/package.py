#!/usr/bin/env python3
"""Package a native lancelot WLAN test pair without accessing the phone."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--accepted-repository", type=Path, required=True)
ap.add_argument("--baseline", type=Path, required=True)
ap.add_argument("--workspace", type=Path, required=True)
ap.add_argument("--kernel-result", type=Path, required=True)
ap.add_argument("--mu-result", type=Path, required=True)
ap.add_argument("--private-bundle", type=Path, required=True)
ap.add_argument("--root-tree", type=Path, required=True)
ap.add_argument("--out", type=Path, required=True)
ap.add_argument("--round", default="native Wi-Fi")
args = ap.parse_args()
if os.name == "nt":
    raise SystemExit("Run package.py in WSL with Linux paths")
sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
read = lambda path: json.loads(Path(path).read_text())
baseline, kernel, mu = read(args.baseline), read(args.kernel_result), read(args.mu_result)
private = read(args.private_bundle / "result.json")
assert baseline["device"] == "lancelot" and kernel["passed"] and mu["passed"] and private["device"] == "lancelot"
for name, expected in kernel["inputs"].items():
    assert sha(ROOT / "xnu/files" / name) == expected, ("Kernel source changed since build", name)
memory_map = "Platforms/Xiaomi/lancelotPkg/Library/MemoryMapLib/MemoryMapLib.c"
assert mu["overlay"]["repository_port_inputs"][memory_map] == sha(ROOT / "uefi/files" / memory_map)
old_root = args.accepted_repository / baseline["root"]["path"]
old_mu = args.accepted_repository / baseline["mu"]["path"]
assert sha(old_root) == baseline["root"]["sha256"] and sha(old_mu) == baseline["mu"]["sha256"]
assert sha(kernel["kernel"]["path"]) == kernel["kernel"]["sha256"]
assert sha(mu["firmware"]["path"]) == mu["firmware"]["sha256"]
assert sha(args.private_bundle / "wifi-board.bin") == private["board_sha256"]
out, tree = args.out.resolve(), args.root_tree.resolve()
out.mkdir(parents=True, exist_ok=False)
assert not tree.exists()
runs = []

def run(stage, command, cwd=ROOT, env=None):
    command = list(map(str, command))
    log = out / (stage + ".log")
    with log.open("xb") as stream:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    runs.append({"stage": stage, "exit": result.returncode, "log": log.name, "sha256": sha(log)})
    (out / "progress.json").write_text(json.dumps(runs, indent=2) + "\n")
    print(stage, result.returncode, flush=True)
    if result.returncode:
        print(log.read_text(errors="replace")[-5000:], flush=True)
        raise SystemExit(result.returncode)

tool = args.workspace / "apfs-work/apfs-tool"
apfs = args.workspace / "tools/apfs"
census = subprocess.check_output([tool, "census", old_root], text=True)
(out / "baseline-census.tsv").write_text(census)
modes = out / "root-modes.manifest"
lines = []
baseline_entries = {}
for line in census.splitlines():
    path, kind, mode, uid, gid, size, fnv = line.split("\t")
    assert not any(c.isspace() for c in path), "A manifest path needs explicit escaping"
    baseline_entries[path] = line
    lines.append(f"{mode} {uid} {gid} {path}\n")
modes.write_text("".join(lines))
original_tree = Path(baseline["root_tree"])
run("baseline-root-check", [sys.executable, "-B", apfs / "check-image.py", old_root,
                             "redwood=" + str(original_tree) + ",manifest=" + str(modes)])
shutil.copytree(original_tree, tree, symlinks=True)
destination = tree / "fw/connsys"
assert not destination.exists()
destination.mkdir(parents=True)
for name, digest in private["files"].items():
    source = args.private_bundle / "fw/connsys" / name
    assert source.name == name and sha(source) == digest
    shutil.copy2(source, destination / name)
    (destination / name).chmod(0o600 if name == "WIFI" else 0o644)
specification = "redwood=" + str(tree) + ",manifest=" + str(modes)
probe, root = out / "size-probe.apfs", out / "root.apfs"
writer = [sys.executable, "-B", ROOT / "tools/lancelot-wifi/pack-root.py", "--writer-dir", apfs]
run("root-size", [*writer, "-z", "--slack", "16", "-o", probe, specification])
target = baseline["root"]["bytes"]
assert probe.stat().st_size <= target, "Wi-Fi root must fit the original allocated bootstrap length"
slack = 16 + (target - probe.stat().st_size) // 4096
run("root-pack", [*writer, "-z", "--slack", str(slack), "-o", root, specification])
assert root.stat().st_size == target
run("root-check", [sys.executable, "-B", apfs / "check-image.py", root, specification])
new_census = subprocess.check_output([tool, "census", root], text=True)
new_entries = {line.split("\t")[0]: line for line in new_census.splitlines()}
assert all(new_entries.get(path) == line for path, line in baseline_entries.items())
for name, digest in private["files"].items():
    content = subprocess.check_output([tool, "cat", root, "/fw/connsys/" + name])
    assert hashlib.sha256(content).hexdigest() == digest

profile_path = args.private_bundle / "wifi-profile.json"
profile = read(profile_path)
spec = importlib.util.spec_from_file_location("profile", ROOT / "tools/lancelot-profile.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
cmdline = module.render(profile, {})
assert cmdline.split().count("wifi=1") == 1 and "wifi=0" not in cmdline.split()
loader_tree = out / "loader-source"
for name in ["xnuloader", "xnu/files/bsd/dev"]:
    dst = loader_tree / name
    dst.mkdir(parents=True)
    for p in (ROOT / name).iterdir():
        if p.is_file() and p.suffix in (".c", ".h", ".inc", ".sh", ".py"):
            shutil.copy2(p, dst / p.name)
registry = read(ROOT / "tools/loader-lancelot/root-variants.json")
registry["additional"].append({"image": str(root), "bytes": target, "sha256": sha(root)})
(out / "root-variants.json").write_text(json.dumps(registry, indent=2) + "\n")
spec = importlib.util.spec_from_file_location("pins", ROOT / "tools/loader-lancelot/apply-root-variants.py")
pins = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pins)
header = loader_tree / "xnuloader/lancelot_ubuntu.h"
header.write_text(pins.render(header.read_text(), registry))
run("loader", ["bash", "build.sh", "lancelot"], loader_tree / "xnuloader")
shutil.copy2(loader_tree / "xnuloader/loader-lancelot.efi", out / "loader.efi")
shutil.copy2(kernel["kernel"]["path"], out / "mach.lancelot")
shutil.copy2(mu["firmware"]["path"], out / "SILICIUM_UEFI.fd")
shutil.copy2(args.private_bundle / "wifi-board.bin", out / "wifi-board.bin")
run("payload", [sys.executable, "-B", ROOT / "xnuloader/mkpayload.py", "-o", out / "boot.xnup",
                "kernel=" + str(out / "mach.lancelot"), "wifi-board=" + str(out / "wifi-board.bin"), "--ubuntu-boot", "--cmdline", cmdline])
run("payload-check", [sys.executable, "-B", ROOT / "tools/test-lancelot-payload.py", out / "boot.xnup",
                      "--expect-boot", "--expect-ubuntu", "--expect-graphics", "--expect-cpus", "8"])
run("cmdline-check", [sys.executable, "-B", ROOT / "tools/test-lancelot-cmdline.py", out / "boot.xnup"])
run("kernel-check", [sys.executable, "-B", ROOT / "tools/verify-xnu-checkpoint.py", out / "mach.lancelot"])

# Reuse the accepted Android boot-v2 header and DTB, replacing only its Mu FD.
raw = old_mu.read_bytes()
assert raw[:8] == b"ANDROID!" and struct.unpack_from("<I", raw, 40)[0] == 2
page = struct.unpack_from("<I", raw, 36)[0]
assert page == 2048 and struct.unpack_from("<I", raw, 1632)[0] == 0
align = lambda n: (n + page - 1) // page * page
pos, parts = page, []
for offset in (8, 16, 24):
    size = struct.unpack_from("<I", raw, offset)[0]
    assert pos + size <= len(raw)
    parts.append(raw[pos:pos + size])
    pos += align(size)
parts += [b"", raw[pos:pos + struct.unpack_from("<I", raw, 1648)[0]]]

def identity(data):
    digest = hashlib.sha1()
    for part in data:
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    return digest.digest()

assert identity(parts) == raw[576:596]
retained = parts[1:]
parts[0] = gzip.compress((out / "SILICIUM_UEFI.fd").read_bytes(), mtime=0)
header_bytes = bytearray(raw[:page])
struct.pack_into("<I", header_bytes, 8, len(parts[0]))
header_bytes[576:608] = identity(parts) + bytes(12)
image = bytes(header_bytes) + b"".join(p + bytes(align(len(p)) - len(p)) for p in parts)
assert len(image) < 64 * 1024 * 1024 and parts[1:] == retained
assert gzip.decompress(image[page:page + len(parts[0])]) == (out / "SILICIUM_UEFI.fd").read_bytes()
(out / "Mu.lancelot.img").write_bytes(image)
meta = lambda p: {"path": str(p), "bytes": p.stat().st_size, "sha256": sha(p)}
record = {"passed": True, "device": "lancelot", "round": args.round,
          "kernel": meta(out / "mach.lancelot"), "root": meta(root), "loader": meta(out / "loader.efi"),
          "boot": meta(out / "boot.xnup"), "mu": meta(out / "Mu.lancelot.img"),
          "board": meta(out / "wifi-board.bin"), "cmdline": cmdline,
          "effective_cmdline_bytes": len((module.PREFIX + cmdline).encode()), "root_tree": str(tree),
          "baseline_entries_preserved": len(baseline_entries), "original_root_bytes_preserved": True,
          "wifi_firmware_compressed": True, "early_touch_firmware_raw": True, "firmware_sha256": private["files"],
          "source_worktree": str(ROOT), "kernel_build": str(args.kernel_result), "mu_build": str(args.mu_result),
          "source_inputs": kernel["inputs"], "runs": runs,
          "hardware_verified": False, "deployed": False, "committed": False}
(out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
print("LOCAL WI-FI TEST PAIR READY", out, flush=True)
