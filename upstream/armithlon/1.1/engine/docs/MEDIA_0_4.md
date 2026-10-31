# QRXVIRT Media Device — development ABI v0.4 (unstable)

New virtual devices at `QRX_IO_BASE=0x00ff0000`. **This is not an Amiga/SAGA/3dfx register map.** RAM is big-endian from the 68k guest perspective. All MMIO media accesses require 32-bit aligned 32-bit words; wrong widths return bus errors.

## Common

| Address | Read value |
|---|---|
| 0x00FF0000 | magic `QRX1` |
| 0x00FF0004 | `0x00010000` (base board ABI) |
| 0x00FF0008 | feature bits: UART=1 TIMER=2 SHUTDOWN=4 VIDEO=8 AUDIO=16 INPUT=32, where bound |

MMIO extends through `0x00FF03FF`. Online vCPU count 1 (capacity 2). No interrupts or IRQ acknowledgments yet. The video queue is synchronous in v0.4; never treat its pointer as a host address. All host mappings validate RAM ranges.

## Graphics: QRXGFX

| Address | Width | Mode | Value |
|---|---|---|---|
| 0x00FF0100 | 32 | R | fixed width `800` |
| 0x00FF0104 | 32 | R | fixed height `450` |
| 0x00FF0108 | 32 | R | stride `3200` |
| 0x00FF010C | 32 | R | framebuffer `0x00200000` |
| 0x00FF0110 | 32 | R | format code `1` = XRGB8888 |
| 0x00FF0114 | 32 | W | write `1` to present one frame |
| 0x00FF0118 | 32 | R | presented frame counter |

Pixel = 32-bit `0x00RRGGBB` on the guest; raw bytes `00 RR GG BB`. Metal host copies/converts to native `RGBA8Unorm` texture; no shader pipeline is exposed to the guest. Framebuffer size `800*450*4=1,440,000 bytes` and needs RAM `>=0x0035F900` approximately. No mode changing or vsync interrupt yet. Nonzero return from `qrx_vm_write` means accepted presentation; emulator does not synchronize actual display scanout with guest. Framebuffer is written to guest RAM; no host pointer given to guest.

## Audio: QRXAudio

| Address | Width | Mode | Value |
|---|---|---|---|
| 0x00FF0200 | 32 | R | sample rate 48000 |
| 0x00FF0204 | 32 | R | channels 2 |
| 0x00FF0208 | 32 | W | PCM16 interleaved: high16 left, low16 right |
| 0x00FF020C | 32 | R | currently queued PCM frames |
| 0x00FF0210 | 32 | R | number of dropped frames |

One atomic producer thread and one AudioQueue callback consumer. 65536-frame ring; writes return false if full; callback zero-fills on underrun. No actual `ahi.device`, resampler, gain or hardware interrupts. The oscillator test source lives in `src/media_demo.c` and is **not a 68k sound driver**.

## Input: QRXInput

| Address | Width | Mode | Value |
|---|---|---|---|
| 0x00FF0300 | 32 | R | queued event count |
| 0x00FF0304 | 32 | R | pop event, zero if empty |
| 0x00FF0308 | 32 | R | mouse X 0..799 |
| 0x00FF030C | 32 | R | mouse Y 0..449 |
| 0x00FF0310 | 32 | R | mouse buttons bitmap (host platform bits) |
| 0x00FF0314 | 32 | R | dropped-event count |

Event format: `[type:8][key/button:8][value:16]`. Types: 1 key down, 2 key up, 3 mouse move (read coords separately), 4 mouse button (value 1=press, 0=release), 5 signed vertical scroll amount. Keyboard codes currently use macOS NSEvent virtual keyCode (not a PC scancode, not an Amiga raw key). Endianness follows 68k MMIO words. 256-event SPSC ring. Events may be dropped on overflow; no IRQ. GUI is input producer and guest CPU thread is consumer.

## Guest example

See `guest/qrxvirt_media.h` and `guest/example_media.c`. They describe how guest-side software *could* access registers after a proper board initialization. A true QRXOS/AROS distribution still needs boot ROM/firmware bindings, an AROS HIDD for RTG, AHI driver, input device and full 68k CPU/exception compatibility.

## Demo proof and limitations

`qrx68100_media_demo` copies synthetic pixels into the guest framebuffer, runs a genuine 68k `MOVE.L #GFX_PRESENT,A0` / `MOVE.L #1,(A0)` sequence through qrx_vm_step, and records its output as a PPM. Other test firmware feeds audio through MMIO and reads events. Cocoa renderer and AudioQueue are native but only built on macOS. No GPU shader programs, SAGA, Glide, Vulkan, hosted Linux window, AROS boot, SMP, or complete 68080 feature set in v0.4.
