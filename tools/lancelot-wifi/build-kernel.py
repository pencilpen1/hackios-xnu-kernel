#!/usr/bin/env python3
"""Build the local native WLAN overlay in an isolated Apple XNU source tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--workspace", type=Path, required=True, help="Existing Linux build dependencies; read only")
ap.add_argument("--source", type=Path, required=True, help="New isolated XNU source path")
ap.add_argument("--out", type=Path, required=True)
ap.add_argument("--jobs", type=int, default=8)
args = ap.parse_args()
if os.name == "nt":
    raise SystemExit("Run this build in WSL, using Linux paths")
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
source = args.source.resolve()
if source == (args.workspace / "xnu-src/xnu-8796.121.2").resolve():
    raise SystemExit("The existing development tree cannot be used as an output")
runs = []

def run(stage, command, env=None):
    log = out / (stage + ".log")
    print(stage, "started", flush=True)
    with log.open("xb") as f:
        result = subprocess.run(list(map(str, command)), stdout=f, stderr=subprocess.STDOUT, env=env)
    runs.append({"stage": stage, "exit": result.returncode, "log": log.name})
    (out / "progress.json").write_text(json.dumps(runs, indent=2) + "\n")
    if result.returncode:
        print(log.read_text(errors="replace")[-6000:], flush=True)
        raise SystemExit(result.returncode)
    print(stage, "passed", flush=True)

if not source.exists():
    env = {**os.environ, "XNU_URL": "file://" + str(args.workspace / "xnu-src/xnu-8796.121.2")}
    run("materialize", ["bash", ROOT / "xnu/apply.sh", source], env)
    (source / ".lancelot-wifi-source.json").write_text(json.dumps({"overlay": str(ROOT)}) + "\n")
else:
    stamp = source / ".lancelot-wifi-source.json"
    if not stamp.is_file() or json.loads(stamp.read_text()).get("overlay") != str(ROOT):
        raise SystemExit("Existing path is not this task's isolated Wi-Fi source")
    # Reuse only our own build directory, never the user's development source.
    shutil.copytree(ROOT / "xnu/files", source, dirs_exist_ok=True)
    run("scheduler-overlay", [sys.executable, ROOT / "tools/apply-lancelot-scheduler.py", source])

inputs = {p.relative_to(ROOT / "xnu/files").as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
          for p in (ROOT / "xnu/files").rglob("*") if p.is_file()}
control_paths = [ROOT / "xnu/apply.sh", ROOT / "xnu/redwood.patch", ROOT / "xnu/lancelot.patch",
                 *sorted((ROOT / "tools").glob("apply-lancelot-*.py"))]
controls = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in control_paths}
env = {**os.environ, "XNU_SRC": str(source), "PYTHONDONTWRITEBYTECODE": "1"}
run("kernel", ["bash", args.workspace / "xnu-build.sh", "-j" + str(args.jobs), "build"], env)
kernel = source / "BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
if not kernel.is_file():
    raise SystemExit("Expected linked kernel is missing")
target = out / "mach.lancelot"
shutil.copy2(kernel, target)
record = {"passed": True, "full_kernel_compile_link": True, "hardware_verified": False,
          "source": str(source), "inputs": inputs, "runs": runs,
          "overlay_controls": controls,
          "scheduler_source_sha256": hashlib.sha256((source / "osfmk/kern/sched_prim.c").read_bytes()).hexdigest(),
          "kernel": {"path": str(target), "bytes": target.stat().st_size,
                     "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}}
(out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
print("Linked native WLAN kernel:", target, flush=True)
