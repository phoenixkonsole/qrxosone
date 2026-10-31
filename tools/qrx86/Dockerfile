# Linux recipe for Apple Silicon Docker Desktop via --platform linux/amd64.
# Native AROS execution still requires the explicit FS-UAE test step.
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev python3 binutils-m68k-linux-gnu gcc-m68k-linux-gnu \
    libc6-dev-m68k-cross qemu-user gcc-mingw-w64-i686 fs-uae xvfb \
    xauth imagemagick xz-utils ca-certificates 7zip fonts-dejavu-core libgl1-mesa-dri && rm -rf /var/lib/apt/lists/*
WORKDIR /src
CMD ["bash"]
