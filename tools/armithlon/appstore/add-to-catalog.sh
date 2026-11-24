#!/bin/sh
# Prints the catalog line of an App Store archive, with its size and SHA-256 measured from the file:
#   add-to-catalog.sh ARCHIVE ID NAME SUBTITLE CATEGORY VERSION INSTALLED-BYTES URL LAUNCH DEVELOPER LICENSE DESCRIPTION
# Append the line to tools/armithlon/pi/image/rootfs/qrx/appstore/catalog.tsv. The URL must be where
# exactly this file is published (a GitHub release asset); the store installs nothing else.
set -eu
[ $# -eq 12 ] || { sed -n 2,5p "$0"; exit 2; }
a=$1; shift
case "$7" in https://*) ;; *) echo "the URL must be https" >&2; exit 1 ;; esac
xz -t "$a" && xz -dc "$a" | tar -t >/dev/null || { echo "not a .tar.xz: $a" >&2; exit 1; }
size=$(wc -c < "$a" | tr -d ' ')
sha=$(sha256sum "$a" | cut -d' ' -f1)
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$size" "$6" "$sha" "$7" "$8" "$9" "${10}" "${11}"
