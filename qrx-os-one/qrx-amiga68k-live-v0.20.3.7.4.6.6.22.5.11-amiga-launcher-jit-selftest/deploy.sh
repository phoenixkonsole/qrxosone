#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ $EUID -eq 0 ]] || { echo "ERROR: run with sudo/root" >&2; exit 1; }
echo "[QRX-v0.20.3.7.4.6.6.22.5.12-X86Runtime] Deploying embedded v0.20.3.7.4.6.5.9 baseline"
bash "$ROOT/base-v5.9/deploy.sh" "$@"
echo "[QRX-v0.20.3.7.4.6.6.22.5.12-X86Runtime] Adding Diagnostics Center / Desktop Foundation"
python3 "$ROOT/validate-6.6.21.py"
python3 "$ROOT/validate-theme-6.6.22.2.py"
python3 "$ROOT/validate-qrx86-6.6.22.3.py"
python3 "$ROOT/validate-qrx86-6.6.22.4.py"
python3 "$ROOT/validate-qrx86-6.6.22.5.py"
bash "$ROOT/selftest-qrx86-6.6.22.5.sh"
python3 -B "$ROOT/validate-22.5.11.py"
python3 -B "$ROOT/validate-22.5.12.py"
if ! bash "$ROOT/diagnostics-v6.6/tools/fetch-amissl-5.27.sh"; then echo "[QRX] AmiSSL bootstrap unavailable; HTTPS remains fail-closed"; fi
bash "$ROOT/diagnostics-v6.6/deploy.sh"
echo "[QRX-v0.20.3.7.4.6.6.22.5.12-X86Runtime] DONE"
