#!/usr/bin/env python3
"""Use the existing APFS writer, retaining the early touch firmware as raw data.

CONNSYS reads through vnodes after root mount, so its firmware can use the same
verified APFS compression as the existing userland binaries. Touch starts
before that and still requires its original raw file.
"""
import argparse
from pathlib import Path
import sys

ap = argparse.ArgumentParser(add_help=False)
ap.add_argument("--writer-dir", type=Path, required=True)
args, remaining = ap.parse_known_args()
sys.path.insert(0, str(args.writer_dir.resolve()))
import mkapfs

original = mkapfs.Volume.is_plain

def is_plain(volume, node):
    return node.path == "fw/nvt_dj_fw.bin" or original(volume, node)

mkapfs.Volume.is_plain = is_plain
sys.argv = [sys.argv[0], *remaining]
mkapfs.main()
