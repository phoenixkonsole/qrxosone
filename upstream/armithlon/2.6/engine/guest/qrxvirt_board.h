#ifndef QRXVIRT_GUEST_BOARD_H
#define QRXVIRT_GUEST_BOARD_H
/* For future 68k guest BSP code ONLY: do not include in native macOS programs. */
#if defined(QRX_NATIVE_040)
#define QRXVIRT_MMIO_BASE      0xFF000000UL
#else
#define QRXVIRT_MMIO_BASE      0x00FF0000UL
#endif
#define QRXVIRT_REG_MAGIC      0x00UL
#define QRXVIRT_REG_VERSION    0x04UL
#define QRXVIRT_REG_FEATURES   0x08UL
#define QRXVIRT_REG_CONSOLE    0x0CUL
#define QRXVIRT_REG_TICKS      0x10UL
#define QRXVIRT_REG_ONLINE_CPU 0x14UL
#define QRXVIRT_REG_TOTAL_CPU  0x18UL
#define QRXVIRT_REG_SHUTDOWN   0x1CUL
/* 2.1: 32-bit guest reads total RAM as two big-endian register words. */
#define QRXVIRT_REG_RAM_LO      0x34UL
#define QRXVIRT_REG_RAM_HI      0x38UL
#define QRXVIRT_REG_LOW_ALIAS   0x3CUL
#define QRXVIRT_MAGIC          0x51525831UL
#define QRXVIRT_VERSION        0x00010000UL
#define QRXVIRT_FEAT_UART      1UL
#define QRXVIRT_FEAT_TIMER     2UL
#define QRXVIRT_FEAT_SHUTDOWN  4UL
#define QRXVIRT_FEAT_BLOCK     8UL
#define QRXVIRT_FEAT_HOSTFS    16UL
#define QRXVIRT_FEAT_RAMSIZE   32UL
#endif
