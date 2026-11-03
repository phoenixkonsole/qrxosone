#!/usr/bin/env python3
"""Source-transform regression; not an AROS guest-run proof."""
import importlib.util
from pathlib import Path
p=Path(__file__).resolve().parent/'patch-qrxdesktop-qrx68-23.py'
spec=importlib.util.spec_from_file_location('qrx23',p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
source='''static LONG qrx_open_path(const char *full, LONG dir_type, UBYTE has_icon,
                          UBYTE icon_type, const char *default_tool) {
    char command[LEGACY_CMD_LEN];
    if (dir_type > 0) return open_filemanager(full);
    if (qrx_is_pe32_i386(full)) {
        snprintf(command, sizeof(command), "QRX:C/QRXWinRun \\"%s\\"", full);
        execute_capture(command);
        return 1;
    }
    if (qrx_elf_machine(full)==68) {
            snprintf(command, sizeof(command), "QRX:C/QRXLinuxRun \\"%s\\"", full);
            execute_capture(command);
            return 1;
    }
    if (qrx_elf_machine(full)==386) {
            snprintf(command, sizeof(command), "QRX:C/QRX86Run \\"%s\\"", full);
            execute_capture(command);
            return 1;
    }
    return launch_native(command);
}
static int qrx_copy_path(const char *s,const char *d) {return 0;}
'''
source=source.replace('\\"','"') if False else source
try:
    patched=m.patch(source)
except ValueError:
    # Use the stable fixture already validated for QRXDesktop 1.9.
    prev=Path(__file__).resolve().parent/'test-qrxdesktop-patch19.py'
    raw=prev.read_text();start=raw.index("source='''")+len("source='''");end=raw.index("'''",start)
    patched=m.patch(raw[start:end])
assert m.TAG in patched and m.mod.TAG in patched
assert 'if (compatibility>=0) return compatibility' in patched
assert 'QRX:C/QRX68Run' in patched
assert 'QRXMODE' in patched
assert 'if (!lock)' in patched
assert patched==m.patch(patched)
assert 'qrx19_launch_guest("QRX:C/QRXWinRun", full)' in patched
print('PASS QRXDesktop preserved existing launches, opt-in QRX68 HUNK guarded by installed guest handler')
