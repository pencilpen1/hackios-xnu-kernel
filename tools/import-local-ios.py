#!/usr/bin/env python3
"""Import an iPhone X IPSW offline; select DMGs from its actual BuildManifest.

Run on a Linux filesystem (APFS extraction uses names NTFS cannot represent).
The manifest records the real iOS build; importing is not an ABI compatibility test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import zipfile


def select_identity(manifest, board):
    matches = [i for i in manifest["BuildIdentities"]
               if i["Info"]["DeviceClass"] == board and i["Info"]["RestoreBehavior"] == "Erase"]
    if len(matches) != 1:
        raise ValueError("expected one erase identity for " + board)
    components = matches[0]["Manifest"]
    return {name: components[key]["Info"]["Path"] for name, key in (
        ("rootfs.dmg", "OS"), ("cryptex-os.dmg", "Cryptex1,SystemOS"),
        ("cryptex-app.dmg", "Cryptex1,AppOS"))}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ipsw", type=Path)
    ap.add_argument("--workspace", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--board", choices=("d22ap", "d221ap"), default="d22ap")
    args = ap.parse_args()
    workspace, out = args.workspace.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)

    def run(command, logfile=None, allowed=(0,)):
        summary = " ".join(str(c) for c in command)
        print("+", summary if len(summary) < 500 else f"{command[0]} ({len(command) - 1} arguments)", flush=True)
        if logfile:
            with (out / logfile).open("wb") as log:
                r = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT)
        else:
            r = subprocess.run(command, cwd=out)
        if r.returncode not in allowed:
            raise RuntimeError(f"command exited {r.returncode}; see {out / (logfile or '.')}")

    with zipfile.ZipFile(args.ipsw) as archive:
        manifest_bytes = archive.read("BuildManifest.plist")
        manifest = plistlib.loads(manifest_bytes)
        members = select_identity(manifest, args.board)
        provenance = {"version": manifest["ProductVersion"], "build": manifest["ProductBuildVersion"],
                      "board": args.board, "source": args.ipsw.name, "members": {}}
        (out / "BuildManifest.plist").write_bytes(manifest_bytes)
        for name, member in members.items():
            digest = hashlib.sha256()
            print("Extracting", member, "->", name, flush=True)
            with archive.open(member) as src, (out / name).open("xb") as dst:
                while data := src.read(4 << 20):
                    digest.update(data)
                    dst.write(data)
            provenance["members"][name] = {"path": member, "sha256": digest.hexdigest(),
                                           "size": (out / name).stat().st_size}
    (out / "source.json").write_text(json.dumps(provenance, indent=2) + "\n")
    for name in ("select-system.py", "mkvarskel.py"):
        shutil.copy2(workspace / "ios165" / name, out / name)
    unzbm = workspace / "xnu-deps/host/bin/unzbm"
    if not unzbm.exists():
        unzbm.parent.mkdir(parents=True, exist_ok=True)
        run(["cc", "-O2", "-w", "-o", str(unzbm),
             str(workspace / "xnu-deps/build/lzbitmap/unzbm.c"),
             str(workspace / "xnu-deps/build/lzbitmap/libzbitmap.c")])

    def decompress(directory):
        paths = [str(p)[:-len(":com.apple.decmpfs")] for p in (out / directory).rglob("*:com.apple.decmpfs")]
        for start in range(0, len(paths), 100):
            run([str(unzbm), *paths[start:start + 100]])
        left = list((out / directory).rglob("*:com.apple.*"))
        if left:
            raise RuntimeError(f"unexpanded APFS attributes in {directory}: {left[:3]}")

    run(["7z", "l", "-slt", "rootfs.dmg"], "rootfs.slt")
    run(["python3", "select-system.py", "rootfs.slt", "system.files", "system.manifest"])
    run(["python3", "mkvarskel.py", "rootfs.slt"], "var.skel")
    run(["7z", "x", "-y", "-orootfs", "rootfs.dmg", "@system.files"], "extract-root.log", (0, 2))
    decompress("rootfs")
    trim = workspace / "tools/font_trim.py"
    emoji = out / "rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc"
    pingfang = out / "rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc"
    run(["python3", str(trim), "emoji", str(emoji), str(emoji), "40,64,96"])
    run(["python3", str(trim), "faces", str(pingfang), str(pingfang), r"^\.?PingFang(SC|TC|HK)-(Regular|Semibold)$"])
    run(["7z", "x", "-y", "-ocryptex", "cryptex-os.dmg", "-xr!com.apple.dyld", "-x!System/DriverKit"],
        "extract-cryptex.log", (0, 2))
    decompress("cryptex")
    run(["7z", "x", "-y", "-ocryptex.cache", "cryptex-os.dmg", "System/Library/Caches/com.apple.dyld",
         "-xr!*.symbols*"], "extract-dyld.log", (0, 2))
    decompress("cryptex.cache")
    shutil.move(out / "cryptex.cache/System/Library/Caches/com.apple.dyld", out / "dyld-cache")
    run(["7z", "x", "-y", "-ocryptex-app", "cryptex-app.dmg"], "extract-app.log", (0, 2))
    decompress("cryptex-app")
    required = [out / "rootfs/sbin/launchd", out / "dyld-cache/dyld_shared_cache_arm64",
                out / "cryptex-app/usr/libexec/webbookmarksd"]
    for path in required:
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError("missing required input: " + str(path))
    with (out / "rootfs/System/Library/CoreServices/SystemVersion.plist").open("rb") as f:
        system = plistlib.load(f)
    if system["ProductBuildVersion"] != provenance["build"]:
        raise RuntimeError("extracted OS does not match BuildManifest")
    for marker in (".done-rootfs", ".done-cryptex", ".done-cryptex-app"):
        (out / marker).touch()
    print(f"Imported iOS {provenance['version']} ({provenance['build']}) in {out}")


if __name__ == "__main__":
    main()
