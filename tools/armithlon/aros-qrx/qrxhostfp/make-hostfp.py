#!/usr/bin/env python3
"""Writes hostfp.S (and hostfp.inc, the same as C top-level asm): compiler-rt's soft float functions (and sqrt, sqrtf) for m68k64 AROS as the
68100EC64's host floating point instruction $AFnn (engine/include/qrx_ec64_hostfp.h).
Each one loads its arguments from the stack (8-byte slots at 8 and 16 from SP, a 32-bit value in the
slot's low half) into D0 and D1, runs $AFnn and returns: D0 has the result.
Only for the 68100EC64 engine with the instruction; on other engines it is an illegal instruction."""
S, D, I, L = 's', 'd', 'i', 'l'        # argument kinds: single, double, 32-bit integer, 64-bit
FUNCS = [
    # name, n, kinds of the arguments
    ('__addsf3', 0x00, 'ss'), ('__subsf3', 0x01, 'ss'), ('__mulsf3', 0x02, 'ss'), ('__divsf3', 0x03, 'ss'),
    ('__lesf2', 0x04, 'ss'), ('__ltsf2', 0x04, 'ss'), ('__eqsf2', 0x04, 'ss'), ('__nesf2', 0x04, 'ss'),
    ('__gesf2', 0x05, 'ss'), ('__gtsf2', 0x05, 'ss'), ('__unordsf2', 0x06, 'ss'),
    ('__fixsfsi', 0x07, 's'), ('__fixunssfsi', 0x08, 's'), ('__fixsfdi', 0x09, 's'), ('__fixunssfdi', 0x0A, 's'),
    ('__floatsisf', 0x0B, 'i'), ('__floatunsisf', 0x0C, 'i'), ('__floatdisf', 0x0D, 'l'), ('__floatundisf', 0x0E, 'l'),
    ('__extendsfdf2', 0x0F, 's'), ('sqrtf', 0x10, 's'),
    ('__adddf3', 0x20, 'dd'), ('__subdf3', 0x21, 'dd'), ('__muldf3', 0x22, 'dd'), ('__divdf3', 0x23, 'dd'),
    ('__ledf2', 0x24, 'dd'), ('__ltdf2', 0x24, 'dd'), ('__eqdf2', 0x24, 'dd'), ('__nedf2', 0x24, 'dd'),
    ('__gedf2', 0x25, 'dd'), ('__gtdf2', 0x25, 'dd'), ('__unorddf2', 0x26, 'dd'),
    ('__fixdfsi', 0x27, 'd'), ('__fixunsdfsi', 0x28, 'd'), ('__fixdfdi', 0x29, 'd'), ('__fixunsdfdi', 0x2A, 'd'),
    ('__floatsidf', 0x2B, 'i'), ('__floatunsidf', 0x2C, 'i'), ('__floatdidf', 0x2D, 'l'), ('__floatundidf', 0x2E, 'l'),
    ('__truncdfsf2', 0x2F, 'd'), ('sqrt', 0x30, 'd'),
]
out = ['/* written by make-hostfp.py: soft float as the 68100EC64 host floating point instruction */',
       '\t.text']
for name, n, kinds in FUNCS:
    out += ['', '\t.globl\t%s' % name, '\t.p2align\t1', '\t.type\t%s,@function' % name, '%s:' % name]
    for i, k in enumerate(kinds):
        reg = '%%d%d' % i
        slot = 8 + 8 * i
        if k in 'si':
            out.append('\tmove.l\t(%d,%%sp), %s' % (slot + 4, reg))
        else:
            out.append('\tmove.q\t(%d,%%sp), %s' % (slot, reg))
    out.append('\t.short\t0x%04X' % (0xAF00 | n))
    out.append('\trts')
    out.append('\t.size\t%s, .-%s' % (name, name))
open('hostfp.S', 'w').write('\n'.join(out) + '\n')
# the same as top-level asm for a C file (Gecko's widget/aros/aros_appshell_glue.c includes it)
inc = ['/* written by tools/armithlon/aros-qrx/qrxhostfp/make-hostfp.py: compiler-rt\'s soft float as the',
       ' * 68100EC64 host floating point instruction $AFnn (one instruction instead of 50 to 200) */',
       '__asm__(']
inc += ['    "%s\\n"' % l.replace('"', '\\"') for l in out[1:]]
inc += ['    "\\t.text\\n");']
open('hostfp.inc', 'w').write('\n'.join(inc) + '\n')
