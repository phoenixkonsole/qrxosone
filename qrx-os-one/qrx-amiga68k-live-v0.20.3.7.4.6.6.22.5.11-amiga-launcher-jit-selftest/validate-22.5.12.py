#!/usr/bin/env python3
"""QRX OS One 22.5.12 validator: x86 runtime (qrx86core, qrx-i386, qrx-win32),
Amiga application scenarios, deploy and desktop wiring.

Static checks always run.  If gcc with -m32 support is available, the integer
and x87 differential tests run against native i386 execution (host only - this
is NOT evidence for the Deadwood m68k target; see test-evidence-22.5.12/)."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent
x86 = root / 'x86-runtime-22.5.12'
fails = 0


def need(rel, *needles):
    global fails
    p = root / rel
    if not p.exists():
        print(f'FAIL: missing {rel}')
        fails += 1
        return ''
    t = p.read_text(errors='replace')
    for n in needles:
        if n not in t:
            print(f'FAIL: {rel}: missing {n!r}')
            fails += 1
    return t


need('x86-runtime-22.5.12/core/qrx86core.h', 'x86_call', 'x86_fpu_push', 'x86_d2i64', 'tlb_rtag')
need('x86-runtime-22.5.12/core/qrx86core.c', 'fpu_exec', 'trig_range', 'string_op', 'case 0xC7:', 'shld(')
need('x86-runtime-22.5.12/core/qrx_bigstack.h', 'NewStackSwap')
need('x86-runtime-22.5.12/linux/qrx86linux.c', 'run_slice', 'LX_PIPE_MAX', 'set_thread_area', 'proc_fork', 'do_getdents', 'put_statx')
need('x86-runtime-22.5.12/win32/user.c', 'create_dialog', 'menu_popup_proc', 'radio_group_clear', 'user_snapshot', 'TranslateAcceleratorW')
need('x86-runtime-22.5.12/win32/gdi.c', 'qw_bitmap_from_dib', 'gdi_drawtext', 'BitBlt')
need('x86-runtime-22.5.12/win32/kernel.c', 'qfmt', 'qscan', 'RegSetValueExW', '__wgetmainargs')
need('x86-runtime-22.5.12/win32/backend_aros.c', 'WritePixelArray', 'MapRawKey', 'WA_RMBTrap')
need('x86-runtime-22.5.12/win32/qrxwin_fontdata.h', 'AROS Public License', 'qfont_table')
need('x86-runtime-22.5.12/build-target.sh', '-fno-optimize-sibling-calls', '000003f3', 'qrx-win32', 'qrx-i386', 'build-qmath.sh')
need('x86-runtime-22.5.12/qmath/qmath.h', '#define floor qm_floor', '#define frexp qm_frexp')
need('x86-runtime-22.5.12/qmath/COPYRIGHT.musl', 'MIT')
need('x86-runtime-22.5.12/core/qrx86core.c', '../qmath/qmath.h', 'nanfix')
need('x86-runtime-22.5.12/win32/kernel.c', 'qf_format', 'qf_strtod')
need('x86-runtime-22.5.12/X86-RUNTIME-22.5.12.txt', 'ReactOS 0.4.15 contains NO stand-alone clock program', 'NOT a proof')
need('diagnostics-v6.6/deploy.sh', 'REL="0.20.3.7.4.6.6.22.5.12"', 'x86-runtime-22.5.12', 'qrx-i386 qrx-win32', 'qrxclock.exe')
need('deploy.sh', 'validate-22.5.12.py', 'validate-22.5.11.py')
need('base-v5.9/tools/qrx_desktop.canonical-6.6.20.c', '"i386","win32","rechner","minesweeper","uhr"', 'QRX:C/qrx-win32 QRX:Windows/ReactOS/System32/calc.exe',
     '"amigarun","amigacheck"')
need('apps-22.5.12/x86/User-Startup', 'qrx-i386', 'qrx-win32', 'QRXScreenGrab')
need('apps-22.5.12/amiga/prepare-overlays.sh', 'DOpus5_104_os3.lha', 'HippoPlayer')

# sidecars
for f, side in [('base-v5.9/tools/qrx_desktop.canonical-6.6.20.c', 'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c.sha256'),
                ('x86-runtime-22.5.12/win32/apps/prebuilt/qrxclock.exe', 'x86-runtime-22.5.12/win32/apps/prebuilt/qrxclock.exe.sha256')]:
    real = hashlib.sha256((root / f).read_bytes()).hexdigest()
    want = (root / side).read_text().split()[0]
    if real != want:
        print(f'FAIL: SHA-256 sidecar mismatch for {f}')
        fails += 1
    else:
        print(f'PASS: sidecar {f}')

# The experimental backend is separate and explicitly opt-in. The default
# interpreter remains available on every host; native execution must fail closed.
need('x86-runtime-22.5.12/core/qrx86jit.h', 'XJ_NATIVE', 'xj_profile', 'xj_fresh',
     'CacheClearE', 'PROT_READ | PROT_EXEC', 'if (e.bad)', 'c->jb || !xj_profile',
     'b->count > c->left', '(uint32_t)(stop - b->eip)', 'base_icount')
need('x86-runtime-22.5.12/core/qrx86core.c', 'run_loop_jit', 'if (c->jit)', 'x86_jit_disable(c)')
need('x86-runtime-22.5.12/win32/qrx_win32_main.c', 'experimental', 'interpreter fallback', 'x86_jit_enable')
need('diagnostics-v6.6/tools/qrx86_grt_exec.c', 'native JIT quarantined', 'v.jit_enabled=0;')

# host differential tests
gcc = shutil.which('gcc')
if gcc and subprocess.run([gcc, '-m32', '-x', 'c', '-o', os.devnull, '-'], input=b'int main(void){return 0;}', capture_output=True).returncode == 0:
    with tempfile.TemporaryDirectory() as td:
        t = pathlib.Path(td)
        cf = ['-std=gnu99', '-O2', '-w']
        subprocess.run(['bash', x86 / 'qmath/build-qmath.sh', gcc, 'ar', t], check=True, capture_output=True)
        subprocess.run([gcc, *cf, '-o', t / 'qrx-i386', x86 / 'core/qrx86core.c', x86 / 'linux/qrx86linux.c', x86 / 'linux/qrx_i386_main.c', x86 / 'linux/qrx_x11.c', t / 'libqmath.a', '-lm'], check=True)
        subprocess.run([sys.executable, x86 / 'tests/gen-diff-test.py', t / 'd.S', t / 'd.meta', '22512', '6'], check=True, capture_output=True)
        subprocess.run([gcc, '-m32', '-nostdlib', '-static', '-o', t / 'd', t / 'd.S'], check=True)
        nat = subprocess.run([t / 'd'], capture_output=True)
        if nat.returncode != 0:
            print('SKIP: host cannot execute i386 binaries')
        else:
            (t / 'nat.bin').write_bytes(nat.stdout)
            (t / 'emu.bin').write_bytes(subprocess.run([t / 'qrx-i386', t / 'd'], capture_output=True).stdout)
            r = subprocess.run([sys.executable, x86 / 'tests/compare-diff.py', t / 'd.meta', t / 'nat.bin', t / 'emu.bin'], capture_output=True, text=True)
            print(('PASS' if r.returncode == 0 else 'FAIL') + ': integer differential: ' + r.stdout.strip().splitlines()[-1])
            fails += r.returncode != 0
            subprocess.run([gcc, '-m32', '-static', '-O1', '-mfpmath=387', '-fno-pie', '-o', t / 'f', x86 / 'tests/fpu-diff.c', '-lm'], check=True)
            (t / 'fn.txt').write_bytes(subprocess.run([t / 'f'], capture_output=True).stdout)
            (t / 'fe.txt').write_bytes(subprocess.run([t / 'qrx-i386', t / 'f'], capture_output=True).stdout)
            r = subprocess.run([sys.executable, x86 / 'tests/compare-fpu.py', t / 'fn.txt', t / 'fe.txt'], capture_output=True, text=True)
            print(('PASS' if r.returncode == 0 else 'FAIL') + ': x87 differential: ' + r.stdout.strip().splitlines()[-1])
            fails += r.returncode != 0
else:
    print('SKIP: gcc -m32 not available (differential tests)')

print('22.5.12 VALIDATION: ' + ('PASS' if not fails else f'FAIL ({fails})') + ' (host checks; m68k evidence in test-evidence-22.5.12/)')
sys.exit(1 if fails else 0)
