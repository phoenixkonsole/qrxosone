#!/bin/sh
# Joins the parts, checks SHA256SUMS, unpacks the toolchains into DIR and puts the sources into DIR/sources.
# usage: unpack.sh DIR
set -e
HERE=$(cd "$(dirname "$0")" && pwd); DIR=${1:?usage: unpack.sh DIR}
mkdir -p "$DIR/sources"; T=$(mktemp -d "$DIR/.join.XXXXXX"); trap 'rm -rf "$T"' EXIT
for d in toolchains sources; do
  for first in "$HERE/$d"/*.part00; do
    name=$(basename "$first" .part00); cat "$HERE/$d/$name".part* > "$T/$name"
  done
done
(cd "$T" && sha256sum -c "$HERE/SHA256SUMS")
for a in "$T"/*.tar.xz; do case $(basename "$a") in *-src.tar.xz|llvm-project-*) continue ;; esac; xz -dc "$a" | tar -x -C "$DIR"; done
mv "$T"/llvm-project-*.tar.xz "$T"/rustc-*-src.tar.xz "$T"/aros-portssources.tar "$DIR/sources/"
echo "unpacked into $DIR"
