#!/usr/bin/env python3
"""Build a version-matched, on-demand full-root bootstrap profile in a new directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess

# Explicit eager set: never promote every on-demand job in a previously expanded
# input profile to an eager job by treating all filenames as command-line labels.
BOOT_JOBS = """
com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
com.apple.powerd com.apple.analyticsd com.apple.logd_helper
com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd com.apple.SpringBoard com.apple.wifid
""".split()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--workspace", required=True, type=Path)
    ap.add_argument("--ios-source", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--bootdiag", required=True, type=Path)
    args = ap.parse_args()
    workspace, source, out = args.workspace.resolve(), args.ios_source.resolve(), args.out.resolve()
    patch = Path(__file__).resolve().parents[1] / "common/patches/userland-job-env-name.patch"
    command = ["git", "-C", str(workspace), "apply"]
    if subprocess.run([*command, "--check", str(patch)], capture_output=True).returncode == 0:
        subprocess.run([*command, str(patch)], check=True)
    else:
        subprocess.run([*command, "--reverse", "--check", str(patch)], check=True)
    manifest = json.loads((source / "source.json").read_text())
    system = plistlib.loads((source / "rootfs/System/Library/CoreServices/SystemVersion.plist").read_bytes())
    if (system["ProductBuildVersion"], system["ProductVersion"]) != (manifest["build"], manifest["version"]):
        raise SystemExit("BuildManifest/extracted SystemVersion mismatch")
    program = args.bootdiag.read_bytes() if args.bootdiag.is_file() else b""
    if len(program) < 32 or struct.unpack_from("<II", program) != (0xfeedfacf, 0x0100000c):
        raise SystemExit("bootdiag is not an arm64 Mach-O program")
    out.mkdir(parents=True, exist_ok=False)
    root = out / "root"
    original = workspace / "userland/root"
    shutil.copytree(original, root, symlinks=True)
    def local_output(path):
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise SystemExit("output path escapes the copied profile: " + str(path))
        return path
    migration = root / "var/mobile/Library/Preferences/com.apple.migration.plist"
    local_output(migration)
    data = plistlib.loads(migration.read_bytes())
    before = data.get("LastSystemVersion")
    data["LastSystemVersion"] = manifest["build"]
    data.setdefault("DMLastMigrationResults", {})["buildVersion"] = manifest["build"]
    migration.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_XML))
    jobs = local_output(root / "LaunchDaemons")
    labels = sorted(BOOT_JOBS)
    # The copied output is ours; regenerate it from the actual full-root listing.
    shutil.rmtree(jobs)
    jobs.mkdir()
    env = dict(os.environ, OSLOG="com.apple.SpringBoard com.apple.backboardd")
    log = subprocess.check_output(["python3", str(workspace / "userland/mkjobs.py"), "--rest",
                                   "--root-volume", str(source / "rootfs.slt"),
                                   str(source / "rootfs"), str(jobs), *labels], env=env, stderr=subprocess.STDOUT)
    (out / "jobs-build.log").write_bytes(log)
    # Keep existing port service jobs, except the interactive CPU/surface stress demo.
    for path in (original / "LaunchDaemons").glob("com.redwood.*.plist"):
        if path.stem != "com.redwood.demo": shutil.copy2(path, jobs / path.name)
    for label, name in (("com.apple.SpringBoard", "SpringBoard"), ("com.apple.backboardd", "backboardd")):
        path = jobs / (label + ".plist")
        job = plistlib.loads(path.read_bytes())
        job["StandardOutPath"] = job["StandardErrorPath"] = f"/private/var/log/hackios/{name}.log"
        path.write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
    local_output(root / "bin")
    local_output(root / "bin/bootdiag")
    shutil.copy2(args.bootdiag, root / "bin/bootdiag")
    (root / "bin/bootdiag").chmod(0o755)
    diag = {"Label": "com.hackios.bootdiag", "ProgramArguments": ["/redwood/bin/bootdiag"],
            "RunAtLoad": True, "KeepAlive": True, "ProcessType": "Background", "ThrottleInterval": 30,
            "StandardOutPath": "/dev/console", "StandardErrorPath": "/dev/console"}
    (jobs / "com.hackios.bootdiag.plist").write_bytes(plistlib.dumps(diag, fmt=plistlib.FMT_BINARY))
    with local_output(root / "etc/var.skel").open("a") as file:
        file.write("d\t755\t0\t0\t/private/var/log/hackios\n")
    report = {"build": manifest["build"], "version": manifest["version"], "previous_migration_seed": before,
              "migration_seed_scope": "fresh RAM data-volume template only; persisted restores remain authoritative",
              "jobs_before": len(list((original / "LaunchDaemons").glob("*.plist"))),
              "jobs_after": len(list(jobs.glob("*.plist"))), "stress_demo_removed": True,
              "wifid_present": (jobs / "com.apple.wifid.plist").exists(),
              "bootdiag_sha256": hashlib.sha256((root / "bin/bootdiag").read_bytes()).hexdigest()}
    if report["jobs_after"] <= report["jobs_before"] or not report["wifid_present"]:
        raise SystemExit("on-demand dependency generation did not produce the expected expanded profile")
    (out / "profile.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
