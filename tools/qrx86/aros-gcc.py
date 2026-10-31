#!/usr/bin/env python3
"""Narrow AROS SDK compile/link adapter; not a replacement Deadwood GCC release.

Generated configuration lives beside this installed script. Build with the
repository bootstrap tool. Only the compile/link operations used by QRX scripts
are supported. Never link Linux startup/libc into an AROS executable.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    cfg = json.loads(Path(__file__).with_name('toolchain.json').read_text())
    base = cfg['sdk']
    cc = cfg['gcc']
    flags = ['-include', cfg['abi_header'], '-fomit-frame-pointer', '-D__AROS__', '-DAMIGA', '-D_AMIGA', '-U__linux__', '-U__linux',
             '-Ulinux', '-U__gnu_linux__', '-U__unix__', '-U__unix', '-Uunix',
             # Debian injects Linux time64 macros even with -nostdinc. They
             # must not change the AROS SDK's file/time structure ABI.
             '-U_TIME_BITS', '-U_FILE_OFFSET_BITS', '-nostdinc']
    for inc in ('/include/aros/posixc', '/include/aros/stdc', '/include'):
        flags += ['-isystem', base + inc]
    flags += ['-isystem', cfg['gcc_include']]
    args = sys.argv[1:]
    if any(a in args for a in ('-c', '-E', '-S', '--version')):
        return subprocess.call([cc, *(flags[2:] if any(a.endswith('.S') for a in args) else flags), *args])
    output, compileflags, inputs, i = 'a.out', [], [], 0
    while i < len(args):
        arg = args[i]
        if arg == '-o':
            output = args[i + 1]
            i += 2
            continue
        if arg.endswith(('.c', '.S', '.o', '.a')) or arg.startswith(('-l', '-L')):
            inputs.append(arg)
        else:
            compileflags.append(arg)
        i += 1
    with tempfile.TemporaryDirectory(prefix='qrx-aros-') as tmp:
        objects = []
        for index, arg in enumerate(inputs):
            if arg.endswith(('.c', '.S')):
                obj = str(Path(tmp) / f'{index}.o')
                subprocess.run([cc, *(flags[2:] if arg.endswith('.S') else flags), *compileflags, '-c', arg, '-o', obj], check=True)
                objects.append(obj)
            else:
                objects.append(arg)
        cpu = next((a for a in reversed(compileflags) if a in ('-m68020','-m68030','-m68040','-m68060')), '-m68020')
        runtime = [cfg['runtime']] if cpu == '-m68060' else []
        libraries = Path(base, 'lib/auto').read_text().splitlines()[1].split()
        cmd = [cfg['collect'], '-o', output, '-L' + base + '/lib',
               '-L' + cfg['gcc_lib'], base + '/lib/startup.o', cfg['bridges'], *runtime,
               *cfg['wrap_flags'], *objects,
               '--start-group', '-lcrtprog', '-lcrt', '-lstdlib', *libraries,
               '-lexec', '-lstdc.static', '-lgcc', '--end-group']
        return subprocess.call(cmd)


if __name__ == '__main__':
    sys.exit(main())
