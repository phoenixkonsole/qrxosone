#!/usr/bin/env python3
"""Names the places where cores took the 68100EC64's global Forbid (QRX_EC64_GF_STATS=FILE).
Usage: ec64gfstat.py FILE LOG [--program ELF] [--kernel-map MAP] [--top N]
FILE has lines "count pc w0 .. w5" (pc and the first stack words); LOG is the run's console log
(module addresses, QRXBT anchor). The caller is the first stack word that lies in known code; the
names come from ec64prof.py's tables."""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser()
ap.add_argument('stats'); ap.add_argument('log')
ap.add_argument('--program'); ap.add_argument('--kernel-map'); ap.add_argument('--top', type=int, default=30)
a = ap.parse_args()
# ec64prof.py builds its tables at import from sys.argv: give it a profile-less command line
argv = ['ec64prof.py', os.devnull, a.log]
if a.program: argv += ['--program', a.program]
if a.kernel_map: argv += ['--kernel-map', a.kernel_map]
sys.argv = argv
import io, contextlib
with contextlib.redirect_stdout(io.StringIO()):
    import ec64prof
def in_code(v):
    i = ec64prof.bisect.bisect_right(ec64prof.starts, v) - 1
    return i >= 0 and v < ec64prof.ranges[i][1]
rows = []
for line in open(a.stats):
    f = line.split()
    n, pc, words = int(f[0]), int(f[1], 16), [int(w, 16) for w in f[2:]]
    ret = next((w for w in words if in_code(w)), 0)
    rows.append((n, pc, ret))
total = sum(r[0] for r in rows)
by_pc = collections.Counter()
by_pair = collections.Counter()
for n, pc, ret in rows:
    by_pc[ec64prof.name(pc)] += n
    by_pair[(ec64prof.name(pc), ec64prof.name(ret))] += n
print('%d times in all' % total)
print('\nwhere (pc):')
for k, v in by_pc.most_common(a.top): print('  %6.2f%%  %s' % (100.0 * v / total, k[:140]))
print('\nwhere, from where (pc <- the first stack word in code):')
for (p, r), v in by_pair.most_common(a.top): print('  %6.2f%%  %s  <-  %s' % (100.0 * v / total, p[:70], r[:90]))
