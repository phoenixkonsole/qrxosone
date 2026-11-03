# Distribution gate

This is a SOURCE/DEVELOPMENT kit, not an end-user production ARMITHLON build.
Do not market the AROS ISO or the preview ISO as a working QRX OS One release.

Third-party emulator FS-UAE is installed by the user independently. It is GPL
software; if redistribution of modified or bundled versions is planned, provide
appropriate license notices, corresponding source and any other required items.
AROS and its ROMs have their own license obligations. QRX source may include
additional third-party components (ReactOS, BusyBox, demos, drivers), each of
which must be reviewed individually. The ARMITHLON zip intentionally excludes
such assets until the user fetches them from the private repository.

The QRX68100 test CPU is NOT the proprietary Apollo 68080 or SAGA. No trademark
or license to redistribute proprietary core implementations is implied.

For a real native chipset-less Mac/ARM Linux release, implement and verify:
complete 68020/040 CPU + FPU/MMU contract, a new AROS qrxvirt m68k BSP,
interrupts and PCI-style virtual-bus services, executable JIT cache coherence,
A64 native code safety, RTG/Metal/Vulkan host driver, AHI audio backend,
TCP/IP/network bridge, security sandboxing, SMP semantics and Mac/Linux tests.
