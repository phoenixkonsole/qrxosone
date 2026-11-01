#!/bin/bash
# Double-click launcher on macOS: QRX firmware shell / real AROS desktop (FS-UAE).
cd "$(dirname "$0")" || exit 1
printf '\nQRX OS One / ARMITHLON 2.4\n'
printf '1) QRX68100 - local 68k firmware shell with persistent disk (offline)\n'
printf '2) AROS Amiga desktop via FS-UAE (external verified ROM/ISO required)\n'
printf '3) QRX68100 native firmware GUI (Cocoa/macOS, experimental)\n'
printf '4) AROS + QRX68 compatibility service (FS-UAE, guest runner required)\n'
printf 'Selection [1]: '
read -r select
case "$select" in
  ''|1) ./armithlon boot ; result=$?;;
  2) ./armithlon desktop ; result=$?;;
  3) ./armithlon gui ; result=$?;;
  4) printf 'Optional extracted AROS system directory (blank for live AROS): '
     read -r aros_dir
     if [[ -n "$aros_dir" ]]; then ./armithlon qrx68-aros-start "$aros_dir"; result=$?
     else ./armithlon qrx68-aros-start; result=$?; fi;;
  *) echo 'Invalid choice'; result=2;;
esac
printf '\nExit code: %s\nPress Enter to close...' "$result"
read -r _
exit "$result"
