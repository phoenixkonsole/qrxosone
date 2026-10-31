#!/usr/bin/env python3
"""Build AROS's own linker/converter around a generic m68k ELF GCC and pinned SDK.

No root, network, source edits, Linux target libc, or verification bypass needed.
This is an experimental adapter, validated separately from Deadwood's GCC 10.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def verified(directory, name):
    entries = dict((parts[1].lstrip('*'), parts[0]) for line in
                   (directory / 'SHA256SUMS').read_text().splitlines()
                   if len(parts := line.split(maxsplit=1)) == 2)
    path = directory / name
    if hashlib.sha256(path.read_bytes()).hexdigest() != entries[name]:
        raise RuntimeError(f'checksum mismatch: {path}')
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('work', type=Path)
    ap.add_argument('--cross-prefix', default='m68k-linux-gnu-')
    ap.add_argument('--host-cc', default='cc')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    bindir = work / 'bin'
    bindir.mkdir(exist_ok=True)
    config = bindir / 'toolchain.json'
    if config.exists() and json.loads(config.read_text()).get('kind') != 'qrx-experimental-aros-sdk':
        raise RuntimeError('refusing to overwrite another toolchain configuration')
    root = repo / 'upstream/aros-deadwood'
    sdk = verified(root / 'm68k-build-5b5fd4c', 'aros-m68k-sdk-5b5fd4c.tar.xz')
    source = verified(root / 'source', 'deadwood-AROS-master-5b5fd4cfd4f0.tar.xz')
    # Only pinned, checksummed archives; safe extraction rejects path escapes.
    with tarfile.open(sdk) as tf:
        tf.extractall(work / 'sdk', filter='data')
    with tarfile.open(source) as tf:
        names = ('deadwood-AROS-5b5fd4c/tools/collect-aros',
                 'deadwood-AROS-5b5fd4c/tools/elf2hunk')
        tf.extractall(work / 'source', members=[m for m in tf if
                      any(m.name == n or m.name.startswith(n + '/') for n in names)], filter='data')
    paths = {}
    for tool in ('gcc', 'ar', 'as', 'ld', 'nm', 'objdump', 'strip'):
        path = shutil.which(args.cross_prefix + tool)
        if not path:
            raise RuntimeError(f'missing cross tool: {args.cross_prefix + tool}')
        paths[tool] = str(Path(path).resolve())
    collectsrc = work / 'source/deadwood-AROS-5b5fd4c/tools/collect-aros'
    env = ''.join(f'#define {macro}_NAME {json.dumps(paths[tool])}\n' for macro, tool in
                  [('LD', 'ld'), ('STRIP', 'strip'), ('NM', 'nm'), ('OBJDUMP', 'objdump')])
    env += '#define TARGET_CPU_m68k\n#define OBJECT_FORMAT "-mm68kelf"\n'
    (collectsrc / 'env.h').write_text(env)
    sdkdir = work / 'sdk/Development'
    collect = bindir / 'collect-aros'
    subprocess.run([args.host_cc, '-std=gnu99', '-O2',
                    '-DOBJLIBDIR=' + json.dumps(str(sdkdir / 'lib')),
                    *[str(collectsrc / (s + '.c')) for s in
                      ('collect-aros', 'gensets', 'misc', 'backend-generic', 'docommand-exec')],
                    '-o', str(collect)], check=True)
    subprocess.run([args.host_cc, '-O2', str(work / 'source/deadwood-AROS-5b5fd4c/tools/elf2hunk/elf2hunk.c'),
                    '-o', str(bindir / 'elf2hunk')], check=True)
    def query(option):
        return subprocess.check_output([paths['gcc'], option], text=True).strip()
    abi = bindir / 'aros-abi.h'
    abi.write_text('#include <aros/cpu.h>\n#undef __AROS_FP_REG\n#undef __AROS_FP_SREG\n'
                   '#define __AROS_FP_REG A6\n#define __AROS_FP_SREG "A6"\n')
    # AROS C library stubs return pointers in D0; generic Linux m68k GCC
    # expects A0. These fixed-stack-argument bridges preserve D0 and also set A0.
    # This is a bounded SDK adapter, not a claim of arbitrary C ABI compatibility.
    words = json.loads(Path(__file__).with_name('pointer-return-abi.json').read_text())
    listing = subprocess.check_output([paths['nm'], '-g', '--defined-only',
        str(sdkdir / 'lib/libcrt.a'), str(sdkdir / 'lib/libstdlib.a')], text=True)
    exported = {line.split()[-1] for line in listing.splitlines() if len(line.split()) >= 3}
    words = {name: n for name,n in words.items() if name in exported}
    assembly = ['.text']
    for name,n in words.items():
        assembly += ['.globl __wrap_' + name, '__wrap_' + name + ':',
                     'movem.l %a2/%a6,-(%sp)', 'lea 12(%sp),%a2']
        assembly += [f'move.l {i*4}(%a2),-(%sp)' for i in reversed(range(n))]
        assembly += ['jsr __real_' + name]
        if n:
            assembly += [f'lea {n*4}(%sp),%sp']
        assembly += ['movem.l (%sp)+,%a2/%a6', 'move.l %d0,%a0', 'rts']
    bridge_source = bindir / 'pointer-bridges.S'
    bridge_source.write_text('\n'.join(assembly) + '\n')
    bridges = bindir / 'pointer-bridges.o'
    subprocess.run([paths['gcc'], '-m68020', '-c', str(bridge_source), '-o', str(bridges)], check=True)
    runtime = bindir / 'int64-runtime.o'
    runtime_source = repo / 'qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest/x86-runtime-22.5.12/core/qrx_int64_runtime.c'
    subprocess.run([paths['gcc'], '-m68060', '-O2', '-fno-builtin', '-fno-pic', '-fno-PIC',
                    '-fno-optimize-sibling-calls', '-c', str(runtime_source), '-o', str(runtime)], check=True)
    cfg = dict(kind='qrx-experimental-aros-sdk', sdk=str(sdkdir), abi_header=str(abi), **paths,
               gcc_include=str(Path(query('-print-file-name=include')).resolve()),
               gcc_lib=str(Path(query('-print-libgcc-file-name')).resolve().parent),
               collect=str(collect), runtime=str(runtime), bridges=str(bridges), wrap_flags=['--wrap=' + n for n in words], gcc_version=query('--version').splitlines()[0],
               sdk_sha256=hashlib.sha256(sdk.read_bytes()).hexdigest(),
               source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    config.write_text(json.dumps(cfg, indent=2) + '\n')
    wrapper = bindir / 'aros-gcc'
    shutil.copyfile(Path(__file__).with_name('aros-gcc.py'), wrapper)
    wrapper.chmod(0o755)
    print(f'Prepared {bindir}; SDK/source SHA-256 verified. This is not the Deadwood GCC distribution.')


if __name__ == '__main__':
    main()
