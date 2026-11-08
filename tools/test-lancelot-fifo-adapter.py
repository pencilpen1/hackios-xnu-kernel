"""Exercise the exact TX adapter and inspect its ARM64 barrier placement."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib, json, os, re, subprocess, sys

P = Path(__file__).resolve().parent
R = P.parent
if os.name == 'nt':
    script = '/mnt/' + P.drive[0].lower() + P.as_posix()[2:] + '/' + Path(__file__).name
    raise SystemExit(subprocess.call(['wsl', '-d', 'Ubuntu', '--exec', 'python3', '-B', script]))
E = R / 'out/lancelot/tests'
E.mkdir(parents=True, exist_ok=True)
O = E / ('fifo-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
O.mkdir()
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
source = R / 'xnu/files/bsd/dev/lancelot_emmc.c'
sources = {source.relative_to(R).as_posix(): sha(source),
           Path(__file__).relative_to(R).as_posix(): sha(Path(__file__))}
original = source.read_text(encoding='utf-8')

def function(name):
    match = re.search(r'^static void\n' + re.escape(name) + r'\(', original, re.M)
    assert match, name
    brace = original.index('{', match.start())
    depth = 0
    for end in range(brace, len(original)):
        depth += (original[end] == '{') - (original[end] == '}')
        if depth == 0:
            return original[match.start():end + 1]
    raise AssertionError(name)

tx = function('mmc_fifo_write_words')
control = function('mmc_write_register')
barrier = '__asm__ volatile ("dsb sy" ::: "memory");'
assert tx.count('*fifo=value;') == 1 and tx.count(barrier) == 1
assert control.count('*(volatile uint32_t *)(registers + offset) = value;') == 1
assert control.count(barrier) == 1
host_tx = tx.replace('*fifo=value;', 'model_fifo_write(fifo,value);').replace(barrier, 'model_barrier();')
host_control = control.replace('*(volatile uint32_t *)(registers + offset) = value;',
                              'model_control_write(offset,value);').replace(barrier, 'model_barrier();')
host_prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __unused __attribute__((unused))
#define LM_TX 0x18u
#define LM_CMD 0x34u
#define os_atomic_store(p,v,order) __atomic_store_n((p),(v),__ATOMIC_RELAXED)
static struct {uint32_t command;} mmc_observer;
static uint8_t mmio[512] __attribute__((aligned(4)));
static uintptr_t registers;
static uint32_t observed[32], control_offset, control_value;
static unsigned writes, barriers, expected_writes;
static void model_fifo_write(volatile uint32_t *fifo,uint32_t value) {
 assert(fifo==(volatile uint32_t *)(mmio+LM_TX));
 assert(writes<expected_writes&&writes<32&&!barriers);
 observed[writes++]=value;
}
static void model_control_write(uint32_t offset,uint32_t value) {
 assert(!barriers&&!writes);control_offset=offset;control_value=value;writes++;
}
static void model_barrier(void) {assert(writes==expected_writes&&!barriers);barriers++;}
'''
host_suffix = r'''
int main(void) {
 registers=(uintptr_t)mmio;unsigned cases=0;
 for(unsigned alignment=0;alignment<8;alignment++)for(unsigned words=0;words<=32;words++) {
  size_t n=(size_t)words*4+alignment;
  uint8_t *allocation=malloc(n?n:1),*before=malloc(n?n:1);assert(allocation&&before);
  for(size_t i=0;i<n;i++)allocation[i]=(uint8_t)(i*67+words*13+alignment);
  memcpy(before,allocation,n);writes=barriers=0;expected_writes=words;
  mmc_fifo_write_words(NULL,allocation+alignment,words);
  assert(writes==words&&barriers==1&&!memcmp(allocation,before,n));
  for(unsigned i=0;i<words;i++) {
   const uint8_t *p=before+alignment+i*4;
   uint32_t expected=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
   assert(observed[i]==expected);
  }
  free(before);free(allocation);cases++;
 }
 for(uint32_t offset=0;offset<512;offset+=4) {
  writes=barriers=0;expected_writes=1;
  uint32_t prior_command=mmc_observer.command;
  uint32_t value=0x89abcdefu^offset;mmc_write_register(NULL,offset,value);
  assert(writes==1&&barriers==1&&control_offset==offset&&control_value==value);
  assert(mmc_observer.command==(offset==LM_CMD?value:prior_command));
 }
 printf("PASS extracted production TX adapter: %u aligned/unaligned/empty bursts, LE word values, exact reads, one barrier after each bounded burst; 128 control writes each kept their barrier\n",cases);
 return 0;
}
'''
unit = O / 'adapter-host.c'
unit.write_text(host_prefix + host_tx + '\n' + host_control + '\n' + host_suffix, encoding='utf-8')
arm = O / 'adapter-arm64.c'
arm.write_text('typedef __UINT8_TYPE__ uint8_t;\ntypedef __UINT32_TYPE__ uint32_t;\n'
               'typedef __UINTPTR_TYPE__ uintptr_t;\n#define __unused __attribute__((unused))\n'
               '#define LM_TX 0x18u\n#define LM_CMD 0x34u\n'
               '#define os_atomic_store(p,v,order) __atomic_store_n((p),(v),__ATOMIC_RELAXED)\n'
               'static struct {uint32_t command;} mmc_observer;\nuintptr_t registers;\n' + tx + '\n' + control + '\n'
               '__attribute__((noinline,used)) void actual_fifo(const uint8_t *p,unsigned words)'
               '{mmc_fifo_write_words((void *)0,p,words);}\n'
               '__attribute__((noinline,used)) void actual_control(uint32_t offset,uint32_t value)'
               '{mmc_write_register((void *)0,offset,value);}\n', encoding='utf-8')
runs = []

def run(label, command):
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=90)
    log = O / (label + '.log')
    log.write_bytes(result.stdout + result.stderr)
    runs.append({'name': label, 'exit': result.returncode, 'log': log.name, 'sha256': sha(log)})
    print(label, result.returncode, (result.stdout + result.stderr).decode(errors='replace')[-1800:], flush=True)
    return result.returncode == 0

passed = True
for cc in ['gcc', 'clang']:
    exe = O / cc
    if not run(cc + '-build', [cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                              '-fsanitize=address,undefined', unit, '-o', exe]):
        passed = False
        break
    if not run(cc + '-run', [exe]):
        passed = False
        break
assembly = O / 'adapter-arm64.s'
arm_passed = passed and run('arm64-strict-align', ['clang', '--target=aarch64-none-elf', '-mstrict-align',
                                                '-ffreestanding', '-O2', '-S', arm, '-o', assembly])
if arm_passed:
    asm = assembly.read_text(encoding='utf-8')
    for name in ['actual_fifo', 'actual_control']:
        section = asm[asm.index(name + ':'):asm.index('.size\t' + name)]
        barriers_found = list(re.finditer(r'\bdsb\s+sy\b', section))
        stores = list(re.finditer(r'\bstr\s+w\d+,', section))
        assert len(barriers_found) == 1 and stores and barriers_found[0].start() > stores[-1].start()
        assert not re.search(r'\bbl\s+.*memcpy', section)
    print('ARM64 strict alignment: native TX stores precede one trailing dsb sy; control store retains dsb sy', flush=True)
assert all(sha(R / name) == h for name, h in sources.items())
result = {'passed': passed and arm_passed, 'sources': sources, 'runs': runs,
          'actual_production_adapter_extracted': True, 'MMIO_ARM_barrier_and_observer_object_modeled': True,
          'alignments_tested': 8, 'burst_word_counts_tested': 33, 'maximum_burst_words': 32,
          'ARM64_strict_alignment_compiled': bool(arm_passed), 'device_access': False,
          'hardware_FIFO_ordering_verified': False, 'GPU_complete': False}
with (O / 'result.json').open('x', encoding='utf-8') as f:
    json.dump(result, f, indent=2)
print('FIFO ADAPTER TESTS', O, flush=True)
raise SystemExit(0 if result['passed'] else 1)
