#!/usr/bin/env python3
"""Exercise the actual userland job generator with/without RWA_PACKED16."""
from pathlib import Path
import os
import plistlib
import subprocess
import sys
import tempfile

generator = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    source = tmp / "rootfs"
    jobs = source / "System/Library/LaunchDaemons"
    jobs.mkdir(parents=True)
    (source / "usr/libexec").mkdir(parents=True)
    labels = ["com.test.primary", "com.test.helperA", "com.test.helperB"]
    for label in labels:
        program = "/usr/libexec/" + label
        (source / program.lstrip("/")).touch()
        (jobs / (label + ".plist")).write_bytes(plistlib.dumps({
            "Label": label, "Program": program, "MachServices": {label: True},
            "RunAtLoad": True, "KeepAlive": True,
        }))
    for enabled in (False, True):
        out = tmp / str(enabled)
        env = dict(os.environ)
        env.pop("RWA_PACKED16", None)
        if enabled: env["RWA_PACKED16"] = "1"
        subprocess.run([sys.executable, str(generator), "--rest", str(source), str(out), labels[0]],
                       check=True, env=env, stdout=subprocess.DEVNULL)
        assert {p.name for p in out.iterdir()} == {label + ".plist" for label in labels}
        for label in labels:
            job = plistlib.loads((out / (label + ".plist")).read_bytes())
            assert job["Label"] == label
            if label != labels[0]: assert "RunAtLoad" not in job and "KeepAlive" not in job
            assert job.get("EnvironmentVariables", {}).get("RWA_PACKED16") == ("1" if enabled else None)
    print("On-demand jobs retain distinct plist names; environment forwarding cannot overwrite filenames")
