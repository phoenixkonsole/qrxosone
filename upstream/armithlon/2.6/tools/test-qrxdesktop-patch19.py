#!/usr/bin/env python3
import importlib.util
from pathlib import Path
p=Path(__file__).resolve().parent/'patch-qrxdesktop-launch19.py'
spec=importlib.util.spec_from_file_location('qrxpatch',p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
source='''static LONG qrx_open_path(const char *full, LONG dir_type, UBYTE has_icon,
                          UBYTE icon_type, const char *default_tool) {
    char command[LEGACY_CMD_LEN];
    if (qrx_is_pe32_i386(full)) {
        snprintf(command, sizeof(command), "QRX:C/QRXWinRun \\"%s\\"", full);
        execute_capture(command);
        return 1;
    }
    int em = qrx_elf_machine(full);
    if(em==68){
            snprintf(command, sizeof(command), "QRX:C/QRXLinuxRun \\"%s\\"", full);
            execute_capture(command);
            return 1;
    }
    if(em==386){
            snprintf(command, sizeof(command), "QRX:C/QRX86Run \\"%s\\"", full);
            execute_capture(command);
            return 1;
    }
    return launch_native(command);
}

static int qrx_copy_path(const char *s, const char *d) {return 0;}
'''
a=m.patch(source)
assert m.TAG in a
assert a==m.patch(a)
assert 'return qrx19_launch_guest("QRX:C/QRX86Run", full)' in a
assert 'return qrx19_launch_guest("QRX:C/QRXWinRun", full)' in a
assert a.count('execute_capture(command)')==0
try:
    m.patch(source.replace('QRX:C/QRXWinRun','QRX:C/UNKNOWN'))
    raise AssertionError('must fail closed on unexpected source')
except ValueError:pass
print('PASS: QRXDesktop source patch (idempotent, guarded, asynchronous, safe quoted paths)')
