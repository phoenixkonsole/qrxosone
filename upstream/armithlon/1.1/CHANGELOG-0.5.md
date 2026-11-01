# ARMITHLON v0.5 (2026-10-31)

- QRX GPU2D virtual MMIO mailbox: 4 bounded operations (clear, fill, guest
  2D source blit, present), error/status regs, caps and command count.
- New guest-side m68k qrxvirt_gpu.h API; AROS developer CLI probe source
  (not a registered graphics.hidd or audio driver).
- PCM16BE bulk audio DMA reads bounded VM guest memory, endian-preserving
  transfer into lock-free SPSC output buffer; tested guest buffer errors.
- Native macOS preview now executes guest 68k machine-code for its 2D drawing,
  and initiates guest-audio DMA through 68k opcode MMIO writes. Host CPU raster
  + Metal frame presentation, CoreAudio and input remain from v0.4.
- QRX86 integration plan for PE/ELF import interception with host API bridge;
  no DirectX/OpenGL execution feature claimed.
- All six Linux x86_64 CTest checks successful. macOS remains not executed here.
- No changes to user's private QRX OS One GitHub branch. Existing 0.3
  authenticated fetch, AROS FS-UAE fallback and ISO preview preserved.
