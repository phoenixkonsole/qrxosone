#!/usr/bin/env bash
# Reassemble the archived ReactOS 0.4.15 release images and verify them.
#   restore.sh [outdir]   (default: /tmp, where base-v5.9/deploy.sh looks for
#                          ReactOS-0.4.15-x86-live.zip)
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; OUT="${1:-/tmp}"; mkdir -p "$OUT"
(cd "$HERE" && sha256sum -c SHA256SUMS.parts)
for f in ReactOS-0.4.15-release-1-gdbb43bbaeb2-x86-live.zip ReactOS-0.4.15-release-1-gdbb43bbaeb2-x86-iso.zip; do
  cat "$HERE/$f".part* > "$OUT/$f"
done
(cd "$OUT" && sha256sum -c "$HERE/SHA256SUMS.full")
ln -sf "$OUT/ReactOS-0.4.15-release-1-gdbb43bbaeb2-x86-live.zip" "$OUT/ReactOS-0.4.15-x86-live.zip"
echo "ReactOS 0.4.15 restored to $OUT (live zip linked as ReactOS-0.4.15-x86-live.zip for deploy.sh)"
