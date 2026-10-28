#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parents[1]; d=(r/'deploy.sh').read_text(); p=(r/'tools/patch-qrxdesktop-flat-retro-ui.py').read_text()
assert 'v0.20.3.7.4.6.5.9' in d
assert 'patch-qrxdesktop-requester-free-clean-host.py' in d
assert 'patch-qrxdesktop-flat-retro-ui.py' in d
assert 'OCS 2-bit baseline' in p and 'qrx_flat_button' in p and 'qrx_flat_frame' in p
print('v0.20.3.7.4.6.5.9 validation OK')
