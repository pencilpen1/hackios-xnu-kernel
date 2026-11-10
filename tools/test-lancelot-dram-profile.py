#!/usr/bin/env python3
"""Check read-only MT6768 DRAM snapshots and known vendor OPP encodings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if os.name == "nt":
    def linux(s):
        return "/mnt/"+s[0].lower()+s[2:].replace("\\","/") if len(s)>2 and s[1]==":" else s
    raise SystemExit(subprocess.call(["wsl","-d","Ubuntu","--exec","python3","-B","-u",
        linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out",type=Path,required=True)
args=parser.parse_args()
out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=["xnu/files/bsd/dev/lancelot_dram_profile.h","xnu/files/bsd/dev/lancelot_dram_profile.inc",
       "xnu/files/bsd/dev/lancelot_hakios_mali.inc","tools/mali-tests/dram_profile_test.c",
       "tools/test-lancelot-dram-profile.py"]
sources={name:sha(ROOT/name) for name in names}
kernel=(ROOT/names[1]).read_text()
assert "*(const volatile uint32_t *)" in kernel
assert all(word not in kernel for word in ["write32","writel(","mt_secure_call","sspm_ipi","memcpy("])
assert all(address in kernel for address in ["0x1022a000","0x10228000","0x10219000"])
env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1","UBSAN_OPTIONS":"halt_on_error=1"}
runs=[]
for cc in ["clang","gcc"]:
    binary=out/cc
    compile=[cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
             "-fno-sanitize-recover=all","-I"+str(ROOT/"xnu/files/bsd/dev"),
             str(ROOT/"tools/mali-tests/dram_profile_test.c"),"-o",str(binary)]
    for stage,command in [("build",compile),("run",[str(binary)])]:
        log=out/(cc+"-"+stage+".log")
        with log.open("xb") as stream:
            result=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,env=env,timeout=90)
        runs.append({"compiler":cc,"stage":stage,"exit":result.returncode,"log":log.name,"sha256":sha(log)})
        print(cc,stage,result.returncode,flush=True)
        if result.returncode:
            print(log.read_text(errors="replace"));raise SystemExit(result.returncode)
assert all(sha(ROOT/name)==digest for name,digest in sources.items())
(out/"result.json").write_text(json.dumps({"passed":True,"sources":sources,"runs":runs,
    "hardware_access":False,"MMIO_write_interface":False,"snapshot_reads_max":16,
    "runtime_DRAM_clock_verified":False},indent=2)+"\n")
print("PASS",out,flush=True)
