# QRX Direct3D 9 native reference (MIT source)

This is an original, self-contained Windows PE64 Direct3D9 sample for a GPU driver
reference. **Do not report its FPS as QRX86 FPS**; the current QRX86 PE32 HLE
has no `d3d9.dll` implementation. It has a separate limited DirectDraw HLE.

For a PE32 binary targeting a future QRX86 D3D9 translator:

```sh
i686-w64-mingw32-gcc -O2 -o qrx-d3d9-i386.exe qrx_d3d9_benchmark.c -ld3d9 -lgdi32 -luser32 -lm
```

Current test: on a native Windows host with the Direct3D9 runtime, run
`qrx-d3d9-i386.exe 90 10 direct3d9-bench.ppm`. It prints JSON to stdout.
Compiled only if mingw-w64 is installed; never silently run through QRX86.
