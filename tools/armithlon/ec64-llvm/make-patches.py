#!/usr/bin/env python3
"""Writes the two patches from a working LLVM tree. The tree's own archive is the base.
Usage: make-patches.py <work dir>   (holding dl/ and llvm-project-22.1.2.src/)"""
import difflib, os, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
work = os.path.abspath(sys.argv[1])
V = 'llvm-project-22.1.2.src'
CORE = ['llvm/CMakeLists.txt', 'llvm/include/llvm/TargetParser/Triple.h', 'llvm/lib/TargetParser/Triple.cpp',
        'llvm/lib/TargetParser/TargetDataLayout.cpp', 'llvm/lib/BinaryFormat/ELF.cpp',
        'llvm/include/llvm/Object/ELFObjectFile.h', 'llvm/include/llvm/IR/RuntimeLibcalls.td',
        'clang/lib/Basic/Targets.cpp', 'clang/lib/Basic/Targets/M68k.h', 'clang/lib/CodeGen/CodeGenModule.cpp',
        'clang/lib/CodeGen/Targets/M68k.cpp', 'clang/lib/Driver/ToolChains/CommonArgs.cpp',
        'lld/ELF/Arch/M68k64.cpp', 'lld/ELF/CMakeLists.txt', 'lld/ELF/Target.h', 'lld/ELF/Target.cpp',
        'lld/ELF/Driver.cpp', 'lld/ELF/InputFiles.cpp', 'lld/ELF/ScriptParser.cpp',
        # AROS as an OS (from AROS's tools/crosstools/llvm clang-20 and llvm-20 patches)
        'clang/include/clang/Options/Options.td', 'clang/lib/Basic/Targets/OSTargets.h',
        'clang/lib/Driver/CMakeLists.txt', 'clang/lib/Driver/Driver.cpp',
        'clang/lib/Driver/ToolChains/AROS.cpp', 'clang/lib/Driver/ToolChains/AROS.h',
        'clang/lib/Lex/InitHeaderSearch.cpp',
        # C++ runtime for AROS (build-runtimes.sh): cxa_atexit default, libunwind for m68k64, emutls,
        # libc++ and libc++abi on AROS
        'clang/lib/Driver/ToolChains/Clang.cpp',
        'libunwind/include/__libunwind_config.h', 'libunwind/include/libunwind.h', 'libunwind/src/Registers.hpp',
        'libunwind/src/UnwindCursor.hpp', 'libunwind/src/libunwind.cpp', 'libunwind/src/config.h',
        'libunwind/src/UnwindRegistersSave.S', 'libunwind/src/UnwindLevel1-gcc-ext.c', 'libunwind/src/UnwindRegistersRestore.S',
        'libcxx/src/system_error.cpp', 'libcxx/src/chrono.cpp',
        'libcxxabi/src/CMakeLists.txt', 'libcxxabi/src/cxa_thread_atexit.cpp',
        'compiler-rt/lib/builtins/emutls.c']
NEW = {'lld/ELF/Arch/M68k64.cpp', 'clang/lib/Driver/ToolChains/AROS.cpp', 'clang/lib/Driver/ToolChains/AROS.h'}
def lines(p):
    return open(p, encoding='utf8').read().splitlines(True) if os.path.exists(p) else []
with tempfile.TemporaryDirectory() as tmp:
    subprocess.run(['tar', 'xJf', f'{work}/dl/{V}.tar.xz', '-C', tmp, '--exclude=*/test/*',
                    *[f'{V}/{c}' for c in CORE if c not in NEW], f'{V}/llvm/lib/Target/M68k'], check=True)
    out = []
    for c in CORE:
        out += difflib.unified_diff(lines(f'{tmp}/{V}/{c}'), lines(f'{work}/{V}/{c}'), 'a/' + c, 'b/' + c)
    open(f'{here}/llvm-22.1.2-core.patch', 'w', encoding='utf8').writelines(out)
    subprocess.run([sys.executable, '-I', f'{here}/make-fork.py', f'{tmp}/{V}/llvm/lib/Target'], check=True)
    base, cur = f'{tmp}/{V}/llvm/lib/Target/M68k64', f'{work}/{V}/llvm/lib/Target/M68k64'
    names = set()
    for top in (base, cur):
        for d, _, fs in os.walk(top):
            names |= {os.path.relpath(os.path.join(d, f), top) for f in fs}
    out = []
    for n in sorted(names):
        p = 'llvm/lib/Target/M68k64/' + n
        out += difflib.unified_diff(lines(f'{base}/{n}'), lines(f'{cur}/{n}'), 'a/' + p, 'b/' + p)
    open(f'{here}/llvm-22.1.2-m68k64.patch', 'w', encoding='utf8').writelines(out)
