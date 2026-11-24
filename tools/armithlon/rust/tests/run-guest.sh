#!/bin/bash
# Runs one m68k64 AROS program as C:T in the QRX OS One 64 guest on the x86-64 reference machine
# (qrxboot, two EC64 cores) and prints what it wrote. Usage: run-guest.sh <program> [seconds]
# QRX_AROS64: the AROS 64 build (bin/native-m68k64), QRXBOOT: the qrxboot binary.
B=${QRX_AROS64:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64}; A=$B/AROS; CORE=$B/gen/rom/boot/core.elf
QRXBOOT=${QRXBOOT:-/opt/qrx-os-one/armx86/build/qrxboot}
BOOT="$(cd "$(dirname "$0")/../../aros-qrx/boot" && pwd)"
export PATH="${QRX_AROS64_TOOLS:-$B/../linux-x86_64/tools}:$PATH"
W=$(mktemp -d); SYS=$W/sys; mkdir -p $SYS/C $SYS/Libs $SYS/S $SYS/T; echo m68k64 > $SYS/AROS.boot
cp $A/Libs/crt.library $A/Libs/stdlib.library $A/Libs/m.library $A/Libs/locale.library $A/Libs/iffparse.library \
   $A/Libs/uuid.library $SYS/Libs/
cp $A/C/* $SYS/C/; printf 'SetClock LOAD\n' > $SYS/S/Startup-Sequence
cp "$1" $SYS/C/T; chmod +x $SYS/C/T
python3 -I $BOOT/mkffsimg.py $SYS $W/t.img 32 QRXSYS >/dev/null
QRXBOOT_DISK=$W/t.img QRXBOOT_CMDLINE=econsole-startup QRXBOOT_INPUT="C:T\nshutdown\n" $QRXBOOT $CORE ${2:-60} 2 512 \
  $A/Libs/utility.library $A/Libs/aros.library $A/boot/native/Devs/battclock.resource $A/boot/native/Devs/timer.device \
  $A/boot/native/Libs/expansion.library $A/Devs/bootloader.resource $A/Libs/dos.library $A/Devs/lddemon.resource \
  $A/L/econsole-handler $A/Devs/FileSystem.resource $A/L/afs-handler $A/Devs/qrxdisk.device $A/Devs/shell.resource \
  $A/Devs/shellcommands.resource $A/Devs/dosboot.resource > $W/out.txt 2>&1
sed -n '/1> C:T/,/1> shutdown/p' $W/out.txt | sed '1d;$d'; grep -E "Software Failure" $W/out.txt | head -1
rm -rf $W
