#!/usr/bin/env python3
"""Compiler driver for Blender on QRX OS One: m68k-elf-gcc/g++ against the AROS SDK.

tools/qrx86/aros-gcc.py only knows what the small QRX tools need. This one adds C++ (with no
standard library of our own build, see build_libstdcxx.sh), separate objects and static libraries, and the extra
pointer bridges of tools/blender/bridges. The ABI rule stays: GCC reads a returned pointer from
a0, the SDK libraries return it in d0, so every SDK function returning a pointer needs a bridge.
"""
import json, os, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('QRX_ROOT', '/Users/pascalpapara/Documents/GitHub/qrxosone'))
CFG = json.loads((ROOT / '.build/qrx-sdk/bin/toolchain.json').read_text())
BASE = CFG['sdk']
CPU = ['-m68040', '-fno-pic', '-fomit-frame-pointer', '-mlong-jump-table-offsets']
DEFS = ['-D__AROS__', '-DAMIGA', '-D_AMIGA', '-D__BIG_ENDIAN__=1', '-U__linux__', '-U__linux', '-Ulinux',
        '-U__gnu_linux__', '-U_TIME_BITS', '-U_FILE_OFFSET_BITS', '-D__unix__', '-Dunix']
STDCXX = ROOT / '.build/blender-2.79b/gcc/build'
DROP_LIBS = {'-lm', '-lpthreads', '-ldl', '-lutil', '-lrt', '-lc', '-lstdc++', '-lgcc_s'}


def includes(cxx):
    out = ['-nostdinc']
    if cxx:
        # While libstdc++ is being configured it brings its own headers.
        out += ['-nostdinc++']
        if not os.environ.get('QRX_CXX_BARE'):
            out += ['-isystem', str(STDCXX / 'include/m68k-elf'), '-isystem', str(STDCXX / 'include'),
                    '-isystem', str(STDCXX.parent / 'gcc-16.2.0/libstdc++-v3/libsupc++')]
    out += ['-isystem', str(HERE / 'shim/include')]
    for inc in ('/include/aros/posixc', '/include/aros/stdc', '/include'):
        out += ['-isystem', BASE + inc]
    return out + ['-isystem', CFG['gcc_include']]


# Options Blender's CMake adds that must not reach the compiler. The offset macros would change
# the file and time structures of the SDK, and -fPIC would override -fno-pic.
DROP_ARGS = {'-fPIC', '-fpic', '-fPIE', '-fpie', '-pie', '-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE64_SOURCE', '-D_LARGEFILE_SOURCE',
             '-Wmissing-include-dirs', '-Wredundant-decls', '-Wcast-align'}


def main():
    cxx = Path(sys.argv[0]).name.endswith('c++')
    args = [a for a in sys.argv[1:] if a not in DROP_ARGS]
    if '--version' in args or '-dumpversion' in args or '-v' in args:
        return subprocess.call([CFG['gcc'], *args])
    is_link = not any(a in args for a in ('-c', '-E', '-S', '-M', '-MM'))
    if not is_link:
        drv = CFG['gcc'][:-3] + 'g++' if cxx else CFG['gcc']
        flags = CPU + DEFS + ['-Wno-error=float-conversion', '-w', '-fno-math-errno',
                              # sin plus cos would become a call of cexp in the SDK's libm
                              '-fno-builtin-sin', '-fno-builtin-cos', '-fno-builtin-sinf', '-fno-builtin-cosf',
                              '-fno-builtin-cexp', '-fno-builtin-cexpf'] + includes(cxx)
        if cxx:
            flags += ['-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-optimize-sibling-calls']
        if not any(a.endswith(('.S', '.s')) for a in args):
            flags = ['-include', CFG['abi_header'], '-include', str(HERE / 'shim/include/qrx_extra.h')] + flags
        return subprocess.call([drv, *flags, *args])
    # link: only what ends up in an AROS executable
    output, inputs, i = 'a.out', [], 0
    while i < len(args):
        a = args[i]
        if a == '-o':
            output = args[i + 1]; i += 2; continue
        if a.endswith(('/libGL.a', '/libGLU.a')):
            pass  # first stage: no GL at all, see tools/blender/gen_missing.py
        elif a.endswith(('.o', '.a', '.obj')) or a.startswith('-L') or (a.startswith('-l') and a not in DROP_LIBS):
            inputs.append(a)
        elif a == '-Wl,--start-group' or a == '-Wl,--end-group':
            inputs.append(a[4:])
        i += 1
    libs = Path(BASE, 'lib/auto').read_text().splitlines()[1].split()
    wraps = []
    port = ROOT / '.build/blender-2.79b/port'
    extra = sorted(str(p) for p in port.glob('qrx_*.o')) if port.exists() else []
    if (port / 'wraps.flags').exists():
        wraps += (port / 'wraps.flags').read_text().split()
    cmd = [CFG['collect'], '-o', output, '-L' + BASE + '/lib', '-L' + CFG['gcc_lib'] + '/m68040',
           BASE + '/lib/startup.o', *extra, *wraps,
           '--start-group', *[i for i in inputs if i not in ('--start-group', '--end-group')],
           *([str(STDCXX / 'src/.libs/libstdc++.a')] if cxx else []),
           str(port / 'libqrx_msun.a'), str(port / 'libqrx_bridges.a'), '-lcrtprog', '-lcrt', '-lstdlib', *libs, '-lpthread', '-lm', '-lexec',
           '-lstdc.static', '-lgcc', '--end-group']
    rc = subprocess.call(cmd)
    if rc or not cxx:
        return rc
    merged = output + '.merged'
    rc = subprocess.call([CFG['ld'], '-r', '--force-group-allocation', '-T', str(HERE / 'merge-sections.ld'),
                          output, '-o', merged])
    if rc == 0:
        os.replace(merged, output)
    return rc


if __name__ == '__main__':
    sys.exit(main())
