from pathlib import Path
import re,sys
root=Path(__file__).resolve().parent/'diagnostics-v6.6'/'tools'
apps=['qrx_web.c','qrx_ai.c','qrx_unified_apps.c','ares_antivirus.c']
for n in apps:
 s=(root/n).read_text()
 if 'WFLG_BORDERLESS' in s and 'qrx_theme.h' not in s: raise SystemExit(f'{n}: borderless app bypasses shared theme')
 if re.search(r'SetAPen\s*\([^,]+,\s*[0-3]\s*\)',s): raise SystemExit(f'{n}: literal low palette pen bypass remains')
h=(root/'qrx_theme.h').read_text()
for tok in ['QP_BAR 224','QP_PANEL 227','QP_TEXT 229','QP_RED 234','QP_BUTTON 239']:
 if tok not in h: raise SystemExit('shared palette mismatch: '+tok)
print('UNIFIED-THEME-GATE PASS: shared QRXDesktop palette 224..244, no direct low-pen chrome in QRX apps')
