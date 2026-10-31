#!/usr/bin/env bash
# Build a separate pinned FS-UAE, without replacing the installed emulator.
set -Eeuo pipefail
PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:?usage: build.sh NEW-WORKDIR}"
mkdir "$WORK"
WORK="$(cd "$WORK" && pwd)"
git clone --depth 1 --branch v3.2.35 https://github.com/FrodeSolheim/fs-uae.git "$WORK/source"
cd "$WORK/source"
[[ $(git rev-parse HEAD) == 4ae7ddaec50b567ed80d71ffbff067cb58e945a3 ]]
git apply --check "$PATCH_DIR/aros-diagnostic-rom.patch"
git apply "$PATCH_DIR/aros-diagnostic-rom.patch"
./bootstrap
./configure --prefix="$WORK/install" --enable-jit --without-libmpeg2
make -j"${QRX_BUILD_JOBS:-4}"
make install
sha256sum "$WORK/install/bin/fs-uae" > "$WORK/emulator.sha256"
printf 'Emulator: %s\n' "$WORK/install/bin/fs-uae"
