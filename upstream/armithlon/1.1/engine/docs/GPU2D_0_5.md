# QRXGPU v0.5 virtual board — implemented 2D packet ABI

CPU: QRX68100 guest 68k (32-bit address space). Bus is BE longword. For this
release, these registers are only valid when `QRX_IO_BASE` returns `QRX1`.
The current board has 800x450, shared 32-bit XRGB framebuffer at 0x00200000.
MMIO is 0x00FF0000..0x00FF07FF (0x800 bytes).

## Register map

### Existing device bank

- `0x00FF0000`: BOARD_MAGIC `0x51525831` (`QRX1`)
- `0x00FF0008`: features, bit 6 = QRXGPU2D
- `0x00FF0100`: framebuffer width; `+0x104` height; `+0x108` pitch;
  `+0x10c` guest framebuffer offset; `+0x114` legacy PRESENT trigger;
  `+0x118` presented frame count
- `0x00FF0208`: legacy single stereo PCM16 frame BE (L:16,R:16)
- `0x00FF0214`: DMA_SOURCE guest address (32-bit aligned)
- `0x00FF0218`: DMA_FRAMES (1..4096)
- `0x00FF021c`: write `1` to submit; `+0x220` returns 0 OK / 1 error
- `0x00FF0300`: input FIFO count; `+0x304` pops event, other input regs
  unchanged from v0.4

### New QRXGPU bank 0x00FF0400 (reads always 32-bit)

| Offset | Read | Write |
| --- | --- | --- |
| +0x00 | magic `0x51524735` (`QRG5`) | ignored/not permitted |
| +0x04 | ABI `0x00050000` | not permitted |
| +0x08 | caps = FILL(1), BLIT(2), PRESENT(4) | not permitted |
| +0x0c | last error: 0 OK, 1 unknown opcode, 2 bad argument, 3 bad bounds, 4 no framebuffer | not permitted |
| +0x10 | last command | write command opcode; synchronous execute |
| +0x14 | successful command count | not permitted |
| +0x20 | `ARG0` | read/write |
| +0x24 | `ARG1` | read/write |
| +0x28 | `ARG2` | read/write |
| +0x2c | `ARG3` | read/write |
| +0x30 | `ARG4` (reserved) | read/write |
| +0x34 | `ARG5` (reserved) | read/write |

**Op 1 CLEAR**: ARG0=0x00RRGGBB, fills every pixel with `00 R G B` bytes.

**Op 2 FILL**: ARG0=`x<<16 | y`, ARG1=`w<<16 | h`, ARG2=0x00RRGGBB.
Rect is clipped at framebuffer edges. Zero width/height and x/y beyond screen
are rejected; no writes outside guest framebuffer.

**Op 3 BLIT**: ARG0=source guest byte address; ARG1=source pitch bytes;
ARG2=`x<<16 | y`; ARG3=`w<<16 | h`. Guest buffer contains XRGB8888 as
big-endian bytes `00 R G B`, 4 bytes per pixel. 4-byte aligned source, pitch
>= w*4; full source must be within guest RAM, outside framebuffer and MMIO;
source validated *before* clipping. Invalid/overlapping inputs are rejected.

**Op 4 PRESENT**: submits current framebuffer to the host callback (Metal on
macOS after CPU pixel conversion); no arguments. Zero-copy GPU2D host kernel,
OpenGL/Direct3D, Voodoo/Glide, SAGA and Vulkan backends are NOT implemented.

### Other details

- The virtual GPU mailbox supports **only one command producer**; a future SMP
  guest must implement locking or dedicated queues. Commands are synchronous
  and deterministically validated. Failed operations set the status register,
  not a synthetic 68k bus-error exception.
- PCM16BE guest audio DMA uses 1..4096 stereo frames. Buffer must reside in
  VM guest RAM at 4-byte aligned address. The system copies the samples into a
  bounded SPSC ring; a Linux x86_64 test verifies sign/endian/bounds.
- Real AROS HIDD/AHI/input drivers need a bootable qrxvirt-m68k machine target,
  not an Amiga machine/FS-UAE ROM. The provided source API adapter is only
  a prerequisite.

## Test procedure

```
./armithlon build
./armithlon test
./armithlon gpu-test
./armithlon media-test
./armithlon run-native # macOS only; terminal-driven guest 68k demo
```
