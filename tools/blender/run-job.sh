#!/bin/bash
# Copies bin/blender into the test tree as an AROS program and runs a job script in the guest.
# usage: run-job.sh <job script> <timeout s> [engine options]
set -eu
W=/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b
HERE=$(cd "$(dirname "$0")" && pwd)
T=$W/tree
"$W/elf2hunk" "$W/build/bin/blender" "$T/blender" 2>&1 | grep -v '^$' || true
chmod +x "$T/blender"
cp "$1" "$T/S/job"
LIMIT=$2; shift 2
QRX_APPS=$W "$HERE/run-guest.sh" "$T" "$LIMIT" "$@"
