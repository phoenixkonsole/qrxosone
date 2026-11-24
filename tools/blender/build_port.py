#!/usr/bin/env python3
"""Builds what aros_cc.py links into every 68k program of the Blender build.

- port/*.c: POSIX pieces and the core 1 runtime.
- libqrx_msun.a: FreeBSD msun as kept in the AROS source (compiler/crt/stdc/math), compiled with
  hardware floats. The SDK's libm returns doubles in d0:d1, GCC here expects them in fp0, and core 1
  must not make library calls anyway.
- bridges: the SDK libraries return pointers in d0 and doubles in d0:d1, while GCC reads a0 and fp0.
  `-aux-info` gives the real prototypes; each such SDK function the build uses gets a --wrap bridge.
"""
import concurrent.futures as cf, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from qrxenv import ROOT, WORK, SDK, SDK_DIR, GCC, tool  # noqa: E402
OUT = WORK / 'port'
CC = str(HERE / 'aros_cc.py')
NM = tool('nm')
AR = tool('ar')
HEADERS = ['stdio.h', 'stdlib.h', 'string.h', 'strings.h', 'unistd.h', 'time.h', 'dirent.h', 'pthread.h', 'errno.h',
           'ctype.h', 'wchar.h', 'sys/stat.h', 'sys/time.h', 'signal.h', 'fcntl.h', 'locale.h', 'zlib.h', 'png.h',
           'stdio.h', 'jpeglib.h', 'ft2build.h', 'math.h', 'libgen.h', 'semaphore.h', 'sched.h']


def msun():
    src = WORK / 'msun/math'
    objs = OUT / 'msun'
    objs.mkdir(parents=True, exist_ok=True)
    skip = re.compile(r'(?<!_cei)(?<!_mod)l$|l_r$|_128$|^s_c(?!os|bcrt|opysign|eil)|^s_nan')
    def one(f):
        o = objs / (f.stem + '.o')
        r = subprocess.run([CC, '-O2', '-std=gnu99', '-fno-builtin', '-Dlint', '-D__FBSDID(x)=', '-DSTRICT_ASSIGN(type,lval,rval)=((lval)=(type)(rval))', '-include', 'float.h', '-include', 'sys/cdefs.h', f'-I{src}', f'-I{src}/m68k', '-c', str(f), '-o', str(o)],
                           capture_output=True, text=True)
        return f.name, r.returncode, r.stderr
    files = [f for pat in ('e_*.c', 's_*.c', 'k_*.c') for f in src.glob(pat) if not skip.search(f.stem)]
    with cf.ThreadPoolExecutor(8) as ex:
        res = list(ex.map(one, files))
    bad = [(n, e) for n, rc, e in res if rc]
    for n, e in bad:
        print('msun skipped', n, [l for l in e.splitlines() if 'error' in l][:1])
    # msun names the entry points __ieee754_x; a jump keeps the stack arguments and the fp0 result.
    syms = set()
    for o in objs.glob('[es]_*.o'):
        syms |= nm_defined(o)
    gcc = GCC
    for sym in sorted(syms):
        if sym.startswith('__ieee754_') and not sym.endswith(('_r', 'rem_pio2', 'rem_pio2f')):
            name = sym[len('__ieee754_'):]
            if name in syms or name in ('sqrt', 'sqrtf'):
                continue
            a = objs / f'alias_{name}.S'
            a.write_text(f'.text\n.globl {name}\n{name}:\njmp {sym}\n')
            subprocess.check_call([gcc, '-m68040', '-c', str(a), '-o', str(objs / f'alias_{name}.o')])
            a.unlink()
    # The FPU's own square root, not the bit by bit one in C.
    for name, op in (('sqrt', 'fdsqrt.d'), ('sqrtf', 'fssqrt.s')):
        (objs / f'e_{name}.o').unlink(missing_ok=True)
        a = objs / f'alias_{name}.S'
        a.write_text(f'.text\n.globl {name}\n.globl __ieee754_{name}\n{name}:\n__ieee754_{name}:\n{op} 4(%sp),%fp0\nrts\n')
        subprocess.check_call([gcc, '-m68040', '-c', str(a), '-o', str(objs / f'alias_{name}.o')])
        a.unlink()
    lib = OUT / 'libqrx_msun.a'
    lib.unlink(missing_ok=True)
    subprocess.check_call([AR, 'rcs', str(lib), *map(str, objs.glob('*.o'))])
    print(len(files) - len(bad), 'msun files')


def prototypes():
    tu = OUT / 'protos.c'
    tu.write_text(''.join(f'#include <{h}>\n' for h in HEADERS) + '#include FT_FREETYPE_H\n')
    aux = OUT / 'protos.aux'
    subprocess.run([CC, '-c', str(tu), '-o', str(OUT / 'protos.tmp'), '-aux-info', str(aux)], check=True)
    (OUT / 'protos.tmp').unlink()
    out = {}
    for line in aux.read_text(errors='replace').splitlines():
        m = re.match(r'/\* [^*]*\*/ (?:extern )?(.*?)\b(\w+) \((.*)\);', line)
        if not m:
            continue
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3).split(' /*')[0]
        depth, n, cur = 0, 0, ''
        for ch in args:
            depth += ch == '('; depth -= ch == ')'
            if ch == ',' and depth == 0:
                n += 1
        n = 0 if args.strip() in ('void', '') else n + 1
        kind = 'p' if ret.endswith('*') else 'd' if re.search(r'\bdouble$', ret) else 'f' if re.search(r'\bfloat$', ret) else None
        if kind and '...' not in args:
            words = 0
            for a in split_args(args):
                words += 2 if re.search(r'\b(double|long long)\b', a) and '*' not in a else 1
            out[name] = (kind, words)
    return out


def split_args(args):
    if args.strip() in ('void', ''):
        return []
    res, depth, cur = [], 0, ''
    for ch in args:
        depth += ch == '('; depth -= ch == ')'
        if ch == ',' and depth == 0:
            res.append(cur); cur = ''
        else:
            cur += ch
    return res + [cur]


def nm_defined(path):
    out = subprocess.run([NM, '--defined-only', '-g', str(path)], capture_output=True, text=True).stdout
    return {l.split()[-1] for l in out.splitlines() if re.search(r' [TWDBR] ', l)}


def nm_undefined(paths):
    names = set()
    for p in paths:
        out = subprocess.run([NM, '-u', str(p)], capture_output=True, text=True).stdout
        names |= {l.split()[-1] for l in out.splitlines() if l.strip().startswith('U')}
    return names


def bridges():
    protos = prototypes()
    protos.setdefault('realpath', ('p', 2))  # the SDK's stdlib.h hides it from strict modes
    builtin = {w.split('=', 1)[1] for w in __import__('json').loads(
        (SDK_DIR / 'bin/toolchain.json').read_text())['wrap_flags']}
    sdk_syms = set()
    for lib in ('libm.a', 'libcrt.a', 'libstdlib.a', 'libcrtprog.a', 'libstdc.static.a', 'libpthread.a', 'libz.a', 'libpng.a',
                'libjpeg.a', 'libfreetype2.a', 'libarossupport.a', 'libamiga.a'):
        if (SDK / 'lib' / lib).exists():
            sdk_syms |= nm_defined(SDK / 'lib' / lib)
    ours = set()
    for a in WORK.glob('build/lib/*.a'):
        ours |= nm_defined(a)
    ours |= nm_defined(OUT / 'libqrx_msun.a')
    for o in OUT.glob('qrx_*.o'):
        ours |= nm_defined(o)
    # What the port defines itself needs no bridge: plain names replace the SDK's, __wrap_ names
    # keep the --wrap flag so they reach ours.
    port_wrapped = sorted(n[len('__wrap_'):] for o in OUT.glob('qrx_*.o') for n in nm_defined(o) if n.startswith('__wrap_'))
    ours = {n for n in ours if not n.startswith('__wrap_')}
    want = sorted(n for n in (sdk_syms & set(protos)) | (builtin & set(protos))
                  if n not in ours and n not in port_wrapped)
    for n in sorted(builtin - set(protos) - ours - set(port_wrapped)):
        print('no prototype for', n)
    bdir = OUT / 'bridges'
    bdir.mkdir(exist_ok=True)
    for o in bdir.glob('*'):
        o.unlink()
    gcc = GCC
    def one(name):
        kind, words = protos[name]
        asm = ['.text', f'.globl __wrap_{name}', f'__wrap_{name}:', 'movem.l %a2/%a6,-(%sp)', 'lea 12(%sp),%a2']
        for i in reversed(range(words)):
            asm.append(f'move.l {i * 4}(%a2),-(%sp)')
        asm.append(f'jsr __real_{name}')
        if words:
            asm.append(f'lea {words * 4}(%sp),%sp')
        asm.append('movem.l (%sp)+,%a2/%a6')
        if kind == 'p':
            asm.append('move.l %d0,%a0')
        elif kind == 'd':
            asm += ['move.l %d1,-(%sp)', 'move.l %d0,-(%sp)', 'fmove.d (%sp)+,%fp0']
        else:
            asm += ['fmove.s %d0,%fp0']
        asm.append('rts')
        f = bdir / f'{name}.S'
        f.write_text('\n'.join(asm) + '\n')
        subprocess.check_call([gcc, '-m68040', '-c', str(f), '-o', str(bdir / f'{name}.o')])
    with cf.ThreadPoolExecutor(8) as ex:
        list(ex.map(one, want))
    lib = OUT / 'libqrx_bridges.a'
    lib.unlink(missing_ok=True)
    subprocess.check_call([AR, 'rcs', str(lib), *map(str, bdir.glob('*.o'))])
    (OUT / 'wraps.flags').write_text(' '.join('--wrap=' + n for n in want + port_wrapped))
    print(len(want), 'bridges:', ' '.join(f'{n}:{protos[n][0]}{protos[n][1]}' for n in want))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for f in (HERE / 'port').glob('*.c'):
        subprocess.check_call([CC, '-O2', '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-c', str(f), '-o', str(OUT / ('qrx_' + f.stem.removeprefix('qrx_') + '.o'))])
    if 'msun' in sys.argv or not (OUT / 'libqrx_msun.a').exists():
        msun()
    bridges()


if __name__ == '__main__':
    main()
