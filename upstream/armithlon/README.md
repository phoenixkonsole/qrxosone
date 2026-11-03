# ARMithlon 1.1 cumulative source

`1.1/` is the complete, unmodified 121-file contents of the user-supplied
`ARMITHLON-1.1-macOS-Cumulative-Source.zip` (206225 bytes).
Archive SHA-256:
`b3cda0c84b3c4f288835cafac4f7238c82e9723c8acc5f215416c395d104a811`.
All entries in its original `SHA256SUMS` were checked before integration.
Original reports are historical supplier evidence, not results of this build.

The import includes the partial virtual 68100 CPU, AArch64 JIT, Metal frontend,
media devices, 2D/3D command paths, optional Vulkan renderers, OpenGL subset,
QRX86 IPC/input adapters, benchmark sources, launchers and documentation.
The CPU does not yet boot AROS. A complete Glide, D3D9, OpenGL, Linux or Windows
implementation is not claimed. Preserve the included licenses and notices.

Use the repository integration, not the vendor's fetch/patch workflow:

```sh
bash tools/armithlon/build.sh
```

Run from the repository root. It builds the engine, runs CTest and builds
QRX86 host binaries using a disposable copy of the current checkout's runtime.
It does not clone a stale pinned revision or patch the working tree.
See [integration and Mac instructions](../../docs/ARMITHLON_INTEGRATION.md).
