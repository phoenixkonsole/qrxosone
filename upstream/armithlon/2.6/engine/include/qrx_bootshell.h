#ifndef QRX_BOOTSHELL_H
#define QRX_BOOTSHELL_H
/* ARMITHLON 1.8: genuinely executed 68k boot firmware + bounded system device.
 * A standalone QRX firmware monitor, NOT AmigaOS/AROS or a full 68040 core.
 */
#include "qrxvirt.h"
#include "qrx_hostfs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define QRX_BOOT_RAM (256u * 1024u)
#define QRX_BOOT_BUFFER 0x8000u
#define QRX_BOOT_ENTRY 0x1000u
#define QRX_BOOT_RX (QRX_IO_BASE+0x40u)
#define QRX_BOOT_RX_STATUS (QRX_IO_BASE+0x44u)
#define QRX_BOOT_EXEC (QRX_IO_BASE+0x50u)
#define QRX_DISK_SLOTS 16u
#define QRX_DISK_SLOT_BYTES 4096u
#define QRX_DISK_BYTES (512u+QRX_DISK_SLOTS*QRX_DISK_SLOT_BYTES)

struct QRXShell;
typedef struct QRXShell {
    QRXVM *vm;
    unsigned char disk[QRX_DISK_BYTES];
    char disk_path[1024];
    unsigned char input[256];
    unsigned input_head, input_tail;
    uint64_t commands;
    bool reboot, shutdown, quiet;
    bool program_pending;
    unsigned program_bytes;
    uint64_t started_millis;
    void *cpu_opaque;
    int (*run_cycles)(void *cpu_opaque,uint64_t budget); /* optional Musashi backend */
    bool (*run_subroutine)(void *cpu_opaque,uint32_t entry,uint64_t budget,uint64_t *retired);
} QRXShell;

/* Build real 68000-compatible machine code with reset vectors in guest RAM. */
bool qrx_build_boot_shell(uint8_t *ram, size_t bytes);
/* Mount a private, checked QRXFS-1 disk image; create only if absent. */
bool qrx_shell_mount(QRXShell *s, QRXVM *vm, const char *path);
void qrx_shell_detach(QRXShell *s);
bool qrx_shell_enqueue(QRXShell *s, const char *line);
/* Every received command runs via real guest instructions writing QRX_BOOT_EXEC. */
bool qrx_shell_run_line(QRXShell *s, const char *line, uint64_t max_guest_steps);
/* MMIO device callbacks, called by the normal guest bus (never host CLI directly). */
bool qrx_shell_dev_read(QRXVM *v,uint32_t addr,unsigned size,uint32_t *out);
bool qrx_shell_dev_write(QRXVM *v,uint32_t addr,unsigned size,uint32_t val);
#endif
