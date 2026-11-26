#!/bin/bash
# Runs one m68k64 AROS program as C:T in the QRX OS One 64 guest on the x86-64 reference machine
# (qrxboot, two EC64 cores) and prints what it wrote. Usage: run-guest.sh <program> [seconds]
# QRX_MEM_MB: guest memory (512), QRX_IMG_MB: system disk size (32), QRX_CMD: the command to run
# instead of C:T (the program, e.g. one copied in with QRX_TREES).
# QRX_CORES: EC64 cores (2). QRX_LIVE: a file that follows the console while the guest runs (qrxboot QRXBOOT_LIVE).
# QRX_AROS64: the AROS 64 build (bin/native-m68k64), QRXBOOT: the qrxboot binary.
B=${QRX_AROS64:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64}; A=$B/AROS; CORE=$B/gen/rom/boot/core.elf
QRXBOOT=${QRXBOOT:-/opt/qrx-os-one/armx86/build/qrxboot}
BOOT="$(cd "$(dirname "$0")/../../aros-qrx/boot" && pwd)"
export PATH="${QRX_AROS64_TOOLS:-$B/../linux-x86_64/tools}:$PATH"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; trap 'exit 143' TERM INT; SYS=$W/sys; mkdir -p $SYS/C $SYS/Libs $SYS/S $SYS/T; echo m68k64 > $SYS/AROS.boot
cp $A/Libs/crt.library $A/Libs/stdlib.library $A/Libs/m.library $A/Libs/locale.library $A/Libs/iffparse.library \
   $A/Libs/uuid.library $SYS/Libs/
[ -f $A/Libs/z1.library ] && cp $A/Libs/z1.library $SYS/Libs/
cp $A/C/* $SYS/C/; mkdir -p $SYS/L $SYS/Devs/DOSDrivers
# PIPE: for pipe() (std::process): qrxpipe-handler (aros-qrx/qrxpipe, built by build-linux.sh)
[ -f $A/L/qrxpipe-handler ] && cp $A/L/qrxpipe-handler $SYS/L/ && printf 'Handler = L:qrxpipe-handler\nStackSize = 16384\nPriority = 5\nGlobVec = -1\n' > $SYS/Devs/DOSDrivers/PIPE
printf 'SetClock LOAD\n' > $SYS/S/Startup-Sequence
# this boot (econsole-startup) does not run S:Startup-Sequence: the setup goes in as typed commands
PRE='Assign L: SYS:L\nAssign DEVS: SYS:Devs\nAssign T: SYS:T\nMount DEVS:DOSDrivers/PIPE\n'"${QRX_PRE:-}"   # QRX_PRE: more commands
cp "$1" $SYS/C/T; chmod +x $SYS/C/T
# QRX_FILES: more files for the test system, copied into T: (SYS:T)
for f in ${QRX_FILES:-}; do cp "$f" $SYS/T/; done
# QRX_TREES: more directories, "source:SYS-relative destination" pairs (e.g. dist/bin:Firefox, fonts:Fonts/TrueType)
for t in ${QRX_TREES:-}; do mkdir -p "$SYS/${t#*:}"; cp -r "${t%%:*}/." "$SYS/${t#*:}/"; done
python3 -I $BOOT/mkffsimg.py $SYS $W/t.img ${QRX_IMG_MB:-32} QRXSYS >/dev/null
# QRX_KEEP_DISK=<file>: the system disk as the guest left it (qrxboot writes it back), e.g. for
# aros-qrx/boot/ffsget.py to read a file the program wrote
[ -n "${QRX_KEEP_DISK:-}" ] && export QRXBOOT_DISK_WRITE=1
QRXBOOT_LIVE=${QRX_LIVE:-} QRXBOOT_DISK=$W/t.img QRXBOOT_CMDLINE=econsole-startup QRXBOOT_INPUT="${PRE}${QRX_CMD:-C:T} ${QRX_ARGS:-}\nshutdown\n" $QRXBOOT $CORE ${2:-60} ${QRX_CORES:-2} ${QRX_MEM_MB:-512} \
  $A/Libs/utility.library $A/Libs/aros.library $A/boot/native/Devs/battclock.resource $A/boot/native/Devs/timer.device \
  $A/boot/native/Libs/expansion.library $A/Devs/bootloader.resource $A/Libs/dos.library $A/Devs/lddemon.resource \
  $A/L/econsole-handler $A/Devs/FileSystem.resource $A/L/afs-handler $A/Devs/qrxdisk.device $A/Devs/shell.resource \
  $A/Devs/shellcommands.resource $A/Devs/dosboot.resource > $W/out.txt 2>&1
sed -n "\\|1> ${QRX_CMD:-C:T}|,\\|1> shutdown|p" $W/out.txt | sed '1d;$d'; grep -E "Software Failure" $W/out.txt | head -1
[ -n "$KEEP" ] && cp $W/out.txt $KEEP; [ -n "${QRX_KEEP_DISK:-}" ] && mv $W/t.img "$QRX_KEEP_DISK"; rm -rf $W
