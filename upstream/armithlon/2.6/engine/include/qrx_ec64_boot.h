#ifndef QRX_EC64_BOOT_H
#define QRX_EC64_BOOT_H
/* Loads the 64-bit AROS kickstart (core.elf: kernel.resource, exec.library, task.resource) and
 * further kickstart modules into the 68100EC64's RAM, as a boot loader would: the relocatable
 * ELFs below 4 GB from 1 MB on (the core starts in 32-bit mode), the boot tags after them, and a
 * stub at 0x8000 that starts the kernel with A0 at the tags. A disk image goes to the top of RAM,
 * outside the memory exec gets, and the command line names it for qrxdisk.device. */
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *kernel;             /* core.elf */
    const char *const *modules;     /* further kickstart modules, in this order */
    unsigned nmodules;
    const char *cmdline;            /* the kernel command line */
    const uint8_t *disk;            /* disk image or NULL, a multiple of 512 bytes */
    uint64_t disk_size;
    FILE *map;                      /* if set: "address name" of every function */
    FILE *log;                      /* if set: where each module went */
} qec64_aros_boot;

typedef struct {
    uint64_t pc, sp;                /* where core 0 starts, in 32-bit mode */
    uint64_t kernel_end;
    uint64_t disk_at;               /* the disk image's address, 0 without one */
} qec64_aros_loaded;

/* 0 on success; otherwise -1 and a message in err */
int qec64_aros_load(uint8_t *ram, uint64_t ram_size, const qec64_aros_boot *b, qec64_aros_loaded *out,
                    char *err, size_t errlen);

/* The values the 64-bit AROS ends a run with for exec's shutdown (SC_REBOOT in its kernel) */
#define QEC64_END_POWEROFF  0x100
#define QEC64_END_COLDBOOT  0x101
#define QEC64_END_WARMBOOT  0x102
#endif
