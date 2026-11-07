#!/usr/bin/env python3
"""Runs a Blender build tool for the cross build (CMAKE_CROSSCOMPILING_EMULATOR).

datatoc only copies bytes, so it runs as a host program. makesdna and makesrna produce data
that depends on pointer size and byte order: they run as real 68k programs in the guest, so
what they produce is what the target sees. Paths under the work directory reach the guest
through the APPS: HostFS mount.
"""
import os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('QRX_ROOT', '/Users/pascalpapara/Documents/GitHub/qrxosone'))
WORK = ROOT / '.build/blender-2.79b'
SRC = WORK / 'src/blender-2.79b'


def host_tool(name, sources, extra=()):
    out = WORK / 'hosttools' / name
    out.parent.mkdir(exist_ok=True)
    if not out.exists():
        tmp = out.with_name(f'{name}.{os.getpid()}')
        subprocess.check_call(['cc', '-O1', '-w', *map(str, sources), *extra, '-o', str(tmp)])
        os.replace(tmp, out)
    return out


def guest_path(arg):
    s = str(arg)
    if s.startswith(str(WORK) + '/'):
        return 'APPS:' + s[len(str(WORK)) + 1:]
    return s


def run_guest(exe, args):
    tree = WORK / 'tree'
    name = Path(exe).name
    subprocess.check_call([str(WORK / 'elf2hunk'), exe, str(tree / name)])
    os.chmod(tree / name, 0o755)
    job = ['C:QRXMoreMemory', 'C:Stack 2000000',
           f'SYS:{name} ' + ' '.join(f'"{guest_path(a)}"' for a in args) + f' >SYS:{name}.log',
           'If WARN', f'  Echo failed >SYS:{name}.failed', 'EndIf']
    (tree / 'S/job').write_text('\n'.join(job) + '\n')
    failed = tree / f'{name}.failed'
    failed.unlink(missing_ok=True)
    env = dict(os.environ, QRX_APPS=str(WORK))
    limit = os.environ.get('QRX_TOOL_LIMIT', '1200')
    rc = subprocess.call([str(HERE / 'run-guest.sh'), str(tree), limit], env=env)
    log = tree / f'{name}.log'
    if log.exists():
        sys.stdout.write(log.read_text(errors='replace')[-2000:])
    return 1 if rc or failed.exists() else 0


def main():
    exe, args = sys.argv[1], sys.argv[2:]
    name = Path(exe).name
    if name == 'datatoc':
        return subprocess.call([str(host_tool(name, [SRC / 'source/blender/datatoc/datatoc.c'])), *args])
    if name == 'datatoc_icon':
        png = subprocess.check_output(['/opt/homebrew/bin/pkg-config', '--cflags', '--libs', 'libpng'], text=True).split()
        return subprocess.call([str(host_tool(name, [SRC / 'source/blender/datatoc/datatoc_icon.c'], png)), *args])
    if name in ('makesdna', 'makesrna'):
        return run_guest(exe, args)
    print('run_tool: unknown tool', exe, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
