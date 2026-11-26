#ifndef QRX_EC64_HOSTFS_H
#define QRX_EC64_HOSTFS_H
/* QRX OS One 64's file system on a host directory (qrx_ec64_hostfs.c): host service 12 of the
 * 68100EC64 board, for the guest's qrxhostfs-handler (tools/armithlon/aros-qrx/qrxhostfs). The guest
 * sees one host directory as an AmigaDOS volume: a whole tree, 64-bit sizes, names up to 255 bytes,
 * dates, AmigaDOS protection bits. Everything is opened relative to that directory, one component at
 * a time, never following a symbolic link: the guest cannot leave it. Names are matched without
 * regard to case, as AmigaDOS does.
 *
 * The request is the board's host call (qrx_ec64_board.h): 32-bit big endian words, [0] the service,
 * [3] the operation, a[i] at word 4 + i:
 *   a[0], a[1]  the path (high, low word of its guest address): bytes, 0 ended, at most 1023,
 *               components separated by '/', relative to the share ("" is its root)
 *   a[2], a[3]  a buffer (high, low), a[4] its length
 *   a[5], a[6]  a 64-bit offset (high, low), a[7] an argument
 * and the results:
 *   a[8]        0, or an AmigaDOS error code (ERROR_OBJECT_NOT_FOUND, ...)
 *   a[9]        bytes moved
 *   a[10], a[11] the object's size afterwards (high, low)
 *
 * The record STAT and READDIR write (QRX_HFS64_RECORD bytes, big endian):
 *   +0  .l  AmigaDOS entry type: ST_ROOT 1 (the share itself), ST_USERDIR 2, ST_FILE -3
 *   +4  .l  AmigaDOS protection bits (FIBF_*, set = not allowed for R/W/E/D)
 *   +8  .q  size in bytes
 *   +16 .q  last change, seconds since 1970 (UTC)
 *   +24 .q  host inode number
 *   +32 ... reserved, 0
 *   READDIR adds the entry's name, 0 ended, at +QRX_HFS64_RECORD. */
#include <stdbool.h>
#include <stdint.h>

#define QRX_HFS64_SERVICE 12u
#define QRX_HFS64_RECORD 64u
#define QRX_HFS64_PATH_MAX 1023u
#define QRX_HFS64_IO_MAX (16u * 1024u * 1024u)   /* bytes per READ or WRITE */

enum {
    QRX_HFS64_STAT = 1,      /* path; record into the buffer */
    QRX_HFS64_READDIR = 2,   /* path (a directory), a[7] the entry's index from 0; record + name into
                              * the buffer; ERROR_NO_MORE_ENTRIES after the last */
    QRX_HFS64_READ = 3,      /* path, offset, buffer, length: a[9] bytes read (0 at the end) */
    QRX_HFS64_WRITE = 4,     /* path, offset, buffer, length: a[9] bytes written */
    QRX_HFS64_OPEN = 5,      /* path; a[7] 0 must exist (a file), 1 create if missing, 2 create or
                              * empty it (MODE_NEWFILE) */
    QRX_HFS64_DELETE = 6,    /* path: a file, or an empty directory */
    QRX_HFS64_MKDIR = 7,     /* path */
    QRX_HFS64_RENAME = 8,    /* path to the buffer's path (0 ended); fails if that exists */
    QRX_HFS64_SETSIZE = 9,   /* path, offset = the new size */
    QRX_HFS64_PROTECT = 10,  /* path, a[7] AmigaDOS protection bits */
    QRX_HFS64_SETDATE = 11,  /* path, offset = seconds since 1970 */
    QRX_HFS64_INFO = 12,     /* a[10], a[11] total, a[9] used, in 4 KiB blocks capped at 2^31-1 (a[12]
                              * 1 read-only) */
    QRX_HFS64_SYNC = 13,     /* every write on the host's disk before the reply */
};

/* The share: dir, read-only or not. false when dir is no directory. */
bool qrx_ec64_hostfs_mount(const char *dir, bool readonly);
bool qrx_ec64_hostfs_mounted(void);
/* One host call; ram, ram_size: the guest's memory. q: the request (17 words). */
void qrx_ec64_hostfs_call(uint8_t *ram, uint64_t ram_size, uint8_t *q);
#endif
