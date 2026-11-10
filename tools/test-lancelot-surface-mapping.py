#!/usr/bin/env python3
"""Exercise IOSurface mapping lock order and concurrent ref publication."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if os.name == "nt":
    def linux(value):
        return "/mnt/" + value[0].lower() + value[2:].replace("\\", "/") if len(value) > 2 and value[1] == ":" else value
    raise SystemExit(subprocess.call(["wsl", "-d", "Ubuntu", "--exec", "python3", "-B", "-u",
        linux(str(Path(__file__).resolve())), *map(linux, sys.argv[1:])]))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
names = ["xnu/files/iokit/Kernel/MiniIOSurface.cpp", "xnu/files/iokit/Kernel/LancelotMaliCpuLease.h",
         "tools/mali-tests/surface_addref_before.inc", "tools/mali-tests/surface_mapping_test.cpp",
         "tools/test-lancelot-surface-mapping.py"]
sources = {name: sha(ROOT / name) for name in names}
surface = (ROOT / names[0]).read_text()
lease = (ROOT / names[1]).read_text()

def method(text, signature):
    begin = text.index(signature)
    pos, depth = text.index("{", begin) + 1, 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[begin:pos] + "\n"

for variant in ["before", "current"]:
    directory = out / variant
    directory.mkdir()
    add = (ROOT / names[2]).read_text() if variant == "before" else "struct minisurf_client_ref *\n" + method(surface, "IOSurfaceRootUserClient::addRef(")
    # Keep the current caller retain in both variants to isolate the G2 addRef
    # lock error. No scheduler or lock behavior is substituted in production code.
    functions = add + "struct minisurf_client_ref *\n" + method(surface, "IOSurfaceRootUserClient::findRef(")
    functions += "void\n" + method(surface, "IOSurfaceRootUserClient::dropRef(")
    functions += "IOReturn\n" + method(surface, "IOSurfaceRootUserClient::methodLookup(")
    (directory / "actual-surface-ref.inc").write_text(functions)
    (directory / "actual-mapping-gate.inc").write_text(method(lease, "int lockMapping(") + method(lease, "void unlockMapping("))
    (directory / "actual-surface-holds.inc").write_text("static void\n" + method(surface, "surfaceHold(MiniIOSurface * s)") +
        "static void\n" + method(surface, "surfaceUnhold(MiniIOSurface * s)"))

for name in ["methodCreate", "methodLookup", "methodLookupFromPort"]:
    caller = method(surface, "IOSurfaceRootUserClient::" + name + "(")
    start = caller.index("retain();")
    assert start < caller.index("addRef(") < caller.index("IOLockUnlock(gSurfaceLock);", start) < caller.index("release();", start)
assert "addRef(right->surface, &known)" in surface

runs = []
env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
for cc in ["clang++", "g++"]:
    for variant in ["before", "current"]:
        name = cc.replace("+", "p") + "-" + variant
        binary = out / name
        command = [cc, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + str(out / variant)]
        if variant == "before": command.append("-DBEFORE_MAPPING")
        command += [str(ROOT / names[3]), "-o", str(binary)]
        for stage, cmd in [("build", command), ("run", [str(binary)])]:
            log = out / (name + "-" + stage + ".log")
            with log.open("xb") as stream:
                result = subprocess.run(cmd, stdout=stream, stderr=subprocess.STDOUT, env=env, timeout=90)
            runs.append({"name": name, "stage": stage, "exit": result.returncode, "log": log.name, "sha256": sha(log)})
            (out / "progress.json").write_text(json.dumps(runs, indent=2) + "\n")
            print(name, stage, result.returncode, flush=True)
            if result.returncode:
                print(log.read_text(errors="replace"), flush=True)
                raise SystemExit(result.returncode)
assert all(sha(ROOT / name) == digest for name, digest in sources.items())
(out / "result.json").write_text(json.dumps({"passed": True, "sources": sources, "runs": runs,
    "hardware_access": False, "G2_mapping_global_lock_cycle_reproduced": True,
    "G3_CPU_unlock_progress_during_mapping_wait": True,
    "close_growth_duplicate_detach_cleanup_exercised": True,
    "client_retained_by_actual_lookup_during_close": True,
    "GPU_completion_and_VM_are_host_stubs": True}, indent=2) + "\n")
print("PASS", out, flush=True)
