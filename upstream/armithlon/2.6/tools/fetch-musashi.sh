#!/usr/bin/env bash
# Pinned third-party CPU. No branch drift; no force reset or destructive updates.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/engine/third_party/Musashi"
PIN='313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd'
command -v git >/dev/null || { echo 'git missing' >&2; exit 2; }
if [[ -d "$DEST/.git" ]]; then
  got="$(git -C "$DEST" rev-parse HEAD)"
  [[ "$got" == "$PIN" ]] || { echo "Refuse unexpected existing Musashi checkout $got; expected $PIN" >&2; exit 2; }
  [[ -z "$(git -C "$DEST" status --porcelain)" ]] || { echo 'Musashi tree modified; refusing build' >&2; exit 2; }
else
  [[ ! -e "$DEST" || -z "$(ls -A "$DEST" 2>/dev/null)" ]] || { echo 'Nonempty non-Git Musashi directory; refusing overwrite' >&2; exit 2; }
  git clone https://github.com/kstenerud/Musashi.git "$DEST"
  git -C "$DEST" checkout --detach "$PIN"
fi
[[ "$(git -C "$DEST" rev-parse HEAD)" == "$PIN" ]] || exit 2
for f in m68k.h m68kcpu.c m68k_in.c m68kmake.c m68kconf.h softfloat/softfloat.c; do
  [[ -s "$DEST/$f" ]] || { echo "Missing Musashi source: $f" >&2; exit 2; }
done
printf 'Pinned Musashi 68000/010/020/030/040: %s\n' "$PIN"
