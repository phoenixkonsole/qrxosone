from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text();needle='    WANDERER:Wanderer'
block='    If EXISTS QRX:C/QRXDesktop\n        Echo "QRX: starting native QRXDesktop primary session" >>RAM:QRX-Startup.log\n        QRX:C/QRXDesktop\n        Echo "QRX: QRXDesktop returned; starting Wanderer fallback" >>RAM:QRX-Startup.log\n    Else\n        Echo "QRX: QRXDesktop missing; starting Wanderer fallback" >>RAM:QRX-Startup.log\n    EndIf\n    WANDERER:Wanderer'
if 'starting native QRXDesktop primary session' not in s:
    if needle not in s: raise SystemExit('direct Wanderer launch not found')
    s=s.replace(needle,block,1)
p.write_text(s)
q=s.find('QRX:C/QRXDesktop');w=s.find('WANDERER:Wanderer',q)
assert q>=0 and w>q and 'Run QRX:C/QRXDesktop' not in s
print('QRX native boot gate OK')
