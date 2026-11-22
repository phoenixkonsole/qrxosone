#!/usr/bin/env python3
"""Write one small object per pointer-returning libc function: <out>/libqrxbridges.a.

AROS's libraries return pointers in d0, m68k-elf-gcc reads them from a0. The SDK's own
pointer-bridges.o covers a short list; this finds the rest from the headers. Each bridge
sits in its own archive member, so only those a program uses get linked.
Usage: gen-bridges.py <toolchain.json> <out-dir>
"""
import json, re, subprocess, sys
from pathlib import Path

HEADERS = '''stdio.h stdlib.h string.h strings.h time.h locale.h signal.h unistd.h dirent.h pwd.h grp.h
fcntl.h ctype.h wchar.h errno.h setjmp.h pthread.h semaphore.h libgen.h sys/stat.h sys/time.h sys/types.h
sys/wait.h termios.h math.h'''.split()
# Not functions the SDK's headers name, or data rather than a pointer return.
SKIP = {'main', 'alloca'}

def main():
    cfg = json.loads(Path(sys.argv[1]).read_text())
    out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
    base, cc = cfg['sdk'], cfg['gcc']
    inc = []
    for d in ('/include/aros/posixc', '/include/aros/stdc', '/include'):
        inc += ['-isystem', base + d]
    inc += ['-isystem', cfg['gcc_include']]
    avail = [h for h in HEADERS if any(Path(base + d, h).exists() for d in ('/include/aros/posixc', '/include/aros/stdc', '/include'))]
    src = ''.join(f'#include <{h}>\n' for h in avail)
    pp = subprocess.run([cc, '-include', cfg['abi_header'], '-D__AROS__', '-U__linux__', '-nostdinc', '-m68040', *inc, '-E', '-P', '-x', 'c', '-'],
                        input=src, text=True, capture_output=True, check=True).stdout
    pp = re.sub(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)', ' ', pp)
    pp = re.sub(r'\b(?:__extension__|__restrict|restrict|__inline__|__inline)\b', ' ', pp)
    found = {}
    for m in re.finditer(r'(?:^|[;}])\s*((?:[A-Za-z_][\w ]*?)\s*\*[\s\*]*(?:const\s*)?)([A-Za-z_]\w*)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*;', pp):
        ret, name, args = m.groups()
        if 'typedef' in ret or 'static' in ret or name in SKIP: continue
        args = args.strip()
        if '...' in args: continue
        depth = 0; n = 0 if args in ('', 'void') else 1
        for ch in args:
            depth += ch == '('; depth -= ch == ')'
            if ch == ',' and depth == 0: n += 1
        found[name] = n
    # signal() returns a function pointer; the others are declared in forms the pattern misses.
    # Found by comparing a linked program's SDK symbols against their prototypes.
    found.update(signal=2, getpwuid=1, getgrgid=1, readdir64=1, localtime_r=2, index=2, rindex=2, ctime_r=2)
    done = {w.split('=')[1] for w in cfg['wrap_flags']}
    names = sorted(set(found) - done)
    objs = []
    for name in names:
        n = found[name]
        s = [f'.text\n.globl __wrap_{name}\n__wrap_{name}:\nmovem.l %a2/%a6,-(%sp)\nlea 12(%sp),%a2']
        for i in range(n - 1, -1, -1):
            s.append(f'move.l {i*4}(%a2),-(%sp)')
        s.append(f'jsr __real_{name}')
        if n: s.append(f'lea {n*4}(%sp),%sp')
        s.append('movem.l (%sp)+,%a2/%a6\nmove.l %d0,%a0\nrts\n')
        o = out / f'b_{name}.o'
        subprocess.run([cfg['as'], '-m68040', '-o', str(o), '-'], input='\n'.join(s), text=True, check=True)
        objs.append(str(o))
    # AROS's libraries also return doubles in d0/d1, m68k-elf-gcc with an FPU reads fp0.
    # (name, stack words of arguments)
    for name, words in dict(pow=4, log=2, exp=2, expm1=2, log1p=2, log2=2, log10=2, floor=2, ceil=2, round=2, trunc=2,
                            fmod=4, sin=2, cos=2, tan=2, atan=2, atan2=4, sqrt=2, fabs=2, ldexp=3, frexp=3, modf=3,
                            strtod=2, atof=1).items():
        s = [f'.text\n.globl __wrap_{name}\n__wrap_{name}:\nmovem.l %a2/%a6,-(%sp)\nlea 12(%sp),%a2']
        for i in range(words - 1, -1, -1):
            s.append(f'move.l {i*4}(%a2),-(%sp)')
        s.append(f'jsr __real_{name}\nlea {words*4}(%sp),%sp')
        s.append('move.l %d1,-(%sp)\nmove.l %d0,-(%sp)\nfmove.d (%sp)+,%fp0\nmovem.l (%sp)+,%a2/%a6\nrts\n')
        o = out / f'b_{name}.o'
        subprocess.run([cfg['as'], '-m68040', '-o', str(o), '-'], input='\n'.join(s), text=True, check=True)
        objs.append(str(o))
        names.append(name)
    lib = out / 'libqrxbridges.a'
    lib.unlink(missing_ok=True)
    subprocess.run([cfg['ar'], 'rcs', str(lib), *objs], check=True)
    (out / 'wrap-generated.txt').write_text('\n'.join(names) + '\n')
    print(len(names), 'bridges:', ' '.join(names))

main()
