#!/usr/bin/env python3
"""Compile generated bootstrap admission with every registered real root and negatives."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,os,subprocess

REPO=Path(__file__).resolve().parent.parent
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True,help='NEW persistent evidence directory')
    args=ap.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('pins',REPO/'tools/loader-lancelot/apply-root-variants.py')
    pins=importlib.util.module_from_spec(spec);spec.loader.exec_module(pins)
    original=REPO/'xnuloader/lancelot_ubuntu.h';before=original.read_bytes();text=original.read_text()
    registry=json.loads((REPO/'tools/loader-lancelot/root-variants.json').read_text())
    generated=pins.render(text,registry)
    assert pins.render(generated,registry)==generated,'Root generation must be idempotent'
    (out/'generated-lancelot-ubuntu.h').write_text(generated)
    def function(header):
        first=header.index('\tstruct lancelot_ramdisk_variant {')
        loop=header.index('\tfor (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {',first)
        end=header.index('\n\tmemset(end, 0, sizeof(struct payload_entry));',loop)
        # Brace-balanced slice bounded by the unique loop and next statement.
        body=header[header.index('\n',loop)+1:end]
        assert body.endswith('\t}')
        return ('static int verify_slot(unsigned i) {\nstruct ext4_fs fs={0};\n'
                'struct payload_header *header=(void*)payload;\nuint8_t *end=payload+sizeof(*header);\n'+
                header[first:loop]+body[:-2]+'\nreturn 1;\n}\n')
    (out/'actual-bootstrap-slot.h').write_text(function(generated))
    old=REPO/'tools/loader-lancelot/tests/legacy-root-collision.h'
    assert old.exists();old_header=old.read_text();assert 'found = candidate' not in old_header
    (out/'old').mkdir();(out/'old/actual-bootstrap-slot.h').write_text(function(old_header))
    # Reverse equal-size candidate order and retest the production algorithm.
    reverse=json.loads(json.dumps(registry));reverse['additional'].reverse()
    (out/'reverse').mkdir();(out/'reverse/actual-bootstrap-slot.h').write_text(function(pins.render(text,reverse)))
    roots=[(REPO/'local/lancelot-gpu-20261026/private-dma/attempt40/root-40.apfs','537d98abb1a73234e62068f1e18dcc8ceffd3e5f0e55cb439423b4a1b9bc8536')]
    roots += [(REPO/r['image'],r['sha256']) for r in registry['additional']]
    for path,want in roots:assert hashlib.sha256(path.read_bytes()).hexdigest()==want,path
    root49_sha='0fcc4ff64fb44cc1caff0c26fd073c12ddb25c27ef6284719cd32abac8dbd901'
    assert any(a[0].stat().st_size==b[0].stat().st_size and a[1]!=b[1]
               for i,a in enumerate(roots) for b in roots[i+1:]),'Must retain real equal-size collision coverage'
    assert any(want==root49_sha for _,want in roots)
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    fixture=REPO/'tools/loader-lancelot/bootstrap_slot_test.c'
    commands=[]
    with (out/'tests.log').open('x') as log:
        for cc in ['clang','cc']:
            for variant,include in [('fixed',out),('old',out/'old'),('reverse',out/'reverse')]:
                binary=out/(cc+'-'+variant)
                command=[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                         '-I'+str(include),'-I'+str(REPO/'xnuloader'),str(fixture),'-o',str(binary)]
                commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,env=env)
                for path,want in roots:
                    # Historical broken header only exercises its original pins.
                    # Newer images are tested against fixed+reverse generated
                    # admission, not misclassified as the old same-size defect.
                    if variant=='old' and want not in old_header:continue
                    expect=0 if variant=='old' and want==root49_sha else 1
                    command=[str(binary),str(path),want,str(expect)]
                    log.write('$ '+subprocess.list2cmdline(command)+'\n');log.flush();commands.append(command)
                    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,env=env)
    assert original.read_bytes()==before
    with (out/'result.json').open('x') as result:
        json.dump({'checks':{'old_loader_rejects_equal_size_root49_reproduced':True,'fixed_accepts_root40_root44_root49':True,'fixed_accepts_all_registered_roots':True,
                   'reverse_candidate_order_passes':True,'corrupt_digest_bad_APFS_wrong_size_read_failure_missing_rejected':True,
                   'real_production_SHA_and_generated_loader_admission':True,'original_user_header_unchanged':True},
                   'generated_header_sha256':hashlib.sha256(generated.encode()).hexdigest(),
                   'commands':commands,'roots':{str(p):h for p,h in roots}},result,indent=2);result.write('\n')
    (out/'tests.exit').write_text('0\n')
    print(f'PASS GCC/Clang ASan/UBSan: reproduced old root49 rejection; actual generated admission accepts all{len(roots)} roots in both orders, rejects corruption/size/APFS/I/O/slot mismatches; original header untouched.')

if __name__=='__main__':main()
