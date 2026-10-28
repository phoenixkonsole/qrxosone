from pathlib import Path
import sys,re
r=Path(__file__).resolve().parent;t=r/'diagnostics-v6.6/tools';d=(r/'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c').read_text()
checks={
'unified theme API': all('qrx_theme.h' in (t/x).read_text() for x in ['qrx_web.c','qrx_ai.c','qrx_unified_apps.c','ares_antivirus.c']),
'drive no volume rescan in paint': 'qrx_refresh_mounted_volumes();\n    text_at(rp, PEN_MUTED, 14, 106' not in d,
'alt-tab core': 'qrx_cycle_window' in d and 'IEQUALIFIER_LALT' in d,
'4 workspaces core': 'qrx_workspace_switch' in d,
'expose core': 'qrx_expose_toggle' in d,
'web RMB': 'MENUDOWN' in (t/'qrx_web.c').read_text() and 'draw_context' in (t/'qrx_web.c').read_text(),
'AI learned name': 'learn_name' in (t/'qrx_ai.c').read_text() and 'qrxai-user.cfg' in (t/'qrx_ai.c').read_text(),
'tooltips': all('qrx_theme_tooltip' in (t/x).read_text() for x in ['qrx_web.c','qrx_ai.c','qrx_unified_apps.c','ares_antivirus.c']),
'AmiSSL bootstrap': (t/'fetch-amissl-5.27.sh').exists(),
}
for k,v in checks.items(): print(('PASS ' if v else 'FAIL ')+k)
if not all(checks.values()): sys.exit(1)
