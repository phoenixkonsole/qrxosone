#!/usr/bin/env python3
"""Sums up a 68100EC64 profile (qrx_ec64_run --profile FILE) by function.
Usage: ec64prof.py PROFILE LOG [--program ELF] [--kernel-map MAP] [--from S] [--to S] [--top N]
  PROFILE  records of 16 bytes: u32 ms, u8 core, u8 flags (1 supervisor, 2 waiting), u16, u64 pc
  LOG      the run's console log: the "module PATH at A-B" lines name the ROM modules; with
           QRX_BACKTRACE set the program prints "QRXBT anchor A" (its qrx_bt_anchor), which gives
           the load base of --program (an ET_REL AROS executable with symbols, e.g. Gecko's firefox)
  --kernel-map  the kernel's link map (nm -n output of core.elf, loaded at 0x100000 + value)
  --from/--to   only the samples in this window of wall-clock seconds
Prints the share of each core's time that was idle (STOP), then the hottest functions."""
import argparse, bisect, collections, re, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('profile'); ap.add_argument('log')
ap.add_argument('--program'); ap.add_argument('--kernel-map'); ap.add_argument('--kernel-base', default='0x100000')
ap.add_argument('--from', dest='t0', type=float, default=0); ap.add_argument('--to', dest='t1', type=float, default=1e12)
ap.add_argument('--top', type=int, default=40)
ap.add_argument('--llvm', default='/opt/qrx-os-one/ec64llvm/build/bin')
a = ap.parse_args()

ranges = []          # (start, end, name, symbols or None, base)
log = open(a.log, errors='replace').read()
for m in re.finditer(r'module (\S+) at ([0-9a-f]+)-([0-9a-f]+)', log):
    ranges.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1).rsplit('/', 1)[-1], None, 0))

def nm(path):
    out = subprocess.run([a.llvm + '/llvm-nm', '-n', '--defined-only', '-C', path], capture_output=True, text=True).stdout
    s = []
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in 'tTwW':
            s.append((int(p[0], 16), p[2]))
    s.sort()
    return s

if a.kernel_map:
    ks = []
    for line in open(a.kernel_map):
        p = line.split()
        if len(p) >= 2:
            try: ks.append((int(p[0], 16), p[-1]))
            except ValueError: pass
    ks.sort()
    kb = int(a.kernel_base, 0)
    ranges.append((kb, kb + (ks[-1][0] if ks else 0) + 0x10000, 'kernel', ks, kb))
if a.program:
    m = re.search(r'QRXBT anchor (0x)?([0-9a-fA-F]+)', log)
    ps = nm(a.program)
    anc = [v for v, n in ps if n == 'qrx_bt_anchor']
    if m and anc:
        base = int(m.group(2), 16) - anc[0]
        sz = 0
        for line in subprocess.run([a.llvm + '/llvm-readelf', '-S', '-W', a.program], capture_output=True, text=True).stdout.splitlines():
            mm = re.search(r'\]\s+\.text\s+PROGBITS\s+\S+\s+\S+\s+([0-9a-f]+)', line)
            if mm: sz = int(mm.group(1), 16)
        ranges.append((base, base + sz, a.program.rsplit('/', 1)[-1], ps, base))
    else:
        print('no QRXBT anchor in the log: the program is not named', file=sys.stderr)
ranges.sort()
starts = [r[0] for r in ranges]

def name(pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i < 0 or pc >= ranges[i][1]:
        return '?%x' % (pc & ~0xfff)
    lo, hi, mod, syms, base = ranges[i]
    if not syms:
        return mod
    off = pc - base
    j = bisect.bisect_right(syms, (off, '￿')) - 1
    return '%s:%s' % (mod, syms[j][1] if j >= 0 else '?')

data = open(a.profile, 'rb').read()
cores = collections.Counter(); idle = collections.Counter(); hot = collections.Counter(); mods = collections.Counter()
n = 0
for ms, core, fl, _, pc in struct.iter_unpack('<IBBHQ', data):
    t = ms / 1000.0
    if t < a.t0 or t >= a.t1: continue
    cores[core] += 1
    if fl & 2: idle[core] += 1; continue
    nm_ = name(pc)
    hot[nm_] += 1; mods[nm_.split(':')[0]] += 1; n += 1
for c in sorted(cores):
    print('core %d: %d samples, %.1f%% idle' % (c, cores[c], 100.0 * idle[c] / cores[c]))
print('\nbusy samples by module:')
for k, v in mods.most_common(12): print('  %6.2f%%  %s' % (100.0 * v / max(n, 1), k))
print('\nhottest functions (share of busy samples):')
for k, v in hot.most_common(a.top): print('  %6.2f%%  %s' % (100.0 * v / max(n, 1), k[:150]))
