#ifndef QRX_HOSTFS_H
#define QRX_HOSTFS_H
/* QRXHostFS ABI v1: a bounded shared host-directory device, not an AmigaDOS
 * filesystem handler. Guest only sees regular files in ONE mounted directory;
 * no symlinks, subdirectories, special nodes or host absolute paths. */
#include "qrxvirt.h"
#include <stdbool.h>
#include <stdint.h>
#define QRX_HOSTFS_BASE (QRX_IO_BASE+0x800u)
#define QRX_HOSTFS_MAGIC (QRX_HOSTFS_BASE+0x00u) /* 'QRXF' */
#define QRX_HOSTFS_FEATURES (QRX_HOSTFS_BASE+0x04u) /* bit0=read bit1=write */
#define QRX_HOSTFS_NAME_DMA (QRX_HOSTFS_BASE+0x08u)
#define QRX_HOSTFS_DATA_DMA (QRX_HOSTFS_BASE+0x0cu)
#define QRX_HOSTFS_OFFSET (QRX_HOSTFS_BASE+0x10u)
#define QRX_HOSTFS_LENGTH (QRX_HOSTFS_BASE+0x14u)
#define QRX_HOSTFS_COMMAND (QRX_HOSTFS_BASE+0x18u)
#define QRX_HOSTFS_STATUS (QRX_HOSTFS_BASE+0x1cu)
#define QRX_HOSTFS_RESULT (QRX_HOSTFS_BASE+0x20u)
#define QRX_HOSTFS_SIZE (QRX_HOSTFS_BASE+0x24u)
#define QRX_HOSTFS_CMD_LIST 1u
#define QRX_HOSTFS_CMD_STAT 2u
#define QRX_HOSTFS_CMD_READ 3u
#define QRX_HOSTFS_CMD_WRITE 4u
#define QRX_HOSTFS_CMD_DELETE 5u
#define QRX_HOSTFS_CMD_TRUNCATE 6u
#define QRX_HOSTFS_OK 0u
#define QRX_HOSTFS_INVALID 1u
#define QRX_HOSTFS_NOENT 2u
#define QRX_HOSTFS_IOERR 3u
#define QRX_HOSTFS_RDONLY 4u
#define QRX_HOSTFS_ACCESS 5u
#define QRX_HOSTFS_MAX_TRANSFER 4096u
#define QRX_HOSTFS_MAX_FILE (64u*1024u*1024u)
#define QRX_HOSTFS_MAX_NAME 96u
struct QRXHostFS {
    int dirfd;
    bool mounted, readonly;
    uint32_t name_dma,data_dma,offset,length,status,result,size;
};
typedef struct QRXHostFS QRXHostFS;
bool qrx_hostfs_mount(QRXHostFS *fs,const char *dir,bool readonly);
void qrx_hostfs_unmount(QRXHostFS *fs);
void qrx_hostfs_attach(QRXVM *vm, QRXHostFS *fs);
bool qrx_hostfs_read(QRXVM *vm,uint32_t addr,unsigned width,uint32_t *out);
bool qrx_hostfs_write(QRXVM *vm,uint32_t addr,unsigned width,uint32_t value);
#endif
