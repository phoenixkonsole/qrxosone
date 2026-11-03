#!/usr/bin/env python3
from pathlib import Path
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).parent))
from importlib.machinery import SourceFileLoader
m=SourceFileLoader('qrx_x11_input',str(Path(__file__).parent/'patch-qrx86-x11-input.py')).load_module()
with tempfile.TemporaryDirectory() as d:
    f=Path(d)/'qrx-os-one/branch/x86-runtime-22.5.12/linux/qrx_x11.c'
    f.parent.mkdir(parents=True)
    # Exactly the sections from the audited X11 code that the patch touches.
    f.write_text('''#include "qrx_x11.h"\n#include "qrx_x11_bridge.h"\n'''+
        '''void qxx_pump(qxx *x) { (void)x; }\n'''+
        '''void qxx_free(qxx *x) {\n    unsigned i;\n    if (!x)\n        return;\n    for (i=0;i<32;i++) free(x->windows[i].pixels);\n}\n''')
    a,changed=m.patch(Path(d));assert changed and a==f
    b,changed=m.patch(Path(d));assert not changed and b==f
    assert f.read_text().count(m.MARK)==1
    data=f.read_text();f.write_text(data.replace('qrx_x11_input_shutdown(x);',''))
    try:m.patch(Path(d));raise AssertionError('Must reject partially applied patch')
    except ValueError:pass
    f.write_text('int broken;')
    try:m.patch(Path(d));raise AssertionError('Must reject source drift')
    except ValueError:pass
print('PASS QRX86 X11 input patch: apply, idempotence, partial, drift')
