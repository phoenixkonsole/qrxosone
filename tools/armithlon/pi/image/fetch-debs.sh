#!/usr/bin/env bash
# Download the Ubuntu 24.04 arm64 packages the image takes files from (raw material only:
# mkrootfs.py copies selected programs, their ELF dependencies and data files).
#   fetch-debs.sh DEBDIR      (needs an apt with arm64 multiarch from ports.ubuntu.com)
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$1"; mkdir -p "$OUT"
pk=$(grep -v '^#' "$HERE/packages.txt" | sed 's/$/:arm64/')
apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces \
  --no-enhances $pk 2>/dev/null | grep -v '^ ' | grep -v '^<' | sed 's/:arm64$//' | sort -u |
  grep -vxF -f <(grep -v '^#' "$HERE/exclude.txt") > "$OUT/packages.list"
cd "$OUT"
while read -r p; do
  ls "${p}"_*.deb >/dev/null 2>&1 && continue
  apt-get download -q "$p:arm64" >/dev/null 2>&1 || apt-get download -q "$p" >/dev/null 2>&1 || echo "missing: $p"
done < packages.list
for p in linux-firmware-raspi; do ls ${p}_*.deb >/dev/null 2>&1 || apt-get download -q "$p:arm64" >/dev/null; done
ls *.deb | wc -l
