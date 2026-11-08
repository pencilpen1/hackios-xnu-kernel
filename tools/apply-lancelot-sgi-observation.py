#!/usr/bin/env python3
"""Observe the existing XNU signal callbacks without changing their dispatch."""
from pathlib import Path
import argparse
import re

MARKER='/* Lancelot SGI callback observation: dispatch remains unchanged. */'

def apply(tree):
    path=Path(tree)/'osfmk/arm/cpu_common.c'
    source=path.read_text(encoding='utf-8')
    if MARKER in source:
        if source.count(MARKER)!=1 or source.count('LSG_ENTER(')!=8 or source.count('LSG_LEAVE(')!=11:
            raise RuntimeError('Existing SGI observation hooks differ')
        return path
    def replace(old,new,count=1):
        nonlocal source
        if source.count(old)!=count:
            raise RuntimeError(f'XNU signal anchor differs: {old[:100]!r}')
        source=source.replace(old,new)
    replace('#include <libkern/OSAtomic.h>', '#include <libkern/OSAtomic.h>\n'+MARKER+
            '\n#include <arm64/lancelot_sgi_observation.h>')
    for slot,stage,signal in [('cpu_xcall_p0','LSG_XCALL','SIGPxcall'),
                             ('cpu_imm_xcall_p0','LSG_IMMEDIATE','SIGPxcallImm')]:
        anchor=f'\t\txfunc = ptrauth_auth_function(cpu_data_ptr->{slot}, ptrauth_key_function_pointer, cpu_data_ptr);'
        param=slot.replace('_p0','_p1')
        replace(anchor,anchor+f'\n\t\tLSG_ENTER(cpu_data_ptr, {stage}, {signal}, xfunc, cpu_data_ptr->{param});')
        anchor=f'\t\tos_atomic_andnot(&cpu_data_ptr->cpu_signal, {signal}, relaxed);\n\t\txfunc(xparam);\n\t\tINTERRUPT_MASKED_DEBUG_END();'
        replace(anchor,anchor+'\n\t\tLSG_LEAVE(cpu_data_ptr);')
    for callback,stage,signal,argument in [
        ('DebuggerXCall','LSG_DEBUG','SIGPdebug','cpu_data_ptr->cpu_int_state'),
        ('rtclock_intr','LSG_CLOCK','SIGPdec','0'),
        ('kperf_signal_handler','LSG_KPERF','SIGPkppet','0'),
        ('ast_check','LSG_AST','SIGPast','current_processor()')]:
        anchor=f'\t\t\tINTERRUPT_MASKED_DEBUG_START({callback}, DBG_INTR_TYPE_IPI);'
        replace(anchor,f'\t\t\tLSG_ENTER(cpu_data_ptr, {stage}, {signal}, {callback}, {argument});\n'+anchor)
        call={'DebuggerXCall':'DebuggerXCall(cpu_data_ptr->cpu_int_state);',
              'rtclock_intr':'rtclock_intr(FALSE);',
              'kperf_signal_handler':'kperf_signal_handler();',
              'ast_check':'ast_check(current_processor());'}[callback]
        anchor='\t\t\t'+call+'\n\t\t\tINTERRUPT_MASKED_DEBUG_END();'
        replace(anchor,anchor+'\n\t\t\tLSG_LEAVE(cpu_data_ptr);')
    replace('\tdata->func(data->parm);',
            '\tLSG_ENTER(getCpuDatap(), LSG_SIMPLE, 0, data->func, data->parm);\n'
            '\tdata->func(data->parm);\n\tLSG_LEAVE(getCpuDatap());')
    start=source.index('static kern_return_t\ncpu_signal_internal(')
    end=source.index('\nkern_return_t\ncpu_signal(',start)
    segment=source[start:end]
    anchor='\tcpu_data_t      *current_proc = getCpuDatap();'
    if segment.count(anchor)!=1 or segment.count('ml_set_interrupts_enabled(interruptible);')!=4:
        raise RuntimeError('XNU signal mask/return layout differs')
    segment=segment.replace(anchor,anchor+'\n\tLSG_ENTER(current_proc, LSG_SEND, signal, p0, (uintptr_t)(target_proc->cpu_number+1));')
    segment=re.sub(r'(?m)^([ \t]*)ml_set_interrupts_enabled\(interruptible\);$',
                   r'\1LSG_LEAVE(current_proc);\n\1ml_set_interrupts_enabled(interruptible);',segment)
    source=source[:start]+segment+source[end:]
    # Eight enters and eleven leaves: four exits from the same send frame.
    # The GIC adds the outer handler frame separately.
    if source.count('LSG_ENTER(')!=8 or source.count('LSG_LEAVE(')!=11:
        raise RuntimeError('Unexpected signal hook count')
    path.write_text(source,encoding='utf-8')
    return path

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tree',type=Path)
    args=parser.parse_args()
    print('Applied bounded SGI callback observation:',apply(args.tree))
