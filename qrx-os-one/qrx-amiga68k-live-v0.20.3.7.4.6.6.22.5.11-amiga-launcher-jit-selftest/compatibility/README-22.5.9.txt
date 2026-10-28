QRX 22.5.9 COMPATIBILITY FOUNDATION
AmigaOS 1.x-3.x: HUNK candidates are identified, not automatically compatible.
AmigaOS 4 PPC: no OS4 ELF loader, ABI bridge or PPC CPU runtime yet.
Linux PPC: no PPC execution, Linux syscall or dynamic linker emulation yet.
qrx-binary-identify.py is a host-side read-only classifier, not installed into AmigaOS.
QRX86 native JIT remains quarantined until target validation; --jit=on is acknowledged but executes interpreter.
