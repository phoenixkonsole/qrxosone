#!/bin/bash
cd "$(dirname "$0")" || exit 1
./armithlon doctor
./armithlon build && ./armithlon test
echo
read -r -p 'Press Enter to close... ' _
