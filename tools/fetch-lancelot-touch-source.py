#!/usr/bin/env python3
"""Fetch pinned Shockwave touch references, mirror first; verify Git blob + size.

Downloads reference files only, never runs vendor code or accesses the phone.
Use complete directory listings instead of GitHub's truncated recursive root tree.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import datetime
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import urllib.error
import urllib.parse
import urllib.request

REPO = "MrShockWAVEog/shockwave_kernel_xiaomi_mt6768"
COMMIT = "6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7"
TREE = "840cb76830615e7ec4ee5e2aee2e2ff229d6f5bb"
API = "https://api.github.com/repos/" + REPO
LIMIT = 4 * 1024 * 1024
DIRECTORIES = {
    "drivers/input/touchscreen": ["Kconfig", "Makefile", "xiaomi*", "tp_common.h"],
    "drivers/input/touchscreen/mediatek": ["Kconfig", "Makefile", "*.c", "*.h"],
    "drivers/input/touchscreen/mediatek/NT36xxx_HOSTDL_SPI": ["*"],
    "drivers/input/touchscreen/mediatek/NT36672": ["*"],
    "drivers/input/touchscreen/mediatek/FT8719P": ["Kconfig", "Makefile", "focaltech_core.c",
                                                "focaltech_core.h", "focaltech_config.h", "focaltech_spi.c"],
    "drivers/spi": ["Kconfig", "Makefile", "spi-mt65xx.c", "spi-mt65xx-dev.c", "spi.c"],
    "include/linux/platform_data": ["spi-mt65xx.h"],
    "include/linux/spi": ["spi.h"],
    "include/linux/input": ["tp_common.h"],
    "drivers/pinctrl/mediatek": ["Kconfig", "Makefile", "mtk-eint.*", "pinctrl-mt676[58].c",
                                  "pinctrl-mtk-mt676[58].h", "pinctrl-mtk-common*", "pinctrl-paris.*"],
    "drivers/clk/mediatek": ["Kconfig", "Makefile", "clk-mt6768*", "clk-mt6765.c", "clk-mt6765-pg.*",
                              "clk-mtk*", "clk-gate.*", "clk-mux.*", "clk-pll.*", "clk-cpumux.*",
                              "mt6768_clkmgr.*"],
    "drivers/regulator": ["Kconfig", "Makefile", "mt6358-regulator.c"],
    "drivers/misc/mediatek/include/mt-plat": ["mtk_boot*.h", "upmu_common.h", "mtk_gpio.h"],
    "drivers/misc/mediatek/pmic/include/mt6358": ["*.h"],
    "drivers/misc/mediatek/pmic/mt6358/v1": ["Makefile", "pmic.c", "pmic_api.c", "pmic_common.c",
                                            "pmic_initial_setting.c", "regulator_codegen.c"],
    "drivers/misc/mediatek/pmic_wrap": ["Kconfig", "Makefile", "mtk_pmic_wrap.c"],
    "drivers/misc/mediatek/pmic_wrap/mt6768": ["*.c", "*.h", "Makefile"],
    "arch/arm64/boot/dts/mediatek": ["Makefile", "lancelot.dts", "mt6768.dts", "mt6358.dtsi",
                                     "cust_mt6768_touch*.dtsi"],
    "arch/arm64/configs/vendor": ["lancelot.config", "mt6768_defconfig", "xiaomi.config"],
    "include/dt-bindings/clock": ["mt676[58]-clk.h"],
    "include/dt-bindings/pinctrl": ["mt676[58]-pinfunc.h", "mt65xx.h"],
    "include/dt-bindings/gpio": ["gpio.h"],
    "include/dt-bindings/interrupt-controller": ["irq.h", "arm-gic.h"],
    "drivers/misc/mediatek/lcm/nt36672A_fhdp_dsi_vdo_dijing_j19": ["*.c", "Makefile"],
    "drivers/misc/mediatek/lcm": ["lcm_drv.h", "lcm_gpio.c", "lcm_gpio.h"],
}


def safe_path(path):
    p = PurePosixPath(path)
    if p.is_absolute() or "\\" in path or ":" in path or any(v in (".", "..", ".git") for v in p.parts):
        raise ValueError("unsafe source path")
    if not p.parts or str(p) != path:
        raise ValueError("non-canonical source path")
    return p


def verify_blob(data, size, sha):
    if not 0 <= size <= LIMIT or len(data) != size or not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("source length/hash metadata mismatch")
    actual = hashlib.sha1(b"blob " + str(size).encode() + b"\0" + data).hexdigest()
    if actual != sha:
        raise ValueError("source Git blob hash mismatch")
    return hashlib.sha256(data).hexdigest()


def request(url, limit, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": "lancelot-source-audit", "Accept-Encoding": "identity"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError("HTTP response exceeds bound")
        return data, response.geturl()


def api_json(path):
    data, _ = request(API + path, LIMIT)
    return json.loads(data)


def directory(path):
    result = api_json("/contents/" + urllib.parse.quote(path, safe="/") + "?ref=" + COMMIT)
    if not isinstance(result, list) or len(result) >= 1000:
        raise ValueError("missing or possibly incomplete directory: " + path)
    for entry in result:
        safe_path(entry["path"])
        if str(PurePosixPath(entry["path"]).parent) != path:
            raise ValueError("directory entry escaped its parent")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--mirror", default="https://ghfast.top/")
    ap.add_argument("--resume", action="store_true", help="reuse only this fetcher's matching output, checking every file")
    args = ap.parse_args()
    if not args.mirror.startswith("https://"):
        ap.error("mirror must use HTTPS")
    if args.out.exists():
        if not args.resume:
            ap.error("output exists; never overwrite an unfamiliar directory")
        plan = json.loads((args.out / "fetch-plan.json").read_text())
        if plan.get("repo") != REPO or plan.get("commit") != COMMIT:
            ap.error("resume output is not this pinned source set")
    else:
        args.out.mkdir(parents=True)
    commit = api_json("/git/commits/" + COMMIT)
    if commit["sha"] != COMMIT or commit["tree"]["sha"] != TREE:
        raise ValueError("pinned commit/tree identity changed")
    selected, listings = {}, {}
    for path, patterns in DIRECTORIES.items():
        entries = directory(path)
        listings[path] = entries
        for e in entries:
            if e["type"] == "file" and any(fnmatch.fnmatchcase(e["name"], p) for p in patterns):
                selected[e["path"]] = e

    # Board-specific DTS includes can be nested: retain that complete small subtree.
    def board(path):
        entries = directory(path)
        listings[path] = entries
        for e in entries:
            if e["type"] == "dir":
                board(e["path"])
            elif e["type"] == "file" and PurePosixPath(e["path"]).suffix in (".dts", ".dtsi", ".h"):
                selected[e["path"]] = e
    board("arch/arm64/boot/dts/mediatek/lancelot")
    for name in ("COPYING", "Makefile", "include/linux/hqsysfs.h"):
        e = api_json("/contents/" + name + "?ref=" + COMMIT)
        if e["path"] != name or e["type"] != "file":
            raise ValueError("invalid top-level source metadata")
        selected[name] = e
    required = ["drivers/input/touchscreen/mediatek/NT36672/" + n for n in
                ("nt36672.c", "nt36672.h", "nt36672_fw_update.c", "nt36672_mem_map.h", "Kconfig", "Makefile")]
    required += ["drivers/spi/spi-mt65xx.c", "include/linux/platform_data/spi-mt65xx.h",
                 "arch/arm64/boot/dts/mediatek/lancelot.dts", "arch/arm64/configs/vendor/lancelot.config"]
    if any(p not in selected for p in required):
        raise ValueError("required touch/platform references missing")
    if sum(e["size"] for e in selected.values()) > 24 * 1024 * 1024:
        raise ValueError("unexpectedly large selective download")
    paths = list(selected)
    if len({p.casefold() for p in paths}) != len(paths):
        raise ValueError("source paths collide on case-insensitive filesystems")
    plan = {"repo": REPO, "commit": COMMIT, "tree": TREE, "complete_directory_listings": True,
            "commit_metadata": commit, "listings": listings, "selected": selected}
    (args.out / "fetch-plan.json").write_text(json.dumps(plan, indent=2) + "\n")

    def download(item):
        path, e = item
        rel = safe_path(path)
        dest = args.out / "source" / Path(*rel.parts)
        if dest.exists():
            digest = verify_blob(dest.read_bytes(), e["size"], e["sha"])
            return path, {"bytes": e["size"], "git_blob": e["sha"], "sha256": digest, "reused_verified": True}
        raw = "https://raw.githubusercontent.com/" + REPO + "/" + COMMIT + "/" + urllib.parse.quote(path, safe="/")
        errors = []
        for url in (args.mirror.rstrip("/") + "/" + raw, raw):
            try:
                data, actual_url = request(url, e["size"])
                digest = verify_blob(data, e["size"], e["sha"])
                dest.parent.mkdir(parents=True, exist_ok=True)
                with dest.open("xb") as file:
                    file.write(data)
                return path, {"bytes": len(data), "git_blob": e["sha"], "sha256": digest,
                              "url": actual_url, "failed_routes": errors}
            except (OSError, ValueError) as error:
                errors.append({"url": url, "error": str(error)})
        raise RuntimeError("verified download failed for " + path + ": " + repr(errors))

    manifest = {"repo": REPO, "requested_branch": "17.0", "commit": COMMIT, "tree": TREE,
                "checked_at": datetime.datetime.now(datetime.timezone.utc).isoformat(), "complete": False,
                "phone_accessed": False, "files": {}, "errors": []}
    with ThreadPoolExecutor(max_workers=4) as pool:
        futures = {pool.submit(download, item): item[0] for item in sorted(selected.items())}
        for future in as_completed(futures):
            try:
                path, record = future.result()
                manifest["files"][path] = record
                print(f"verified {len(manifest['files'])}/{len(selected)} {path}", flush=True)
            except Exception as error:
                manifest["errors"].append({"path": futures[future], "error": str(error)})
    manifest["complete"] = not manifest["errors"] and len(manifest["files"]) == len(selected)
    (args.out / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if not manifest["complete"]:
        raise SystemExit("incomplete source download; inspect source-manifest.json, then use --resume")
    print(f"Complete: {len(selected)} verified files, {sum(e['size'] for e in selected.values())} bytes; no phone access")


if __name__ == "__main__":
    main()
