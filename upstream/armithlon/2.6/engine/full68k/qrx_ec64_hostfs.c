/* QRX OS One 64's file system on a host directory: host service 12 (qrx_ec64_hostfs.h).
 *
 * Each request names its object by a path relative to the share. The path is walked one component
 * at a time with openat/fstatat from the share's directory descriptor, O_NOFOLLOW everywhere, so a
 * symbolic link, ".." or an absolute path never leads outside it; a component is matched exactly
 * first and else without regard to case. Open files are kept in a small cache by path (AmigaDOS
 * reads and writes in packets of a few KiB), directory listings by path for READDIR's index; both
 * are dropped by anything that changes the tree and expire after HFS64_TTL_MS, since programs on the
 * host may change the share too. Writes go to the host's page cache at once: a crash of the guest
 * or the emulator loses nothing; SYNC (and closing the emulator) puts them on the disk. */
#define _GNU_SOURCE
#include "qrx_ec64_hostfs.h"
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* AmigaDOS's values (dos/dos.h) */
#define E_NO_FREE_STORE 103
#define E_LINE_TOO_LONG 120
#define E_OBJECT_IN_USE 202
#define E_OBJECT_EXISTS 203
#define E_DIR_NOT_FOUND 204
#define E_OBJECT_NOT_FOUND 205
#define E_OBJECT_TOO_LARGE 207
#define E_ACTION_NOT_KNOWN 209
#define E_INVALID_COMPONENT_NAME 210
#define E_OBJECT_WRONG_TYPE 212
#define E_DISK_WRITE_PROTECTED 214
#define E_DIRECTORY_NOT_EMPTY 216
#define E_SEEK_ERROR 219
#define E_DISK_FULL 221
#define E_DELETE_PROTECTED 222
#define E_WRITE_PROTECTED 223
#define E_READ_PROTECTED 224
#define E_NO_DISK 226
#define E_NO_MORE_ENTRIES 232
#define E_NOT_IMPLEMENTED 236
#define ST_ROOT 1
#define ST_USERDIR 2
#define ST_FILE -3
#define FIBF_DELETE 1u
#define FIBF_EXECUTE 2u
#define FIBF_WRITE 4u
#define FIBF_READ 8u

#define HFS64_TTL_MS 1500u
#define HFS64_FILES 32u

typedef struct { char path[QRX_HFS64_PATH_MAX + 1]; int fd; bool writable; uint64_t opened, used; } FileSlot;
typedef struct { char name[256]; struct stat st; } DirEntry;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int root = -1;
static bool read_only;
static FileSlot files[HFS64_FILES];
static uint64_t clock_n;
static char dir_path[QRX_HFS64_PATH_MAX + 1];
static DirEntry *dir;
static unsigned dir_n, dir_cap;
static uint64_t dir_ms;
static bool dir_valid;

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

static int amiga_error(int e)
{
    switch (e) {
    case ENOENT: return E_OBJECT_NOT_FOUND;
    case EEXIST: return E_OBJECT_EXISTS;
    case ENOTDIR: case EISDIR: return E_OBJECT_WRONG_TYPE;
    case ENOTEMPTY: return E_DIRECTORY_NOT_EMPTY;
    case EROFS: return E_DISK_WRITE_PROTECTED;
    case EACCES: case EPERM: return E_WRITE_PROTECTED;
    case ENOSPC: case EDQUOT: return E_DISK_FULL;
    case ENAMETOOLONG: return E_INVALID_COMPONENT_NAME;
    case EBUSY: case ETXTBSY: return E_OBJECT_IN_USE;
    case EFBIG: return E_OBJECT_TOO_LARGE;
    case ENOMEM: return E_NO_FREE_STORE;
    case ELOOP: return E_OBJECT_WRONG_TYPE;     /* a symbolic link: not followed */
    default: return E_SEEK_ERROR;
    }
}

bool qrx_ec64_hostfs_mount(const char *path, bool ro)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    pthread_mutex_lock(&lock);
    if (root >= 0) close(root);
    root = fd;
    read_only = ro;
    for (unsigned i = 0; i < HFS64_FILES; i++) files[i].fd = -1;
    dir_valid = false;
    pthread_mutex_unlock(&lock);
    return true;
}
bool qrx_ec64_hostfs_mounted(void) { return root >= 0; }

static void files_drop(void)
{
    for (unsigned i = 0; i < HFS64_FILES; i++)
        if (files[i].fd >= 0) { close(files[i].fd); files[i].fd = -1; files[i].path[0] = 0; }
}
static void changed(void) { files_drop(); dir_valid = false; }

/* the real name of `want` in directory fd: itself, or the one entry equal without regard to case */
static int match(int fd, const char *want, char out[256])
{
    struct stat st;
    if (fstatat(fd, want, &st, AT_SYMLINK_NOFOLLOW) == 0) { strcpy(out, want); return 0; }
    if (errno != ENOENT) return -1;
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) return -1;
    DIR *d = fdopendir(copy);
    if (!d) { close(copy); return -1; }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL)
        if (!strcasecmp(e->d_name, want)) { strcpy(out, e->d_name); found = 1; break; }
    closedir(d);
    if (!found) { strcpy(out, want); errno = ENOENT; return 1; }   /* not there: out is the name to create */
    return 0;
}
/* the directory that holds path's last component, opened, and that component's real name; for ""
 * the share and ".". -1 with errno when a directory on the way is missing or not one. */
static int walk(const char *path, char leaf[256])
{
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!*path) { strcpy(leaf, "."); return fd; }
    const char *p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (!n || n > 255 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) { close(fd); errno = EINVAL; return -1; }
        char part[256];
        memcpy(part, p, n);
        part[n] = 0;
        if (match(fd, part, leaf) < 0) { int e = errno; close(fd); errno = e; return -1; }
        if (!slash || !slash[1]) return fd;
        int next = openat(fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) { int e = errno; close(fd); errno = e == ENOENT ? ENOENT : e; return -1; }
        close(fd);
        fd = next;
        p = slash + 1;
    }
}

static uint32_t protection(const struct stat *st)
{
    uint32_t p = 0;     /* AmigaDOS: a set bit forbids */
    if (!(st->st_mode & S_IRUSR)) p |= FIBF_READ;
    if (!(st->st_mode & S_IWUSR)) p |= FIBF_WRITE | FIBF_DELETE;
    /* every file may run (AmigaDOS programs have no x bit on the host) */
    return p;
}
static void record(uint8_t *r, const struct stat *st, bool is_root)
{
    memset(r, 0, QRX_HFS64_RECORD);
    int32_t type = is_root ? ST_ROOT : S_ISDIR(st->st_mode) ? ST_USERDIR : ST_FILE;
    put32(r, (uint32_t)type);
    put32(r + 4, protection(st));
    put64(r + 8, S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size);
    put64(r + 16, (uint64_t)(int64_t)st->st_mtime);
    put64(r + 24, (uint64_t)st->st_ino);
}

/* a cached descriptor for path (a regular file), opened for writing when write */
static int file_fd(const char *path, bool write, int *err)
{
    uint64_t t = now_ms();
    for (unsigned i = 0; i < HFS64_FILES; i++) {
        FileSlot *f = &files[i];
        if (f->fd < 0 || strcmp(f->path, path)) continue;
        if (t - f->opened > HFS64_TTL_MS || (write && !f->writable)) { close(f->fd); f->fd = -1; break; }
        f->used = ++clock_n;
        return f->fd;
    }
    char leaf[256];
    int parent = walk(path, leaf);
    if (parent < 0) { *err = amiga_error(errno); return -1; }
    int fd = openat(parent, leaf, (write ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
    int e = errno;
    close(parent);
    if (fd < 0) { *err = amiga_error(e); return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); *err = E_OBJECT_WRONG_TYPE; return -1; }
    FileSlot *best = &files[0];
    for (unsigned i = 0; i < HFS64_FILES; i++) {
        if (files[i].fd < 0) { best = &files[i]; break; }
        if (files[i].used < best->used) best = &files[i];
    }
    if (best->fd >= 0) close(best->fd);
    strcpy(best->path, path);
    best->fd = fd;
    best->writable = write;
    best->opened = t;
    best->used = ++clock_n;
    return fd;
}

static int dir_load(const char *path)
{
    if (dir_valid && !strcmp(dir_path, path) && now_ms() - dir_ms <= HFS64_TTL_MS) return 0;
    dir_valid = false;
    dir_n = 0;
    char leaf[256];
    int parent = walk(path, leaf);
    if (parent < 0) return amiga_error(errno);
    int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int e = errno;
    close(parent);
    if (fd < 0) return e == ENOTDIR ? E_OBJECT_WRONG_TYPE : amiga_error(e);
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return E_NO_FREE_STORE; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), ent->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) continue;    /* links, devices: not shown */
        if (dir_n == dir_cap) {
            unsigned cap = dir_cap ? dir_cap * 2 : 64;
            DirEntry *g = realloc(dir, cap * sizeof *g);
            if (!g) { closedir(d); return E_NO_FREE_STORE; }
            dir = g;
            dir_cap = cap;
        }
        strncpy(dir[dir_n].name, ent->d_name, 255);
        dir[dir_n].name[255] = 0;
        dir[dir_n].st = st;
        dir_n++;
    }
    closedir(d);
    strcpy(dir_path, path);
    dir_ms = now_ms();
    dir_valid = true;
    return 0;
}

/* the guest's string at addr: false when not 0 ended within max bytes inside RAM */
static bool guest_string(const uint8_t *ram, uint64_t size, uint64_t addr, char *out, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (addr + i >= size) return false;
        char c = (char)ram[addr + i];
        out[i] = c;
        if (!c) return true;
    }
    return false;
}

void qrx_ec64_hostfs_call(uint8_t *ram, uint64_t ram_size, uint8_t *q)
{
    uint32_t a[13];
    for (unsigned i = 0; i < 13; i++) a[i] = be32(q + 16 + i * 4);
    uint32_t op = be32(q + 12);
    uint64_t path_at = (uint64_t)a[0] << 32 | a[1], buf = (uint64_t)a[2] << 32 | a[3];
    uint64_t off = (uint64_t)a[5] << 32 | a[6];
    uint32_t len = a[4];
    int err = 0;
    uint64_t moved = 0, size = 0;
    uint32_t aux = 0;
    char path[QRX_HFS64_PATH_MAX + 1];

    pthread_mutex_lock(&lock);
    if (root < 0) { err = E_NO_DISK; goto out; }
    if (op != QRX_HFS64_INFO && op != QRX_HFS64_SYNC && !guest_string(ram, ram_size, path_at, path, sizeof path)) { err = E_LINE_TOO_LONG; goto out; }
    /* a buffer the call writes or reads must lie inside RAM */
    if ((op == QRX_HFS64_STAT || op == QRX_HFS64_READDIR || op == QRX_HFS64_READ || op == QRX_HFS64_WRITE) &&
        (buf > ram_size || len > ram_size - buf)) { err = E_SEEK_ERROR; goto out; }
    if (read_only && op >= QRX_HFS64_WRITE && op <= QRX_HFS64_SETDATE && !(op == QRX_HFS64_OPEN && a[7] == 0)) { err = E_DISK_WRITE_PROTECTED; goto out; }

    switch (op) {
    case QRX_HFS64_STAT: {
        if (len < QRX_HFS64_RECORD) { err = E_LINE_TOO_LONG; break; }
        char leaf[256];
        int parent = walk(path, leaf);
        if (parent < 0) { err = amiga_error(errno); break; }
        struct stat st;
        if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW)) err = amiga_error(errno);
        else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) err = E_OBJECT_WRONG_TYPE;
        else { record(ram + buf, &st, !*path); size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size; moved = QRX_HFS64_RECORD; }
        close(parent);
        break;
    }
    case QRX_HFS64_READDIR: {
        if ((err = dir_load(path))) break;
        if (a[7] >= dir_n) { err = E_NO_MORE_ENTRIES; break; }
        const DirEntry *e = &dir[a[7]];
        size_t n = strlen(e->name) + 1;
        if (len < QRX_HFS64_RECORD + n) { err = E_LINE_TOO_LONG; break; }
        record(ram + buf, &e->st, false);
        memcpy(ram + buf + QRX_HFS64_RECORD, e->name, n);
        moved = QRX_HFS64_RECORD + n;
        break;
    }
    case QRX_HFS64_READ: case QRX_HFS64_WRITE: {
        if (len > QRX_HFS64_IO_MAX || off > (uint64_t)INT64_MAX - len) { err = E_SEEK_ERROR; break; }
        int fd = file_fd(path, op == QRX_HFS64_WRITE, &err);
        if (fd < 0) break;
        ssize_t n = op == QRX_HFS64_READ ? pread(fd, ram + buf, len, (off_t)off) : pwrite(fd, ram + buf, len, (off_t)off);
        if (n < 0) { err = amiga_error(errno); break; }
        moved = (uint64_t)n;
        struct stat st;
        if (!fstat(fd, &st)) size = (uint64_t)st.st_size;
        if (op == QRX_HFS64_WRITE) dir_valid = false;      /* sizes in the listing changed */
        break;
    }
    case QRX_HFS64_OPEN: {
        char leaf[256];
        int parent = walk(path, leaf);
        if (parent < 0) { err = amiga_error(errno); break; }
        if (!*path) { close(parent); err = E_OBJECT_WRONG_TYPE; break; }
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
        if (a[7] == 1) flags = O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC;
        if (a[7] == 2) flags = O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC;
        int fd = openat(parent, leaf, flags, 0644);
        if (fd < 0) err = amiga_error(errno);
        else {
            struct stat st;
            if (fstat(fd, &st) || !S_ISREG(st.st_mode)) err = E_OBJECT_WRONG_TYPE;
            else size = (uint64_t)st.st_size;
            close(fd);
            if (a[7]) changed();
        }
        close(parent);
        break;
    }
    case QRX_HFS64_DELETE: case QRX_HFS64_MKDIR: case QRX_HFS64_SETSIZE: case QRX_HFS64_PROTECT: case QRX_HFS64_SETDATE: {
        char leaf[256];
        int parent = walk(path, leaf);
        if (parent < 0) { err = amiga_error(errno); break; }
        changed();
        struct stat st;
        if (!*path && op != QRX_HFS64_SETDATE) { close(parent); err = E_OBJECT_WRONG_TYPE; break; }
        if (op == QRX_HFS64_MKDIR) {
            if (mkdirat(parent, leaf, 0755)) err = amiga_error(errno);
        } else if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW)) err = amiga_error(errno);
        else if (op == QRX_HFS64_DELETE) {
            if (unlinkat(parent, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0)) err = amiga_error(errno);
        } else if (op == QRX_HFS64_SETSIZE) {
            int fd = openat(parent, leaf, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0 || ftruncate(fd, (off_t)off)) err = amiga_error(errno);
            else size = off;
            if (fd >= 0) close(fd);
        } else if (op == QRX_HFS64_PROTECT) {
            mode_t m = st.st_mode & 07777;
            m = (a[7] & FIBF_READ) ? (m & ~(mode_t)S_IRUSR) : (m | S_IRUSR);
            m = (a[7] & (FIBF_WRITE | FIBF_DELETE)) ? (m & ~(mode_t)S_IWUSR) : (m | S_IWUSR);
            if (fchmodat(parent, leaf, m, 0)) err = amiga_error(errno);
        } else {
            struct timespec t[2] = { { (time_t)(int64_t)off, 0 }, { (time_t)(int64_t)off, 0 } };
            if (utimensat(parent, leaf, t, AT_SYMLINK_NOFOLLOW)) err = amiga_error(errno);
        }
        close(parent);
        break;
    }
    case QRX_HFS64_RENAME: {
        char dest[QRX_HFS64_PATH_MAX + 1], from_leaf[256], to_leaf[256];
        if (!guest_string(ram, ram_size, buf, dest, sizeof dest) || !*dest || !*path) { err = E_INVALID_COMPONENT_NAME; break; }
        int from = walk(path, from_leaf);
        if (from < 0) { err = amiga_error(errno); break; }
        int to = walk(dest, to_leaf);
        if (to < 0) { err = amiga_error(errno); close(from); break; }
        changed();
        struct stat st;
        if (fstatat(from, from_leaf, &st, AT_SYMLINK_NOFOLLOW)) err = amiga_error(errno);
        /* a name that differs only in case is the same object: a rename that changes the case */
        else if (fstatat(to, to_leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && strcasecmp(path, dest)) err = E_OBJECT_EXISTS;
        else if (renameat(from, from_leaf, to, to_leaf)) err = amiga_error(errno);
        close(from);
        close(to);
        break;
    }
    case QRX_HFS64_INFO: {
        struct statvfs s;
        if (fstatvfs(root, &s)) { err = amiga_error(errno); break; }
        uint64_t total = (uint64_t)s.f_blocks * s.f_frsize / 4096, used = (uint64_t)(s.f_blocks - s.f_bfree) * s.f_frsize / 4096;
        if (total > INT32_MAX) total = INT32_MAX;
        if (used > total) used = total;
        size = total;
        moved = used;
        aux = read_only;
        break;
    }
    case QRX_HFS64_SYNC:
        for (unsigned i = 0; i < HFS64_FILES; i++) if (files[i].fd >= 0 && files[i].writable) fsync(files[i].fd);
#ifdef __linux__
        syncfs(root);
#else
        fsync(root);
#endif
        break;
    default:
        err = E_ACTION_NOT_KNOWN;
    }
out:
    pthread_mutex_unlock(&lock);
    {
        static int trace = -1;      /* QRX_HFS64_TRACE=1: every request on stderr */
        if (trace < 0) trace = getenv("QRX_HFS64_TRACE") != NULL;
        if (trace) fprintf(stderr, "[hostfs] op %u \"%s\" arg %u off %llu len %u -> err %d moved %llu size %llu\n", op,
                           op == QRX_HFS64_INFO || op == QRX_HFS64_SYNC ? "" : path, a[7], (unsigned long long)off, len, err,
                           (unsigned long long)moved, (unsigned long long)size);
    }
    put32(q + 16 + 8 * 4, (uint32_t)err);
    put32(q + 16 + 9 * 4, (uint32_t)moved);
    put32(q + 16 + 10 * 4, (uint32_t)(size >> 32));
    put32(q + 16 + 11 * 4, (uint32_t)size);
    put32(q + 16 + 12 * 4, aux);
}
