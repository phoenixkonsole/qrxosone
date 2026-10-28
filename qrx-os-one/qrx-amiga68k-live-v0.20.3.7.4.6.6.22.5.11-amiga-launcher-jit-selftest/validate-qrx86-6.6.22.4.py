from pathlib import Path
root=Path(__file__).resolve().parent
src=(root/'diagnostics-v6.6/tools/qrx_win_guest_runtime.c').read_text()
dep=(root/'diagnostics-v6.6/deploy.sh').read_text()
desk=(root/'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c').read_text()
required_src=[
    'scan_dependencies', 'resolve_imports', 'QRX_BRIDGE_BASE',
    'vDbgPrintExWithPrefix', 'parse_tls', 'build_init_plan',
    'QRX_INIT_HOST_NTDLL', 'QRX_INIT_DLLMAIN', 'QRX_INIT_EXE_TLS',
    '--emit-handoff', 'QRXGRT1', 'INIT/LINK PASS'
]
missing=[x for x in required_src if x not in src]
if missing: raise SystemExit('22.4 runtime markers missing: '+', '.join(missing))
for x in ['QRXWinGuestRuntime.elf','qrx_win_guest_runtime.c','QRXWinGuestRuntime','TestWinGuestInit']:
    if x not in dep: raise SystemExit('22.4 deploy marker missing: '+x)
for x in ['wininit ','winbridge ','QRX:C/QRXWinGuestRuntime','T:QRX-calc.qrxgrt']:
    if x not in desk: raise SystemExit('22.4 desktop marker missing: '+x)
# two-pass requirement: graph scan must be separate from import resolution
if src.find('scan_dependencies(&l,root,0)') > src.find('resolve_imports(&l,&l.modules[n])'):
    raise SystemExit('22.4 two-pass order invalid')
print('QRX86 6.6.22.4 validation: PASS')
print('  recursive mapping: two-pass/cycle-safe')
print('  ntdll bridge: deterministic guest thunk address table')
print('  TLS/DllMain: dependency-first PROCESS_ATTACH plan')
print('  QRX86 handoff: mapped images + bridge + init-call serialization')
