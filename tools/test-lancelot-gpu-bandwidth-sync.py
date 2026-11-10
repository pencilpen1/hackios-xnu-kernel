#!/usr/bin/env python3
"""Exercise production cache ownership and presenter pacing without hardware."""
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
names = ["xnu/files/iokit/Kernel/LancelotMaliCpuLease.h",
         "xnu/files/iokit/Kernel/lancelot_hakios_surface_handoff.inc",
         "xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp",
         "tools/mali-tests/gpu_cache_handoff_test.cpp", "tools/mali-tests/presenter_pacing_test.cpp",
         "tools/mali-tests/gpu_prepare_before_sync.inc", "tools/mali-tests/presenter_before_pacing.inc",
         "tools/test-lancelot-gpu-bandwidth-sync.py",
         "xnu/files/bsd/dev/lancelot_hakios_mali.inc"]
sources = {name: sha(ROOT / name) for name in names}
lease = (ROOT / names[0]).read_text()
handoff = (ROOT / names[1]).read_text()
display = (ROOT / names[2]).read_text()
native = (ROOT / "xnu/files/bsd/dev/lancelot_hakios_mali.inc").read_text()

def method(text, signature):
    begin = text.index(signature)
    pos, depth = text.index("{", begin) + 1, 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[begin:pos] + "\n"

show_begin = display.index("{", display.index("IOMobileFramebuffer::show(")) + 1
show_end = display.index("\t/* its turn has come:", show_begin)
callbacks = handoff[handoff.index('extern "C" void lancelot_hakios_cache_sync'):handoff.index("class HakiosSurfaceHandoff")]
for variant in ["before", "current"]:
    directory = out / variant
    directory.mkdir()
    prepare = (ROOT / "tools/mali-tests/gpu_prepare_before_sync.inc").read_text() if variant == "before" else method(lease, "int beginGpu(")
    prefix = (ROOT / "tools/mali-tests/presenter_before_pacing.inc").read_text() if variant == "before" else display[show_begin:show_end]
    (directory / "actual-gpu-prepare.inc").write_text(prepare)
    (directory / "actual-gpu-retire.inc").write_text(method(lease, "int markLegacy(") + method(lease, "int endGpu("))
    (directory / "actual-surface-cache-callbacks.inc").write_text(callbacks)
    (directory / "actual-cache-operation.inc").write_text(method(native, "void lancelot_hakios_cache_sync("))
    (directory / "actual-presenter-prefix.inc").write_text(prefix)

runs = []
env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
for cc in ["clang++", "g++"]:
    for variant in ["before", "current"]:
        for test, define in [("gpu_cache_handoff", "BEFORE_SYNC"), ("presenter_pacing", "BEFORE_PACING")]:
            name = cc.replace("+", "p") + "-" + variant + "-" + test
            binary = out / name
            command = [cc, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-I" + str(out / variant)]
            if variant == "before": command.append("-D" + define)
            command += [str(ROOT / ("tools/mali-tests/" + test + "_test.cpp")), "-o", str(binary)]
            for stage, cmd in [("build", command), ("run", [str(binary)])]:
                path = out / (name + "-" + stage + ".log")
                with path.open("xb") as stream:
                    result = subprocess.run(cmd, stdout=stream, stderr=subprocess.STDOUT, env=env, timeout=90)
                runs.append({"name": name, "stage": stage, "exit": result.returncode, "log": path.name, "sha256": sha(path)})
                (out / "progress.json").write_text(json.dumps(runs, indent=2) + "\n")
                print(name, stage, result.returncode, flush=True)
                if result.returncode:
                    print(path.read_text(errors="replace"), flush=True)
                    raise SystemExit(result.returncode)
assert all(sha(ROOT / name) == digest for name, digest in sources.items())
record = {"passed": True, "sources": sources, "runs": runs, "hardware_access": False,
          "old_first_clean_publication_race_reproduced": True,
          "first_clean_serialized_with_cookie_publication": True,
          "cache_cost_charged_to_preparation_deadline": True,
          "overlapping_GPU_retirement_and_quarantine_verified": True,
          "pixel_only_cache_ranges_preserve_shared_metadata": True,
          "cache_bytes_and_time_accounting_with_trace_disabled_passthrough": True,
          "old_presenter_blocks_GPU_during_pacing_reproduced": True,
          "presenter_paces_before_owning_CPU_readers": True,
          "pixel_reads_and_failure_cleanup_keep_both_gates": True}
(out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
print("PASS", out, flush=True)
