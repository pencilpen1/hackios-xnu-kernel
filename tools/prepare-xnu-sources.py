#!/usr/bin/env python3
"""Pre-fetch the pinned public build sources while host tool archives download."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

SOURCES = (
    ("AvailabilityVersions", "apple-oss-distributions/AvailabilityVersions", "5d9baee345b3bc72b26c71adc65f94f2d5e9ced2"),
    ("libdispatch", "apple-oss-distributions/libdispatch", "a641558f447659d90ad002ef0ee6f0212273fbfc"),
    ("libpthread", "apple-oss-distributions/libpthread", "a6c70ca0e544347a0377babf748f831577ec24f9"),
    ("mm_bootstrap_cmds", "markmentovai/bootstrap_cmds", "f337e600741638896fb6056ce87c6479f74b89a3"),
    ("tcsh", "tcsh-org/tcsh", "b7db07931546178c4cb2542b5fc850c483628ed5"),
    ("unifdef", "fanf2/unifdef", "08d12cd3059fe072929ea001ad9833f5487fd355"),
)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("workspace", type=Path)
    args = ap.parse_args()

    def fetch(item):
        name, repo, commit = item
        dest = args.workspace.resolve() / "xnu-deps" / name
        if dest.exists():
            actual = subprocess.check_output(["git", "-C", str(dest), "rev-parse", "HEAD"], text=True).strip()
            if actual != commit:
                raise ValueError("existing source does not match pinned revision: " + name)
            return
        dest.mkdir(parents=True)
        def git(*command):
            subprocess.run(["git", "-C", str(dest), *command], check=True, timeout=300)
        git("init", "-q")
        git("remote", "add", "origin", "https://github.com/" + repo + ".git")
        git("fetch", "-q", "--depth", "1", "origin", commit)
        git("-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD")
        print(name, commit, flush=True)

    with ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(fetch, SOURCES))


if __name__ == "__main__":
    main()
