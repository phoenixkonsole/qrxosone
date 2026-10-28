#!/usr/bin/env python3
"""QRX OS One 22.5.11 - host regression tests for the portable HUNK/launcher core.

HOST-ONLY: this compiles src/qrx_hunk.c with the host GCC and feeds it synthetic
and (optionally) real files.  It proves parser/quoting/return-code logic, NOT that
the Deadwood m68k build or AROS LoadSeg behave the same; that is covered by the
FS-UAE target run (emulator/run-target-tests.sh).

Usage: test-amiga-launcher-host.py [extra-real-hunk-dir ...]
"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
H = struct.Struct('>I')

def L(*v):
    return b''.join(H.pack(x & 0xffffffff) for x in v)

def hunk_file(hunks, header_sizes=None, table=None, first=0, extra=b'', resident=b''):
    """hunks: list of (type, payload_bytes|bss_len, relocs{target:[offsets]}, flags)."""
    sizes = header_sizes or []
    if not sizes:
        for t, p, _, fl in hunks:
            n = (len(p) + 3) // 4 if t != 0x3EB else p
            sizes.append(n | (fl << 30))
    tab = table if table is not None else len(sizes)
    out = L(0x3F3) + resident + L(0, tab, first, first + len(sizes) - 1) + L(*sizes)
    for t, p, rel, fl in hunks:
        if t == 0x3EB:
            out += L(t, p)
        else:
            p = p + b'\0' * (-len(p) % 4)
            out += L(t, len(p) // 4) + p
        if rel:
            out += L(0x3EC)
            for tgt, offs in rel.items():
                out += L(len(offs), tgt, *offs)
            out += L(0)
        out += L(0x3F2)
    return out + extra

# 68k: moveq #0,d0 ; rts  + a string table referencing libraries
CODE = bytes.fromhex('70004e75') + b'\0\0\0\0'
LIBSTR = b'dos.library\0intuition.library\0qrxnonexistent.library\0timer.device\0LIBS:gadtools.library\0not..library\0'

def cases():
    c = {}
    c['minimal'] = hunk_file([(0x3E9, CODE, None, 0)])
    c['code_data_bss'] = hunk_file([(0x3E9, CODE, {1: [4]}, 0), (0x3EA, LIBSTR, None, 0), (0x3EB, 64, None, 0)])
    c['chip'] = hunk_file([(0x3E9, CODE, None, 0), (0x3EA, b'\0' * 16, None, 1)])
    # RELOC32SHORT (0x3FC) with odd word count -> needs padding: cnt=1,tgt=0,off=4,end=0 -> 4 words -> no pad;
    # cnt=2 -> 5 words -> pad
    body = hunk_file([(0x3E9, CODE + b'\0' * 8, None, 0)])
    short = struct.pack('>HHHHH', 2, 0, 4, 8, 0) + b'\0\0'
    c['reloc32short'] = body[:-4] + L(0x3FC) + short + L(0x3F2)
    c['symbols_debug'] = body[:-4] + L(0x3F0, 2) + b'main\0\0\0\0' + L(0, 0) + L(0x3F1, 1, 0xDEADBEEF) + L(0x3F2)
    c['truncated_header'] = L(0x3F3)
    c['truncated_payload'] = hunk_file([(0x3E9, CODE, None, 0)])[:-8]
    c['missing_hunk'] = hunk_file([(0x3E9, CODE, None, 0)], header_sizes=[2, 4])
    c['reloc_oob'] = hunk_file([(0x3E9, CODE, {0: [100]}, 0)])
    c['reloc_bad_target'] = hunk_file([(0x3E9, CODE, {7: [0]}, 0)])
    c['payload_too_big'] = hunk_file([(0x3E9, CODE * 4, None, 0)], header_sizes=[1])
    c['unknown_hunk'] = hunk_file([(0x3E9, CODE, None, 0)])[:-4] + L(0x3AA, 0, 0x3F2)
    c['resident_names'] = hunk_file([(0x3E9, CODE, None, 0)], resident=L(1) + b'abc\0')
    c['huge_table'] = L(0x3F3, 0, 100000, 0, 99999)
    c['object_unit'] = L(0x3E7, 1) + b'obj\0'
    c['hunk_lib'] = L(0x3FA, 0)
    c['ext_in_exe'] = hunk_file([(0x3E9, CODE, None, 0)])[:-4] + L(0x3EF, 0, 0x3F2)
    c['overlay'] = hunk_file([(0x3E9, CODE, None, 0)]) + L(0x3F5, 0, 0)
    # ELF headers (52 bytes min)
    def elf(machine, etype, be=True):
        e = bytearray(64)
        e[0:4] = b'\x7fELF'; e[4] = 1; e[5] = 2 if be else 1; e[6] = 1
        pack = '>HH' if be else '<HH'
        e[16:20] = struct.pack(pack, etype, machine)
        return bytes(e)
    c['elf_aros_m68k'] = elf(4, 1)
    c['elf_linux_m68k'] = elf(4, 2)
    c['elf_ppc_os4'] = elf(20, 2)
    c['elf_x86'] = elf(3, 2, be=False)
    c['elf64'] = b'\x7fELF\x02\x02' + b'\0' * 60
    c['pe'] = b'MZ\x90\0' + b'\0' * 60
    c['script'] = b'; startup script\nEcho "hi"\n'
    c['empty'] = b''
    c['garbage'] = bytes(range(256))
    return c

EXPECT = {
    # name: (exit, kind, valid, runnable, extra-checks)
    'minimal': (0, 2, 1, 1, ['HUNKS=1', 'HUNK0=CODE alloc=8']),
    'code_data_bss': (0, 2, 1, 1, ['HUNKS=3', 'RELOCS=1', 'HUNK2=BSS alloc=256',
                                   'REFS=dos.library,intuition.library,qrxnonexistent.library,timer.device,gadtools.library']),
    'chip': (0, 2, 1, 1, ['CHIP=16', 'WARN=requests Chip RAM']),
    'reloc32short': (0, 2, 1, 1, ['RELOCS=2']),
    'symbols_debug': (0, 2, 1, 1, ['SYMBOLS=1', 'DEBUG=1']),
    'truncated_header': (1, 2, 0, 0, ['ERROR=truncated HUNK_HEADER']),
    'truncated_payload': (1, 2, 0, 0, ['ERROR=truncated hunk payload']),
    'missing_hunk': (1, 2, 0, 0, ['ERROR=truncated: only 1 of 2 declared hunks present']),
    'reloc_oob': (1, 2, 0, 0, ['ERROR=relocation offset outside its hunk']),
    'reloc_bad_target': (1, 2, 0, 0, ['ERROR=relocation references undeclared hunk']),
    'payload_too_big': (1, 2, 0, 0, ['ERROR=hunk payload larger than header allocation']),
    'unknown_hunk': (1, 2, 0, 0, ['ERROR=unknown hunk type 0x000003aa']),
    'resident_names': (1, 2, 0, 0, ['ERROR=HUNK_HEADER lists resident libraries']),
    'huge_table': (1, 2, 0, 0, ['ERROR=unsupported hunk table size']),
    'object_unit': (1, 3, 0, 0, ['ERROR=HUNK_UNIT object file']),
    'hunk_lib': (1, 4, 0, 0, []),
    'ext_in_exe': (1, 2, 0, 0, ['ERROR=HUNK_EXT in load file']),
    'overlay': (0, 2, 1, 1, ['OVERLAY=1', 'WARN=overlay program']),
    'elf_aros_m68k': (0, 5, 1, 1, ['WARN=AROS-native ELF']),
    'elf_linux_m68k': (1, 6, 0, 0, ["ERROR=Linux/m68k ELF: use 'linuxrun'"]),
    'elf_ppc_os4': (1, 7, 0, 0, ['ERROR=PowerPC ELF (AmigaOS 4/MorphOS/Linux-PPC) is NOT supported']),
    'elf_x86': (1, 8, 0, 0, ['ERROR=ELF for foreign CPU']),
    'elf64': (1, 8, 0, 0, ['ERROR=64-bit ELF']),
    'pe': (1, 9, 0, 0, ["ERROR=x86 DOS/Windows image: use 'winrun'"]),
    'script': (1, 10, 0, 0, ["ERROR=text file: run AmigaDOS scripts with 'Execute'"]),
    'empty': (1, 1, 0, 0, ['ERROR=file is empty']),
    'garbage': (1, 0, 0, 0, ['ERROR=not an Amiga load file']),
}

PARSE = [
    # line, ok, program, args, flags-substrings
    ('C:Echo hello world', 1, 'C:Echo', 'hello world', []),
    ('"Work:My Games/Run Me" -x "two words"', 1, 'Work:My Games/Run Me', '-x "two words"', []),
    ('CHECK "SYS:Tools/Calc"', 1, 'SYS:Tools/Calc', '', ['CHECK=1']),
    ('--check --strict SYS:C/Dir', 1, 'SYS:C/Dir', '', ['CHECK=1', 'STRICT=1']),
    ('STACK=65536 ASYNC QUIET Work:x arg', 1, 'Work:x', 'arg', ['STACK=65536', 'ASYNC=1', 'QUIET=1']),
    ('--stack 20000 Work:x', 1, 'Work:x', '', ['STACK=20000']),
    ('-- CHECK', 1, 'CHECK', '', ['CHECK=0']),
    ('"A*"B*"" tail', 1, 'A"B"', 'tail', []),
    ('"star**name"', 1, 'star*name', '', []),
    ('prog args with trailing newline\n', 1, 'prog', 'args with trailing newline', []),
    ('', 0, '', '', ['ERROR=no program given']),
    ('CHECK', 0, '', '', ['ERROR=no program given']),
    ('"unterminated', 0, '', '', ['ERROR=unterminated quoted argument']),
    ('"bad*xscape"', 0, '', '', ['ERROR=invalid * escape']),
    ('"a"b', 0, '', '', ['ERROR=garbage directly after closing quote']),
    ('STACK=12 prog', 0, '', '', ['ERROR=stack must be 4096..67108864 bytes']),
    ('STACK=abc prog', 0, '', '', ['ERROR=STACK= needs a decimal byte count']),
    ('--bogus prog', 0, '', '', ['ERROR=unknown option']),
]

QUOTE = [
    ('Work:My Game/run', 1, '"Work:My Game/run"'),
    ('a"b', 1, '"a*"b"'),
    ('x*y', 1, '"x**y"'),
    ('line\nbreak', 1, '"line*Nbreak"'),
    ('cr\rhere', 0, ''),
    ('tab\there', 1, '"tab\there"'),
]

RC = [(-1, 'NOT STARTED'), (0, 'OK'), (5, 'WARN'), (10, 'ERROR'), (20, 'FAIL'), (30, 'FAIL')]

def main():
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, 'qh')
    subprocess.run(['gcc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe,
                    os.path.join(HERE, 'qrx_hunk_host.c'), os.path.join(SRC, 'qrx_hunk.c')], check=True)
    passed = failed = 0
    def check(name, cond, detail=''):
        nonlocal passed, failed
        if cond:
            passed += 1
        else:
            failed += 1
            print(f'FAIL {name} {detail}')
    for name, data in cases().items():
        p = os.path.join(tmp, name)
        open(p, 'wb').write(data)
        res = subprocess.run([exe, p], capture_output=True, text=True)
        out = res.stdout
        ex, kind, valid, runnable, extra = EXPECT[name]
        check(name + ':exit', res.returncode == ex, f'rc={res.returncode}\n{out}')
        check(name + ':kind', out.startswith(f'KIND={kind} '), out.splitlines()[0] if out else '')
        check(name + ':valid', f'VALID={valid}\n' in out and f'RUNNABLE={runnable}\n' in out, out)
        for e in extra:
            check(name + ':' + e, e in out, out)
    # path with spaces / missing path on the host harness
    sp = os.path.join(tmp, 'dir with space'); os.mkdir(sp)
    open(os.path.join(sp, 'prog'), 'wb').write(cases()['minimal'])
    check('path-with-space', subprocess.run([exe, os.path.join(sp, 'prog')], capture_output=True).returncode == 0)
    check('missing-path', subprocess.run([exe, os.path.join(tmp, 'nope')], capture_output=True).returncode == 3)
    for line, ok, prog, args, flags in PARSE:
        res = subprocess.run([exe, '--parse', line], capture_output=True, text=True)
        out = res.stdout
        check(f'parse[{line!r}]:ok', f'OK={ok}\n' in out, out)
        if ok:
            check(f'parse[{line!r}]:prog', f'PROGRAM=[{prog}]' in out, out)
            check(f'parse[{line!r}]:args', f'ARGS=[{args}]' in out, out)
        for f in flags:
            check(f'parse[{line!r}]:{f}', f in out, out)
    # round trip: quoting the parsed program and re-parsing yields the same program
    for prog in ['Work:My Game/run', 'a"b*c', 'plain']:
        res = subprocess.run([exe, '--quote', prog], capture_output=True, text=True)
        q = res.stdout.split('QUOTED=[', 1)[1].rsplit(']', 1)[0]
        res2 = subprocess.run([exe, '--parse', q + ' arg'], capture_output=True, text=True)
        check(f'roundtrip[{prog}]', f'PROGRAM=[{prog}]' in res2.stdout and 'ARGS=[arg]' in res2.stdout, res2.stdout)
    for s, ok, want in QUOTE:
        res = subprocess.run([exe, '--quote', s], capture_output=True, text=True)
        check(f'quote[{s!r}]', f'OK={ok}\n' in res.stdout and (not ok or f'QUOTED=[{want}]' in res.stdout), res.stdout)
    for rc, cls in RC:
        res = subprocess.run([exe, '--rc', str(rc)], capture_output=True, text=True)
        check(f'rc[{rc}]', res.stdout.strip() == cls, res.stdout)
    # Optional real-file corpus (e.g. Deadwood AROS.HUNK/C): every file must be valid HUNK.
    real = 0
    for d in sys.argv[1:]:
        for fn in sorted(os.listdir(d)):
            p = os.path.join(d, fn)
            if not os.path.isfile(p) or open(p, 'rb').read(4) != b'\0\0\x03\xf3':
                continue
            res = subprocess.run([exe, p], capture_output=True, text=True)
            real += 1
            check(f'real:{fn}', res.returncode == 0 and 'VALID=1' in res.stdout, res.stdout[-300:])
    print(f'amiga launcher host tests: {passed} passed, {failed} failed, real HUNK files checked: {real}')
    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
