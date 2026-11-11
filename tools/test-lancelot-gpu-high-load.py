#!/usr/bin/env python3
"""Exercise the real MT6768 controller and its explicit950MHz policy."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,subprocess,sys
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
    script='/mnt/'+R.drive[0].lower()+R.as_posix()[2:]+'/tools/test-lancelot-gpu-high-load.py'
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',script]))
paths=[R/'xnu/files/bsd/dev'/n for n in ['lancelot_mali_platform.c','lancelot_mali_platform.h','lancelot_mali_dvfs.h','lancelot_mali_trace.h','lancelot_mali_load.h','lancelot_hakios_mali.inc','lancelot_mali.c']]
paths += [R/'tools/mali-tests'/n for n in ['dvfs_model.c','high_load_test.c','high_load_queue_test.c','presenter_reader_test.cpp','cpu_reader_fairness_test.cpp','cpu_gate_before_fairness.inc','hakios_surface_handoff_test.cpp']]
paths += [R/'xnu/files/iokit/Kernel/MiniIOSurface.h',R/'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp',
          R/'xnu/files/iokit/Kernel/LancelotMaliCpuLease.h',R/'xnu/files/iokit/Kernel/lancelot_hakios_surface_handoff.inc',
          R/'xnu/files/iokit/Kernel/lancelot_hakios_client.inc',Path(__file__)]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources={p.relative_to(R).as_posix():sha(p) for p in paths}
out=R/'out/lancelot/tests'/('gpu-high-load-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
out.mkdir(parents=True,exist_ok=False)
native=(R/'xnu/files/bsd/dev/lancelot_hakios_mali.inc').read_text(encoding='utf-8')
signature='lancelot_hakios_mali_enqueue_complete(uint64_t chain,bool fragment,hkmt_completion_t completion,void *context)\n{'
begin=native.index(signature);end=native.index('\n}\n',begin)+3
getter_start=native.index('static bool hakios_queue_depth(uint32_t *depth)\n{')
getter_end=native.index('\n}\n',getter_start)+3
(out/'actual-high-load-queue.h').write_text('uint64_t\n'+native[begin:end]+'\n'+native[getter_start:getter_end]+'\n',encoding='utf-8')
surface=(R/'xnu/files/iokit/Kernel/MiniIOSurface.h').read_text(encoding='utf-8')
begin=surface.index('class LancelotMaliQueueReadGroup {');end=surface.index('\n};',begin)+4
(out/'actual-presenter-read-group.h').write_text(surface[begin:end]+'\n',encoding='utf-8')
lease=(R/'xnu/files/iokit/Kernel/LancelotMaliCpuLease.h').read_text(encoding='utf-8')
state_begin=lease.index('struct lmal_cpu_wait_state {');state_end=lease.index('\n};',state_begin)+4
(out/'actual-wait-state.h').write_text(lease[state_begin:state_end]+'\n')
methods=[]
for signature in ['int addCpuWaiter(uint64_t *ticket)','void removeCpuWaiter(uint64_t ticket)',
                  'bool gpuTurnBlocksCpu(uint64_t ticket) const','int addGpuWaiter()',
                  'void removeGpuWaiter()','bool cpuConflict(task_t own, bool legacy, bool queued = false,',
                  'int awaitGpuTurn(task_t own, bool legacy, uint64_t end, bool waitCpu,',
                  'int waitCpu(uint64_t remaining_ns)','int beginCpuRead(uint64_t remaining_ns)','void endCpuRead()',
                  'int beginCpuWrite(uint64_t remaining_ns)','void endCpuWrite()',
                  'int beginUserCpu(task_t task, bool writer, uint64_t remaining_ns)',
                  'void endUserCpu(task_t task, bool writer)']:
    begin=lease.index(signature);pos=lease.index('{',begin)+1;depth=1
    while depth:
        depth+=(lease[pos]=='{')-(lease[pos]=='}');pos+=1
    methods.append(lease[begin:pos])
for variant,code in [('before',(R/'tools/mali-tests/cpu_gate_before_fairness.inc').read_text(encoding='utf-8')),
                     ('current','\n'.join(methods)+'\n')]:
    directory=out/variant;directory.mkdir()
    (directory/'actual-cpu-gate.inc').write_text(code,encoding='utf-8')
handoff=(R/'xnu/files/iokit/Kernel/lancelot_hakios_surface_handoff.inc').read_text(encoding='utf-8')
begin=handoff.index('class HakiosSurfaceHandoff {');end=handoff.index('\n};',begin)+4
(out/'actual-hakios-surface-handoff.inc').write_text(
    handoff[handoff.index('extern "C" void lancelot_hakios_cache_sync'):end]+'\n',encoding='utf-8')
client=(R/'xnu/files/iokit/Kernel/lancelot_hakios_client.inc').read_text()
begin=client.index('class HakiosSubmitRoles {');end=client.index('\n};',begin)+4
(out/'actual-hakios-submit-roles.h').write_text(client[begin:end]+'\n')
begin=lease.index('class LancelotMaliReadGroup {');end=lease.index('\n};',begin)+4
(out/'actual-cpu-read-group.inc').write_text(lease[begin:end]+'\n',encoding='utf-8')
runs=[]
for cc in ['clang','gcc']:
    binary=out/cc
    commands=[('build',[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I'+str(R/'xnu/files/bsd/dev'),str(R/'tools/mali-tests/high_load_test.c'),
        str(R/'xnu/files/bsd/dev/lancelot_mali_platform.c'),'-o',str(binary)]),('run',[str(binary)])]
    queue=out/(cc+'-queue')
    commands += [('queue-build',[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I'+str(R/'xnu/files/bsd/dev'),'-I'+str(out),str(R/'tools/mali-tests/high_load_queue_test.c'),'-o',str(queue)]),('queue-run',[str(queue)])]
    cxx='clang++' if cc=='clang' else 'g++'
    reader=out/(cc+'-reader')
    commands += [('reader-build',[cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined',
        '-I'+str(out),str(R/'tools/mali-tests/presenter_reader_test.cpp'),'-o',str(reader)]),('reader-run',[str(reader)])]
    for variant in ['before','current']:
        binary=out/(cc+'-fairness-'+variant)
        defines=['-DCURRENT_FAIRNESS=1'] if variant=='current' else []
        commands += [('fairness-'+variant+'-build',[cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined',*defines,'-I'+str(out/variant),str(R/'tools/mali-tests/cpu_reader_fairness_test.cpp'),'-o',str(binary)]),
            ('fairness-'+variant+'-run',[str(binary)]+(['baseline'] if variant=='before' else []))]
    handoff_test=out/(cc+'-hakios-handoff')
    commands += [('hakios-handoff-build',[cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
        '-fsanitize=address,undefined','-I'+str(out),str(R/'tools/mali-tests/hakios_surface_handoff_test.cpp'),'-o',str(handoff_test)]),
        ('hakios-handoff-run',[str(handoff_test)])]
    for stage,command in commands:
        log=out/(cc+'-'+stage+'.log')
        with log.open('xb') as file:
            result=subprocess.run(command,cwd=R,stdout=file,stderr=subprocess.STDOUT,
                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        runs.append({'compiler':cc,'stage':stage,'exit':result.returncode,'log':log.name,'sha256':sha(log)})
        print(cc,stage,result.returncode,flush=True)
        if result.returncode:
            print(log.read_text(errors='replace')[-4000:],flush=True)
            raise SystemExit(result.returncode)
assert all(sha(R/n)==h for n,h in sources.items())
with (out/'result.json').open('x',encoding='utf-8') as file:
    json.dump({'passed':True,'sources':sources,'runs':runs,
        'default850_policy_and_protected_failures_retained':True,
        'explicit950_OPP_transitions':7200,'actual_controller_and_rail_steps_exercised':True,
        'live_lower_caps_and_provider_loss_still_enforced':True,'bounded_timing_trace_exercised':True,
        'actual_FIFO_producer_extracted_and_exercised':True,'normal_and_interactive_submission_windows':True,
        'FIFO_order_and_failure_while_blocked_verified':True,'actual_presenter_read_group_exercised':True,
        'writer_unblocked_only_after_explicit_reader_release':True,
        'old_CPU_wait_starvation_reproduced':True,'actual_CPU_wait_and_conflict_methods_exercised':True,
        'CPU_reader_progress_and_waiter_error_cleanup_verified':True,'existing_user_writer_dependency_retained':True,
        'GPU_waiter_blocks_only_late_CPU_readers':True,'preexisting_CPU_waiter_priority_retained':True,
        'multi_root_handoff_waits_without_refreeze':True,'partial_acquire_timeout_and_cancel_release_all_cookies':True,
        'CPU_and_GPU_use_same_gate_order_with_reversed_root_descriptors':True,
        'hardware_access':False,'GPU_950MHz_hardware_verified':False},file,indent=2)
print('PASS',out,flush=True)
