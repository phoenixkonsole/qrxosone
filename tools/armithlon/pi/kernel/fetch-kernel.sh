#!/usr/bin/env bash
# The Raspberry Pi Linux source the image's kernel is built from: branch rpi-6.6.y at the commit
# below (6.6.78), and nothing else. Too big for the repository; this fetches exactly that commit.
#   fetch-kernel.sh DIR      then: build-kernel.sh DIR FIRMWARE-ROOT OUT
set -Eeuo pipefail
COMMIT=bba53a117a4a5c29da892962332ff1605990e17a
DIR="${1:?usage: fetch-kernel.sh DIR}"
mkdir -p "$DIR"
cd "$DIR"
[ -d .git ] || git init -q
git fetch --depth 1 https://github.com/raspberrypi/linux "$COMMIT"
git checkout -q FETCH_HEAD
[ "$(git rev-parse HEAD)" = "$COMMIT" ] || { echo "not the expected commit" >&2; exit 1; }
echo "Raspberry Pi Linux $COMMIT in $DIR"
