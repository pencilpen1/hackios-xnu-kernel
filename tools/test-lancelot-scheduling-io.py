#!/usr/bin/env python3
"""Test actual idle scheduler hint and eMMC interrupt scope against G3."""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if os.name=="nt":
    def linux(s): return "/mnt/"+s[0].lower()+s[2:].replace("\\","/") if len(s)>2 and s[1]==":" else s
    raise SystemExit(subprocess.call(["wsl","-d","Ubuntu","--exec","python3","-B","-u",linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument("--out",type=Path,required=True);args=p.parse_args()
out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=["xnu/files/osfmk/kern/lancelot_sched.h","xnu/files/osfmk/kern/lancelot_sched_hint.inc",
 "xnu/files/osfmk/arm64/psci_smp.c","xnu/files/bsd/dev/lancelot_msdc.h","xnu/files/bsd/dev/lancelot_emmc.c",
 "tools/mali-tests/msdc_before_latency.h","tools/mali-tests/scheduler_idle_test.c",
 "tools/lancelot-data/tests/msdc_write_test.c","tools/apply-lancelot-scheduler.py","tools/test-lancelot-scheduling-io.py"]
sources={n:sha(ROOT/n) for n in names}
for variant,header in [("before",names[5]),("current",names[3])]:
    directory=out/variant;directory.mkdir();shutil.copy2(ROOT/header,directory/"lancelot_msdc.h")
runs=[]
for cc in ["clang","gcc"]:
    jobs=[("scheduler",ROOT/names[6],["-I"+str(ROOT/"xnu/files/osfmk/kern")])]
    jobs += [(variant,ROOT/names[7],["-I"+str(out/variant)]+(["-DBEFORE_IO_LATENCY"] if variant=="before" else [])) for variant in ["before","current"]]
    for variant,source,flags in jobs:
        exe=out/(cc+"-"+variant)
        build=[cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-Wno-unused-function",
            "-fsanitize=address,undefined","-fno-sanitize-recover=all",*flags,str(source),"-o",str(exe)]
        for stage,command in [("build",build),("run",[str(exe)])]:
            log=out/(exe.name+"-"+stage+".log")
            with log.open("xb") as stream:
                run=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,timeout=90,
                    env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1","UBSAN_OPTIONS":"halt_on_error=1"})
            runs.append({"name":exe.name,"stage":stage,"exit":run.returncode,"log":log.name,"sha256":sha(log)})
            print(exe.name,stage,run.returncode,flush=True)
            if run.returncode: print(log.read_text());raise SystemExit(run.returncode)
assert all(sha(ROOT/n)==h for n,h in sources.items())
(out/"result.json").write_text(json.dumps({"passed":True,"sources":sources,"runs":runs,
    "hardware_access":False,"G3_masks_IRQ_during_card_busy_reproduced":True,
    "G4_FIFO_protected_card_busy_unmasked_and_yielding":True,
    "CPU_policy_actual_hint_exercised":True},indent=2)+"\n")
print("PASS",out,flush=True)
