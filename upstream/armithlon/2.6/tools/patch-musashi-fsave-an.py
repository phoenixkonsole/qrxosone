#!/usr/bin/env python3
"""Copy pinned Musashi CPU sources to the build tree with one surgical fix.

  python3 tools/patch-musashi-fsave-an.py <musashi-src-dir> <dest-dir>

The vendor pin (fetch-musashi.sh) must stay pristine, so the fix lives on a
build-tree copy: m68kcpu.c verbatim, m68kfpu.c with FSAVE-(An) implemented.

Real 68040 `FSAVE (An)` stores the IDLE frame without adjusting An; upstream
Musashi only handles (An)+ / -(An) and calls fatalerror() (exit(1)) for (An).
AROS m68k-amiga cpu_detect.S probes the FPU with exactly `fsave (%a0)`, so an
unpatched Musashi can never boot this ROM. The inserted case mirrors the
adjacent (An)+ handler minus the An adjustment, writing the standard IDLE
frame (version 0x1f) via perform_fsave().

The AROS FPU context-switch helpers also use `fsave/frestore (d16,An)`
(mode 5, e.g. `fsave ($6c,%a0)` in the task-switch save path), which hits the
same fatalerror once the boot reaches the first task switch. The mode-5 cases
mirror the (An) handlers with the address from EA_AY_DI_32().

Idempotent: the copy is regenerated from the pristine source every run and
the anchor must match exactly once, otherwise the script refuses (pin drift).
"""
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('Usage: patch-musashi-fsave-an.py <musashi-src-dir> <dest-dir>')
src = Path(sys.argv[1]).resolve()
dst = Path(sys.argv[2]).resolve()
fpu = (src / 'm68kfpu.c').read_text()
cpu = (src / 'm68kcpu.c').read_text()

anchor = ('\t\tcase 0:\t\t// FSAVE <ea>\n\t\t{\n\t\t\tswitch (mode)\n'
          '\t\t\t{\n\t\t\t\tcase 3:\t// (An)+')
assert fpu.count(anchor) == 1, 'Refuse patch: FSAVE switch anchor changed (pin drift?)'
assert 'ARMITHLON_FSAVE_AN' not in fpu, 'Refuse patch: vendor source already modified'

fix = ('\t\t\t\tcase 2: // (An)\n'
       '\t\t\t\t\t/* ARMITHLON_FSAVE_AN: real 68040 FSAVE (An) stores the\n'
       '\t\t\t\t\t * IDLE frame without adjusting An. Upstream Musashi\n'
       '\t\t\t\t\t * only implements (An)+ / -(An) here. */\n'
       '\t\t\t\t\taddr = REG_A[reg];\n'
       '\t\t\t\t\tif (m68ki_cpu.fpu_just_reset)\n'
       '\t\t\t\t\t{\n'
       '\t\t\t\t\t\tm68ki_write_32(addr, 0);\n'
       '\t\t\t\t\t}\n'
       '\t\t\t\t\telse\n'
       '\t\t\t\t\t{\n'
       '\t\t\t\t\t\t// IDLE frame, no An adjustment\n'
       '\t\t\t\t\t\tperform_fsave(addr, 1);\n'
       '\t\t\t\t\t}\n'
       '\t\t\t\t\tbreak;\n\n'
       '\t\t\t\tcase 5:\t// (d16,An)\n'
       '\t\t\t\t\t/* ARMITHLON_FSAVE_D16AN: same IDLE frame at An+d16.\n'
       '\t\t\t\t\t * AROS task-switch save path uses fsave (d16,An). */\n'
       '\t\t\t\t\taddr = EA_AY_DI_32();\n'
       '\t\t\t\t\tif (m68ki_cpu.fpu_just_reset)\n'
       '\t\t\t\t\t{\n'
       '\t\t\t\t\t\tm68ki_write_32(addr, 0);\n'
       '\t\t\t\t\t}\n'
       '\t\t\t\t\telse\n'
       '\t\t\t\t\t{\n'
       '\t\t\t\t\t\tperform_fsave(addr, 1);\n'
       '\t\t\t\t\t}\n'
       '\t\t\t\t\tbreak;\n\n')
tail = '\t\t\t\tcase 3:\t// (An)+'
assert anchor.endswith(tail)
fpu = fpu.replace(anchor, anchor[:-len(tail)] + fix + tail)

restore_anchor = ('\t\t\t\t\t\tdo_frestore_null();\n\t\t\t\t\t}\n\t\t\t\t\tbreak;\n\n'
                    '\t\t\t\tcase 3:\t// (An)+')
assert fpu.count(restore_anchor) == 1, 'Refuse patch: FRESTORE switch anchor changed (pin drift?)'
assert 'ARMITHLON_FRESTORE_D16AN' not in fpu, 'Refuse patch: vendor source already modified'
restore_fix = ('\t\t\t\tcase 5:\t// (d16,An)\n'
               '\t\t\t\t\t/* ARMITHLON_FRESTORE_D16AN: mirror of (An) at\n'
               '\t\t\t\t\t * An+d16; AROS restore path uses frestore (d16,An). */\n'
               '\t\t\t\t\taddr = EA_AY_DI_32();\n'
               '\t\t\t\t\ttemp = m68ki_read_32(addr);\n\n'
               '\t\t\t\t\t// check for NULL frame\n'
               '\t\t\t\t\tif (temp & 0xff000000)\n'
               '\t\t\t\t\t{\n'
               '\t\t\t\t\t\t// non-NULL frame, no An adjustment to do here\n'
               '\t\t\t\t\t\tm68ki_cpu.fpu_just_reset = 0;\n'
               '\t\t\t\t\t}\n'
               '\t\t\t\t\telse\n'
               '\t\t\t\t\t{\n'
               '\t\t\t\t\t\tdo_frestore_null();\n'
               '\t\t\t\t\t}\n'
               '\t\t\t\t\tbreak;\n\n')
restore_tail = '\t\t\t\tcase 3:\t// (An)+'
assert restore_anchor.endswith(restore_tail)
fpu = fpu.replace(restore_anchor,
                  restore_anchor[:-len(restore_tail)] + restore_fix + restore_tail)

# TAS and CAS must stay atomic once a second core runs. They go through hooks
# that the board overrides for RAM; the weak defaults keep plain Musashi behaviour.
cpu += '''
__attribute__((weak)) int qrx_atomic_cas(unsigned width, unsigned ea, unsigned cmp, unsigned val, unsigned *old)
{ (void)width; (void)ea; (void)cmp; (void)val; (void)old; return 0; }
__attribute__((weak)) int qrx_atomic_tas(unsigned ea, unsigned *old)
{ (void)ea; (void)old; return 0; }
'''
ops = (src / 'm68k_in.c').read_text(encoding='latin-1')
def sub_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'patch anchor not unique: {old!r}')
    return text.replace(old, new)
ops = sub_once(ops, 'M68KMAKE_OPCODE_HANDLER_HEADER\n\n#include <stdio.h>\n#include "m68kcpu.h"\n',
               'M68KMAKE_OPCODE_HANDLER_HEADER\n\n#include <stdio.h>\n#include "m68kcpu.h"\n'
               'extern int qrx_atomic_cas(unsigned width, unsigned ea, unsigned cmp, unsigned val, unsigned *old);\n'
               'extern int qrx_atomic_tas(unsigned ea, unsigned *old);\n')
for bits, mask in ((8, 'MASK_OUT_ABOVE_8(*compare)'), (16, 'MASK_OUT_ABOVE_16(*compare)'), (32, '*compare')):
    size = bits // 8
    value = f'MASK_OUT_ABOVE_{bits}(REG_D[(word2 >> 6) & 7])' if bits != 32 else 'REG_D[(word2 >> 6) & 7]'
    ops = sub_once(ops,
        f'\t\tuint ea = M68KMAKE_GET_EA_AY_{bits};\n\t\tuint dest = m68ki_read_{bits}(ea);\n\t\tuint* compare = &REG_D[word2 & 7];\n',
        f'\t\tuint ea = M68KMAKE_GET_EA_AY_{bits};\n\t\tuint dest;\n\t\tuint* compare = &REG_D[word2 & 7];\n'
        f'\t\tint atomic_done = qrx_atomic_cas({size}, ea, {mask}, {value}, &dest);\n'
        f'\t\tif(!atomic_done) dest = m68ki_read_{bits}(ea);\n')
    ops = sub_once(ops,
        f'\t\t\tm68ki_write_{bits}(ea, {value});\n\t\t}}\n\t\treturn;',
        f'\t\t\tif(!atomic_done) m68ki_write_{bits}(ea, {value});\n\t\t}}\n\t\treturn;')
ops = sub_once(ops, '\tuint dst = m68ki_read_8(ea);\n\tuint allow_writeback;\n',
               '\tuint dst;\n\tuint allow_writeback;\n\tint atomic_done = qrx_atomic_tas(ea, &dst);\n\tif(!atomic_done) dst = m68ki_read_8(ea);\n')
ops = sub_once(ops, '\tif (allow_writeback==1) m68ki_write_8(ea, dst | 0x80);\n',
               '\tif (allow_writeback==1 && !atomic_done) m68ki_write_8(ea, dst | 0x80);\n')
dst.mkdir(parents=True, exist_ok=True)
(dst / 'm68k_in.c').write_text(ops, encoding='latin-1')
(dst / 'm68kcpu.c').write_text(cpu)
(dst / 'm68kfpu.c').write_text(fpu)
print(f'Musashi FSAVE-(An) build-tree copy ready: {dst}')
