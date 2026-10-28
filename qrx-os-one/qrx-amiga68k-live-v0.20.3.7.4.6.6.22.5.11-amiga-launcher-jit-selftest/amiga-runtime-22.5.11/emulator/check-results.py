#!/usr/bin/env python3
"""Compare FS-UAE QRXTEST lines with the expected return codes (22.5.11)."""
import os, re, sys
EXPECT = {
    'T01-check-valid': {0}, 'T02-run-rc7-args': {7}, 'T03-run-path-with-spaces': {0},
    'T04-run-rc5-warn': {5}, 'T05-check-missing-library': {5}, 'T06-run-missing-library': {20},
    'T07-strict-refuses': {20}, 'T08-check-missing-path': {20}, 'T09-run-missing-path': {20},
    'T10-check-script': {10}, 'T11-check-empty': {10}, 'T12-check-truncated-hunk': {10},
    'T13-run-truncated-refused': {20}, 'T14-check-ppc-elf': {10}, 'T15-check-x86-pe': {10},
    'T16-check-system-command': {0, 5}, 'T17-run-system-command': {0}, 'T18-run-async': {0},
    'T19-run-directory': {20}, 'T20-bad-quoting': 'nonzero', 'T21-explicit-check-keyword': {0},
    'T22-run-stack-option': {3}, 'T23-jit-selftest-static': {0},
    'JIT-STATIC': {0}, 'JIT-EXECUTE': {0}, 'QRX86-HELP': {0},
    'QRX86-COUNT6': {0}, 'QRX86-REPFULL': {0}, 'QRX86-MOVR8': {0}, 'QRX86-JITON-QUARANTINE': {0},
    'QRX86-WINTRACE-SPIN': 'nonzero', 'QRX86-WINTRACE-REPSPIN': 'nonzero',
}
OUTPUT_CHECKS = [
    ('TestAmigaRun.txt', 'QRXHUNKTEST arg[2]=<two words>', 'T02 quoted argument arrives as one argv entry'),
    ('TestAmigaRun.txt', 'qrxnonexistent.library -> MISSING', 'T05 missing library is reported by name'),
    ('TestAmigaRun.txt', 'PowerPC ELF', 'T14 PPC ELF classified'),
    ('TestAmigaRun.txt', 'QRX echo through amigarun', 'T17 output of launched program visible'),
    ('TestAmigaRun.txt', 'QRXTEST END', 'test script ran to completion'),
    ('qrx86.txt', 'steps=6 ', 'QRX86 counts 6 guest instructions on m68k'),
    ('qrx86.txt', 'eax=00000000', 'QRX86 REP STOSB completes on m68k'),
    ('qrx86.txt', 'eax=00001234', 'QRX86 MOV r8,imm8 on m68k'),
    ('qrx86.txt', 'native JIT quarantined', 'QRX86 --jit=on stays quarantined on m68k'),
    ('wintrace-spin.txt', 'QRX86 WATCHDOG', 'wintrace endless loop stopped by watchdog on m68k'),
    ('wintrace-repspin.txt', 'QRX86 WATCHDOG', 'wintrace REP endless loop stopped by watchdog on m68k'),
]
def main():
    rep, cpus = sys.argv[1], sys.argv[2:]
    allok = True
    lines = []
    for cpu in cpus:
        d = os.path.join(rep, f'results-{cpu}')
        st = open(os.path.join(d, 'STATUS')).read().strip() if os.path.exists(os.path.join(d, 'STATUS')) else 'MISSING'
        got = {}
        for fn in ('TestAmigaRun.txt', 'jit-static.txt', 'jit-execute.txt', 'qrx86-help.txt', 'qrx86.txt'):
            p = os.path.join(d, fn)
            if os.path.exists(p):
                for m in re.finditer(r'QRXTEST (\S+) rc=(-?\d+)', open(p, errors='replace').read()):
                    got[m.group(1)] = int(m.group(2))
        lines.append(f'== CPU {cpu}: harness {st}')
        for k, want in EXPECT.items():
            if k not in got:
                ok = False; res = 'NOT RUN'
            else:
                ok = (got[k] != 0) if want == 'nonzero' else (got[k] in want)
                res = f'rc={got[k]}'
            allok &= ok
            lines.append(f'  {"PASS" if ok else "FAIL"} {k:30s} {res} expected {want}')
        for fn, needle, what in OUTPUT_CHECKS:
            p = os.path.join(d, fn)
            ok = os.path.exists(p) and needle in open(p, errors='replace').read()
            allok &= ok
            lines.append(f'  {"PASS" if ok else "FAIL"} {what}')
    txt = '\n'.join(lines)
    print(txt)
    open(os.path.join(rep, 'TARGET-RESULTS.txt'), 'w').write(txt + '\n')
    print('TARGET RESULT:', 'PASS' if allok else 'FAIL')
    return 0 if allok else 1
sys.exit(main())
