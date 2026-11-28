#!/usr/bin/env python3
"""Where a task waited: the off-CPU records of qrx_ec64_run --profile with QRX_EC64_PROF_OFFCPU=NAME
(PROFILE.offcpu: u32 ms, u8 tc_State, 3 bytes, then 64 big-endian stack words from its saved stack
pointer up).
Usage: ec64offcpu.py PROFILE.offcpu LOG --program ELF [--from S] [--to S] [--depth N] [--top N]
  LOG      the run's console log with "QRXBT anchor A" (QRX_BACKTRACE set in the guest), which gives
           the program's load base, as for ec64prof.py
Stack words are candidates, as in qrxbt.py: values that fall into the program's code. The chain of
the first --depth of them names a wait; the waits are counted over the samples, so a share is the
part of the time the task was not running that it spent there."""
import argparse, bisect, collections, re, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('offcpu'); ap.add_argument('log')
ap.add_argument('--program', required=True)
ap.add_argument('--from', dest='t0', type=float, default=0); ap.add_argument('--to', dest='t1', type=float, default=1e12)
ap.add_argument('--depth', type=int, default=4); ap.add_argument('--top', type=int, default=25)
ap.add_argument('--llvm', default='/opt/qrx-os-one/ec64llvm/build/bin')
a = ap.parse_args()

syms = []
for line in subprocess.run([a.llvm + '/llvm-nm', '-n', '-C', '--defined-only', a.program],
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
vals = [s[0] for s in syms]
anchor = dict((n, v) for v, n in syms).get('qrx_bt_anchor')
m = re.search(r'QRXBT anchor (?:0x)?([0-9a-fA-F]+)', open(a.log, errors='replace').read())
if anchor is None or not m:
    sys.exit('need qrx_bt_anchor in the program and "QRXBT anchor" in the log (QRX_BACKTRACE)')
base = int(m.group(1), 16) - anchor
lo, hi = base + vals[0], base + vals[-1] + 0x10000

def name(v):
    j = bisect.bisect_right(vals, v - base) - 1
    return syms[j][1] if j >= 0 else '?'

# frames that every wait goes through and that say nothing about what it waited for
generic = re.compile(r'^(qrx_|__|PR_Wait|PR_WaitCondVar|pt_TimedWait|pthread_cond|mozilla::detail::ConditionVariableImpl'
                     r'|mozilla::CondVar|mozilla::OffTheBooksCondVar|mozilla::detail::MutexImpl|_pthread|'
                     r'mozilla::BaseMonitor|mozilla::Monitor::Wait|mozilla::ReentrantMonitor)')
data = open(a.offcpu, 'rb').read()
recsize = 8 + 64 * 8
chains = collections.Counter(); first = collections.Counter(); n = 0; states = collections.Counter()
for i in range(0, len(data) - recsize + 1, recsize):
    ms, st = struct.unpack_from('<IB', data, i)
    if not (a.t0 <= ms / 1000.0 < a.t1): continue
    n += 1; states[st] += 1
    words = struct.unpack_from('>64Q', data, i + 8)
    frames = [name(w) for w in words if lo <= w < hi]
    useful = [f for f in frames if not generic.match(f)]
    chains[' <- '.join(f[:60] for f in useful[:a.depth]) or '(no frame of the program)'] += 1
    if useful: first[useful[0][:110]] += 1
print('%d samples while the task was not running; tc_State: %s' % (n, dict(states)))
print('\nfirst frame of the program that says what it waited in:')
for k, v in first.most_common(a.top): print('  %6.2f%%  %s' % (100.0 * v / max(n, 1), k))
print('\nchains (innermost first):')
for k, v in chains.most_common(a.top): print('  %6.2f%%  %s' % (100.0 * v / max(n, 1), k))
