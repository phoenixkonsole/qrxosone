#!/usr/bin/env python3
"""Debugging aid: walks exec's task lists in a RAM dump (QRX_FULL_DUMP_RAM) and resolves the words
on a task's stack against a linked ELF, to see where a guest program waits.
usage: guest_stack.py <ram dump> <elf> <guest address of main> [task name part]"""
import bisect, struct, subprocess, sys
ram = open(sys.argv[1], 'rb').read()
elf, main_addr = sys.argv[2], int(sys.argv[3], 16)
want = sys.argv[4] if len(sys.argv) > 4 else 'blender'
syms = []
for l in subprocess.run(['/opt/homebrew/bin/m68k-elf-nm', '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
main_off = next(a for a, n in syms if n == 'main')
base = main_addr - main_off
addrs = [a for a, _ in syms]
def r32(a):
    return struct.unpack('>I', ram[a:a + 4])[0] if a + 4 <= len(ram) else 0
def cstr(a):
    if not a or a >= len(ram):
        return '?'
    return ram[a:ram.index(b'\0', a)].decode('latin-1')
sysbase = r32(4)
print(f'SysBase {sysbase:08x}, text base {base:08x}, ThisTask {r32(sysbase + 276):08x} {cstr(r32(r32(sysbase + 276) + 10))}')
def tasks(head):
    n = r32(head)
    while n and r32(n):
        yield n
        n = r32(n)
found = []
for label, head in (('ready', sysbase + 406), ('wait', sysbase + 420)):
    for t in tasks(head):
        name = cstr(r32(t + 10))
        print(label, f'{t:08x}', name, f'sigwait {r32(t + 22):08x} sigrecvd {r32(t + 26):08x}')
        found.append(t)
for t in found + [r32(sysbase + 276)]:
    name = cstr(r32(t + 10))
    # a shell runs its command under the shell task's own name, so check all stacks with text addresses
    sp, lower, upper = r32(t + 54), r32(t + 58), r32(t + 62)
    if not (lower <= sp <= upper):
        continue
    hits = []
    for a in range(sp, upper, 2):
        v = r32(a) - base
        if 0 < v < syms[-1][0]:
            i = bisect.bisect_right(addrs, v) - 1
            hits.append(f'{syms[i][1]}+{v - addrs[i]:x}')
    if hits:
        print(f'--- {name} sp {sp:08x} stack {upper - lower} bytes, used {upper - sp}')
        for h in hits[:48]:
            print('  ', h)
