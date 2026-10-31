#!/usr/bin/env python3
"""QRX OS One 22.5.12 - differential test generator for qrx86core.

Generates a freestanding i386 program (GNU as, AT&T syntax) that executes
thousands of randomly parameterised instructions.  Each case loads all
general registers, EFLAGS and a scratch memory word, executes one instruction
and records registers, flags and memory.  The record is written to stdout via
int 0x80, so the very same static binary runs natively (x86 host with i386
support) and inside qrx-i386; compare.py then diffs both records, ignoring only
flags that the Intel SDM documents as *undefined* for that instruction.

usage: gen-diff-test.py OUT.S OUT.meta [seed] [cases-per-template]
"""
import random
import sys

CF, PF, AF, ZF, SF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800
ALL = CF | PF | AF | ZF | SF | OF

R32 = ['eax', 'ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi']
R16 = ['ax', 'cx', 'dx', 'bx', 'bp', 'si', 'di']
R8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
SUF = {8: 'b', 16: 'w', 32: 'l'}
REGS = {8: R8, 16: R16, 32: R32}


def interesting(rng, bits=32):
    m = (1 << bits) - 1
    pool = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
            0x12345678, 0xDEADBEEF, 0x55555555, 0xAAAAAAAA]
    if rng.random() < 0.4:
        return rng.choice(pool) & m
    return rng.getrandbits(bits)


class Gen:
    def __init__(self, seed, n):
        self.rng = random.Random(seed)
        self.n = n
        self.cases = []      # (asm, undefined_flag_mask, description)

    def add(self, insn, undef=0, setup=None):
        r = self.rng
        regs = {x: interesting(r) for x in R32}
        if setup:
            setup(regs)
        flags = r.getrandbits(1) * CF | r.getrandbits(1) * PF | r.getrandbits(1) * AF | \
            r.getrandbits(1) * ZF | r.getrandbits(1) * SF | r.getrandbits(1) * OF
        mem = interesting(r)
        if setup and 'mem' in regs:
            mem = regs.pop('mem')
        self.cases.append((insn, undef, regs, flags, mem))

    # -- templates ---------------------------------------------------------
    def alu(self):
        r = self.rng
        for op in ['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp', 'test']:
            for sz in (8, 16, 32):
                rs = REGS[sz]
                und = AF if op in ('or', 'and', 'xor', 'test') else 0
                for _ in range(self.n):
                    a, b = r.choice(rs), r.choice(rs)
                    self.add(f'{op}{SUF[sz]} %{a}, %{b}', und)
                    self.add(f'{op}{SUF[sz]} ${interesting(r, 8 if sz == 8 else 16) if sz != 32 else interesting(r)}, %{b}', und)
                    self.add(f'{op}{SUF[sz]} %{a}, scratch', und)
                    self.add(f'{op}{SUF[sz]} scratch, %{a}', und)
                if sz != 8:
                    for _ in range(self.n):
                        imm = r.randint(-128, 127)
                        self.add(f'{op}{SUF[sz]} ${imm}, %{r.choice(rs)}', und)

    def unary(self):
        r = self.rng
        for op in ['inc', 'dec', 'neg', 'not']:
            for sz in (8, 16, 32):
                for _ in range(self.n):
                    self.add(f'{op}{SUF[sz]} %{r.choice(REGS[sz])}')
                    self.add(f'{op}{SUF[sz]} scratch')

    def shifts(self):
        r = self.rng
        for op in ['rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sar']:
            for sz in (8, 16, 32):
                for _ in range(self.n):
                    cnt = r.choice([0, 1, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, r.randint(0, 255)])
                    m = cnt & 31
                    und = 0
                    if m > 1:
                        und |= OF
                    if op in ('shl', 'shr', 'sar') and m:
                        und |= AF
                    if op in ('shl', 'shr') and m > sz:
                        und |= CF | OF
                    if op in ('rcl', 'rcr') and sz < 32 and m > 1:
                        und |= OF
                    if op in ('rol', 'ror') and m > 1:
                        und |= OF
                    reg = r.choice(REGS[sz])
                    self.add(f'{op}{SUF[sz]} ${cnt}, %{reg}', und)
                    if reg not in ('cl', 'cx', 'ecx', 'ch'):
                        def st(regs, c=cnt):
                            regs['ecx'] = (regs['ecx'] & ~0xFF) | c
                        self.add(f'{op}{SUF[sz]} %cl, %{reg}', und, st)
                    self.add(f'{op}{SUF[sz]} $1, scratch', 0 if op not in ('shl', 'shr', 'sar') else AF)

    def double_shift(self):
        r = self.rng
        for op in ['shld', 'shrd']:
            for sz in (16, 32):
                for _ in range(self.n):
                    cnt = r.randint(0, 31)
                    if sz == 16:
                        cnt = r.randint(0, 16)
                    und = AF | (OF if cnt != 1 else 0)
                    a, b = r.choice(REGS[sz]), r.choice(REGS[sz])
                    self.add(f'{op}{SUF[sz]} ${cnt}, %{a}, %{b}', und if cnt else 0)

    def muldiv(self):
        r = self.rng
        for sz in (8, 16, 32):
            for _ in range(self.n):
                src = r.choice([x for x in REGS[sz] if x not in ('al', 'ah', 'ax', 'eax', 'dx', 'edx')])
                self.add(f'mul{SUF[sz]} %{src}', SF | ZF | AF | PF)
                self.add(f'imul{SUF[sz]} %{src}', SF | ZF | AF | PF)
                self.add(f'mul{SUF[sz]} scratch', SF | ZF | AF | PF)
        for sz in (16, 32):
            for _ in range(self.n):
                a, b = r.choice(REGS[sz]), r.choice(REGS[sz])
                self.add(f'imul{SUF[sz]} %{a}, %{b}', SF | ZF | AF | PF)
                self.add(f'imul{SUF[sz]} ${r.randint(-30000, 30000)}, %{a}, %{b}', SF | ZF | AF | PF)
                self.add(f'imul{SUF[sz]} ${r.randint(-128, 127)}, scratch, %{b}', SF | ZF | AF | PF)
        # division: operands chosen so that no #DE occurs
        for sz in (8, 16, 32):
            mask = (1 << sz) - 1
            for _ in range(self.n):
                for signed in (False, True):
                    d = r.randint(1, mask)
                    if signed and r.random() < 0.5:
                        d = (-r.randint(1, mask >> 1)) & mask
                    dv = d if not signed or d < (1 << (sz - 1)) else d - (1 << sz)
                    q = r.randint(0, (mask >> 1) if signed else mask)
                    if signed and r.random() < 0.5:
                        q = -q
                    rem = r.randint(0, abs(dv) - 1)
                    n = q * dv + (rem if q >= 0 else -rem)
                    if signed:
                        lim = 1 << (sz - 1)
                        if not (-lim <= n // dv if n * dv >= 0 else True):
                            pass
                    # verify quotient fits
                    if signed:
                        qq = int(n / dv) if dv else 0
                        if not (-(1 << (sz - 1)) <= qq < (1 << (sz - 1))):
                            continue
                    else:
                        if n < 0 or n // dv > mask:
                            continue
                    nn = n & ((1 << (2 * sz)) - 1)

                    def st(regs, nn=nn, d=d, sz=sz):
                        if sz == 8:
                            regs['eax'] = (regs['eax'] & ~0xFFFF) | nn
                            regs['ebx'] = (regs['ebx'] & ~0xFF) | d
                        elif sz == 16:
                            regs['eax'] = (regs['eax'] & ~0xFFFF) | (nn & 0xFFFF)
                            regs['edx'] = (regs['edx'] & ~0xFFFF) | (nn >> 16)
                            regs['ebx'] = (regs['ebx'] & ~0xFFFF) | d
                        else:
                            regs['eax'] = nn & 0xFFFFFFFF
                            regs['edx'] = nn >> 32
                            regs['ebx'] = d
                    src = {8: 'bl', 16: 'bx', 32: 'ebx'}[sz]
                    self.add(f'{"idiv" if signed else "div"}{SUF[sz]} %{src}', ALL, st)

    def bits(self):
        r = self.rng
        for op in ['bt', 'bts', 'btr', 'btc']:
            for sz in (16, 32):
                for _ in range(self.n):
                    a, b = r.choice(REGS[sz]), r.choice(REGS[sz])
                    self.add(f'{op}{SUF[sz]} %{a}, %{b}', OF | SF | AF | PF | ZF)
                    self.add(f'{op}{SUF[sz]} ${r.randint(0, 255)}, %{b}', OF | SF | AF | PF | ZF)
                    self.add(f'{op}{SUF[sz]} ${r.randint(0, sz - 1)}, scratch', OF | SF | AF | PF | ZF)

                    def st(regs):
                        regs['ecx'] = self.rng.randint(0, 31)
                    self.add(f'{op}l %ecx, scratch', OF | SF | AF | PF | ZF, st)
        for op in ['bsf', 'bsr']:
            for sz in (16, 32):
                for _ in range(self.n):
                    a, b = r.choice(REGS[sz]), r.choice(REGS[sz])

                    def st(regs, a=a, sz=sz):
                        full = R32[REGS[sz].index(a)]
                        if regs[full] & ((1 << sz) - 1) == 0:
                            regs[full] |= 1 << self.rng.randint(0, sz - 1)
                    self.add(f'{op}{SUF[sz]} %{a}, %{b}', CF | OF | SF | AF | PF, st)

    def misc(self):
        r = self.rng
        conds = ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']
        for cc in conds:
            for _ in range(self.n):
                self.add(f'set{cc} %{r.choice(R8)}')
                self.add(f'cmov{cc}l %{r.choice(R32)}, %{r.choice(R32)}')
                self.add(f'cmov{cc}w scratch, %{r.choice(R16)}')
        for _ in range(self.n):
            self.add(f'bswap %{r.choice(R32)}')
            for sz in (8, 16, 32):
                a, b = r.choice(REGS[sz]), r.choice(REGS[sz])
                self.add(f'xadd{SUF[sz]} %{a}, %{b}')
                self.add(f'xadd{SUF[sz]} %{a}, scratch')
                self.add(f'cmpxchg{SUF[sz]} %{a}, %{b}')
                self.add(f'cmpxchg{SUF[sz]} %{a}, scratch')
                self.add(f'xchg{SUF[sz]} %{a}, %{b}')
            self.add(f'movsbl %{r.choice(R8)}, %{r.choice(R32)}')
            self.add(f'movswl %{r.choice(R16)}, %{r.choice(R32)}')
            self.add(f'movzbl %{r.choice(R8)}, %{r.choice(R32)}')
            self.add(f'movzwl scratch, %{r.choice(R32)}')
            self.add(f'movsbw %{r.choice(R8)}, %{r.choice(R16)}')
            for x in ['cbtw', 'cwtl', 'cltd', 'cwtd', 'sahf', 'lahf', 'cmc', 'clc', 'stc']:
                self.add(x)
            self.add('daa', OF)
            self.add('das', OF)
            self.add('aaa', OF | SF | ZF | PF)
            self.add('aas', OF | SF | ZF | PF)
            self.add(f'aam ${r.randint(1, 255)}', OF | AF | CF)
            self.add(f'aad ${r.randint(0, 255)}', OF | AF | CF)
            self.add(f'leal {r.randint(-5000, 5000)}(%{r.choice(R32)},%{r.choice([x for x in R32 if x != "esp"])},{r.choice([1, 2, 4, 8])}), %{r.choice(R32)}')
            self.add(f'leaw {r.randint(-500, 500)}(%{r.choice(R32)}), %{r.choice(R16)}')

    # -- output ------------------------------------------------------------
    def emit(self, path_s, path_meta):
        out = []
        out.append('    .text\n    .globl _start\n_start:\n')
        for i, (insn, und, regs, flags, mem) in enumerate(self.cases):
            out.append(f'    movl ${mem:#x}, scratch\n')
            for x in R32:
                out.append(f'    movl ${regs[x]:#x}, %{x}\n')
            out.append(f'    pushl ${flags | 0x202:#x}\n    popfl\n')
            out.append(f'    {insn}\n')
            out.append('    pushfl\n')
            base = i * 40
            for k, x in enumerate(R32):
                out.append(f'    movl %{x}, rec+{base + k * 4}\n')
            out.append(f'    popl %eax\n    movl %eax, rec+{base + 28}\n')
            out.append(f'    movl scratch, %eax\n    movl %eax, rec+{base + 32}\n')
        n = len(self.cases)
        out.append(f'''    movl $4, %eax
    movl $1, %ebx
    movl $rec, %ecx
    movl ${n * 40}, %edx
    int $0x80
    movl $1, %eax
    xorl %ebx, %ebx
    int $0x80
    .bss
    .align 16
scratch: .space 16
rec: .space {n * 40}
''')
        open(path_s, 'w').write(''.join(out))
        with open(path_meta, 'w') as m:
            for insn, und, *_ in self.cases:
                m.write(f'{und:#x}\t{insn}\n')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 22512
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    g = Gen(seed, n)
    for t in (g.alu, g.unary, g.shifts, g.double_shift, g.muldiv, g.bits, g.misc):
        t()
    g.emit(sys.argv[1], sys.argv[2])
    print(f'generated {len(g.cases)} cases (seed {seed})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
