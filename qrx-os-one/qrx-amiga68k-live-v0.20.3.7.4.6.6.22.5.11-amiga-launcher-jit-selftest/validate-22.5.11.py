#!/usr/bin/env python3
"""QRX OS One 22.5.11 source/integration gates + host regression tests.

Runs only host-side checks.  Passing this script proves the portable logic and
the build/ISO gate wiring, NOT behaviour on Deadwood AROS m68k (see
TESTREPORT-22.5.11.txt for target results)."""
import hashlib, os, re, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parent
fail = []

def need(path, *tokens):
    p = root / path
    if not p.is_file():
        fail.append(f'missing file: {path}')
        return ''
    s = p.read_text(errors='replace')
    for t in tokens:
        if t not in s:
            fail.append(f'{path}: missing {t!r}')
    return s

rt = 'amiga-runtime-22.5.11'
need(f'{rt}/src/qrx_hunk.c', 'QRX_HUNK_RELOC32SHORT', 'relocation offset outside its hunk', 'qrx_dos_quote')
need(f'{rt}/src/qrx_amigarun.c', 'LoadSeg(', 'UnLoadSeg(', 'SystemTags(', 'FindResident(', 'IoErr()', 'Fault(',
     'cli_DefaultStack', 'amigacheck', 'pr_WindowPtr')
js = need(f'{rt}/src/qrx_jit_selftest.c', 'CacheClearE(', 'CacheClearU()', 'EXECUTE', 'PENDING', 'LOGDIR=',
          'does NOT enable the QRX86 JIT')
need(f'{rt}/src/qrx_jit_emit.h', '0x4EB9UL', '0x4E75UL', '0x7000UL')
need(f'{rt}/build-target.sh', 'QRX_ELF2HUNK', '000003f3', 'STACK-GATE', 'm68k-amigaos-gcc) echo "ERROR')
need(f'{rt}/target/TestAmigaRun', 'QRXTEST T01-check-valid', 'QRXTEST END')
for d in ['AMIGA-LAUNCHER-22.5.11.txt', 'JIT-SELFTEST-22.5.11.txt']:
    need(f'{rt}/{d}')
need('compatibility/PPC-ARCHITECTURE-PLAN-22.5.11.txt', 'PLAN ONLY', 'No PowerPC instruction is executed')

# QRX86 interpreter fixes + quarantine must still be present
ex = need('diagnostics-v6.6/tools/qrx86_grt_exec.c', 'static QRXVM v;', 'QRX_REP_CHUNK', 'int*resume',
          'native JIT quarantined', 'if(op>=0xb0&&op<=0xb7)', 'QRX86 WATCHDOG:', 'trace_lines<2000u')
if re.search(r'int main\(int argc,char\*\*argv\)\{QRXVM v;', ex):
    fail.append('QRXVM is a stack local again')
for f in ['qrx_win_guest_loader.c', 'qrx_win_guest_runtime.c']:
    s = need(f'diagnostics-v6.6/tools/{f}', 'static QRXLoader l;')
    if re.search(r'int main\([^)]*\)\s*\{\s*QRXLoader l;', s):
        fail.append(f'{f}: QRXLoader is a stack local again')

# Desktop integration (canonical source + provenance sidecar)
desk = need('base-v5.9/tools/qrx_desktop.canonical-6.6.20.c', '"amigarun","amigacheck"',
            'QRX:C/QRXAmigaRun CHECK %s', 'QRX:C/QRXAmigaRun %s',
            'QRX:C/QRX86GRTExec --jit=off --trace --max-steps 100000 T:QRX-wintrace.qrxgrt')
side = (root / 'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c.sha256').read_text().split()[0]
real = hashlib.sha256((root / 'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c').read_bytes()).hexdigest()
if side != real:
    fail.append('canonical desktop sidecar does not match source (deploy SHA gate would fail)')

# Fixes found while building/running on Deadwood m68k (22.5.11)
for f in ['qrx86_grt_exec.c', 'qrx_win_guest_loader.c', 'qrx_win_guest_runtime.c']:
    need(f'diagnostics-v6.6/tools/{f}', '!defined(_STDC_STDIO_H_) && !defined(_POSIXC_STDIO_H_)', 'typedef void QRX_FILE')
need('base-v5.9/tools/qrx_desktop.canonical-6.6.20.c', 'qrx_workspace_switch((int)code-1); return 1;',
     'SetDrMd(rp, JAM1);', 'qrx_free_wallpaper_cache();', 'BltBitMapRastPort(g_wall_cache', 'GetVar((STRPTR)"QRXSCREEN"',
     'g_welcome_autostart', '"welcome=%s\\n"')
need('diagnostics-v6.6/tools/qrx_theme.h', 'SetDrMd(r,JAM1)')
need('diagnostics-v6.6/tools/qrx_unified_apps.c', 'defined(APP_WRITER)||defined(APP_SHEET)||defined(APP_PRESENTER)')

# Deploy wiring
# 22.5.12: the release string moved on; 22.5.11 wiring must stay present in any 22.5.1x release
dep = need('diagnostics-v6.6/deploy.sh', 'REL="0.20.3.7.4.6.6.22.5.1', 'build-target.sh', 'QRXAmigaRun amigarun amigacheck qrx-jit-selftest',
           'FINAL ISO GATE: QRX:Tests/Amiga/', 'qrx-jit-selftest referenced from a startup/runtime script')
top = need('deploy.sh', 'validate-22.5.11.py')
base = need('base-v5.9/deploy.sh', 'ADF_SECTORS="${QRX_EMERGENCY_ADF_SECTORS:-3520}"', 'EBFHUNKROM="${QRX_EBFHUNKROM:-no}"')
# JIT selftest must not be wired into boot / ReactOS paths
for p in [root / 'base-v5.9/deploy.sh', root / 'base-v5.9/tools/patch-qrx-native-boot-6.6.13.py',
          root / 'base-v5.9/tools/patch-aros-startup-arosvision-bootstrap.py', root / 'diagnostics-v6.6/tools/qrx-system-test']:
    if 'qrx-jit-selftest' in p.read_text(errors='replace'):
        fail.append(f'{p.name} references qrx-jit-selftest (must stay opt-in)')
for m in re.finditer(r"cat > \"\$SYSROOT/QRX/S/(\w+)\" <<'?(\w+)'?\n(.*?)\n\2\n", dep, re.S):
    if 'qrx-jit-selftest' in m.group(3):
        fail.append(f'QRX:S/{m.group(1)} references qrx-jit-selftest')

# Every previous release file must still exist (cumulative rule)
prev = [l.split(None, 1)[1].strip() for l in (root / 'SHA256SUMS-6.6.22.5.7').read_text().splitlines() if l.strip()]
for rel in prev:
    if '__pycache__' in rel:
        continue
    if not (root / rel).exists():
        fail.append(f'cumulative file lost: {rel}')

if fail:
    print('22.5.11 SOURCE/INTEGRATION GATES: FAIL')
    for f in fail:
        print('  -', f)
    sys.exit(1)
print('22.5.11 SOURCE/INTEGRATION GATES: PASS')

tests = [
    ['bash', str(root / 'tests-22.5.11/test-qrx86-interpreter.sh')],
    [sys.executable, '-B', str(root / rt / 'tests/test-amiga-launcher-host.py')],
]
for t in tests:
    r = subprocess.run(t)
    if r.returncode:
        print('22.5.11 HOST TEST FAILED:', ' '.join(t))
        sys.exit(1)
r = subprocess.run(['bash', str(root / rt / 'tests/test-jit-emit-host.sh')])
if r.returncode not in (0, 77):
    print('22.5.11 HOST TEST FAILED: test-jit-emit-host.sh')
    sys.exit(1)
print('22.5.11 HOST TESTS: PASS (host GCC only; not evidence for Deadwood m68k)')
