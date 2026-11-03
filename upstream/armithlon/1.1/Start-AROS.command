#!/bin/bash
cd "$(dirname "$0")" || exit 1
./armithlon run-aros
status=$?
echo "Launcher exit code: $status"
read -r -p 'Press Enter to close... ' _
exit "$status"
