# ARMITHLON 1.0 – QRXGPU API and GPU benchmark suite

This directory contains **original open-source benchmark source** written for
ARMITHLON (MIT license for new benchmark code only, not a relicensing of any
pre-existing QRX/AROS/ReactOS components). The exact same 800×450 workload is
submitted through the existing device and API paths:

* `glide-mmio` – 68k-safe QRX3D board MMIO commands, not original 3dfx Glide.
* `opengl-hle` – QRX86 OpenGL 1.x *host HLE* encoder; **does not run an EXE**.
* `cpu` – CPU barycentric pixel renderer as deterministic image reference.
* `metal` – native M1 Metal GPU vertex pipeline, depth, alpha, GPU waits, screenshot.
* `vulkan` – Vulkan 3D offscreen GPU pipeline, depth, alpha, GPU waits, screenshot.
* `direct3d9-native` – **separate Windows source** for API baseline; QRX86 still lacks `d3d9.dll`.
* `glide2x.dll` and `warp3d` – SKIP until actual ABI adapters exist.

The original color-grid scene draws 240 opaque triangles (20×12 grid), then 16
transparent overlaid triangles, with an RGB clear. **No 3D assets, proprietary
SDK, or random third-party code are copied or shipped.** All 256 triangles are
reproducible, with no random numbers or input dependence. All APIs use the
same resolution and triangle count. Differences in API semantics (e.g. HLE
not implementing a depth-write toggle) are documented and not passed off as
identical guest executions.

## One command on M1 or Linux

```sh
./armithlon build
./armithlon test
./armithlon benchmark            # 60 timed frames, 8 warmup frames
./armithlon benchmark-quick      # 10 timed frames
./armithlon benchmark 180        # custom timed frames
```

Look in `reports/latest` for `results.json`, `REPORT.md`, and `qrx-reference.ppm`.
When a GPU backend is built and functional, it also writes `metal-gpu.ppm`
or `vulkan-gpu.ppm`; the collector calculates mean absolute RGB error compared
with the deterministic CPU screenshot. It rejects large discrepancies as
`FAIL_IMAGE` instead of silently printing a success FPS. A GPU shader or
backend is **not verified** merely by compiling successfully.

### Meaning of the results

`command_translation_only`: upper bound on encoder/bridge throughput, **no GPU**.
`cpu_reference`: native CPU rasterization, **not guest 68k performance**.
`gpu_submission_and_wait`: encode+submit+GPU completion, no display/present.
`native_d3d9_present_walltime`: standalone Windows baseline with Present.
**These FPS cannot be ranked as if they measured the same thing.** The GPU
benchmarks run headless, do not represent desktop compositor FPS, and do not
measure the AROS boot time, x86 interpreter throughput, QRX86 JIT speed, or
original 3dfx Voodoo speed.

### Optional GPU dependencies

* macOS Metal: Xcode Command Line Tools, Apple Silicon, an available Metal GPU.
* Vulkan: Vulkan SDK including Vulkan headers, Vulkan loader and a device ICD;
  `glslangValidator` for the shader build. On macOS configure `VULKAN_SDK` and
  loader paths in the terminal used for CMake. Metal is the primary Mac path.
* Direct3D9: on Windows, compile the standalone MIT `direct3d9/` sample with
  MinGW-W64 for a native driver baseline. Do not pass it through `qrx-win32`
  and interpret an unimplemented import as GPU driver failure.

### Real, established open-source host baselines (optional, not bundled)

* [glmark2](https://github.com/glmark2/glmark2) – OpenGL ES2 / OpenGL 2.0;
  GPLv3; uses many APIs not implemented in the QRX86 immediate-mode subset.
* [vkmark](https://github.com/vkmark/vkmark) – Vulkan benchmark, LGPL2.1,
  Linux windowing; can compare **host Vulkan**, not ARMITHLON throughput.
* [Khronos Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples)
  – documented Vulkan API examples / performance samples. Not bundled due to
  project dependencies and component-level asset licenses.
* Windows native Direct3D 9 sample source in `direct3d9/` tests only the D3D9
  host driver. D3D3/7/8/9, DirectX 10/11/12 HLE require separate engineering.

Because third-party benchmarks require APIs/graphics contexts not yet exposed
by our emulation layer, running an unmodified glmark2 or vkmark **inside**
QRX86 is currently impossible; those tools are reference workloads on the host.


## ARMITHLON 1.1 external benchmark integration

All previously mentioned tools (glmark2, vkmark and proprietary Geekbench 7) are
integrated as **optional native host benchmarks**, without shipping their binaries.
See `THIRD-PARTY.md` for installation, licensing, CLI and comparability limitations.
