# QRX86 first real OpenGL PE32 guest benchmark (MIT)

This is a **real Windows PE32 source program** that imports OpenGL 1.1 functions
through `opengl32.dll`, plus `gdi32.SwapBuffers`. It is deliberately restricted
to the ARMITHLON 0.9 experimental HLE. It uses 240 opaque + 16 transparent
triangles, matching the general benchmark scene dimensions.

The code uses the emulator's *synthetic HDC=1* and must **NOT be run on real
Windows**: the invalid native HDC would be meaningless. No window manager,
real WGL context, textures or matrix stack are implemented.

To compile (not bundled as a binary):

```sh
brew install mingw-w64
./armithlon build-pe32-gl-benchmark
```

On Mac terminal A:

```sh
./armithlon run-native-qrx86
```

Mac terminal B:

```sh
./armithlon fetch
./armithlon build-qrx86-host
./armithlon run-pe32-gl-benchmark 20
```

**Status:** source and build script added; NO M1, PE32 guest execution or GPU
synchronization verified in the current Linux x86-64 environment. Our terminal
command checks the display socket exists but records only the host-wall-clock
time for QRX86 command submissions (not GPU completion or true displayed FPS).
A nonzero guest exit is a failure, and no other program or app is affected.
