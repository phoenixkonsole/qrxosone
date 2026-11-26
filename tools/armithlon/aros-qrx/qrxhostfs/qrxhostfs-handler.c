/*
 * qrxhostfs-handler: an AmigaDOS volume on a host directory, for QRX OS One 64 (AROS on the
 * 68100EC64). The emulator does the file work (host service 12, engine/include/qrx_ec64_hostfs.h):
 * this handler turns AmigaDOS packets into paths relative to the shared directory and back. No disk
 * format in between: the host's own file system holds the files, with names of up to 255 bytes and
 * 64-bit sizes, and its page cache is the only cache (a crash of the guest loses nothing written).
 *
 * Mounted from a DOSDriver (DEVS:DOSDrivers/HOST: Handler = L:qrxhostfs-handler), or as the boot
 * volume: the same source built as a kickstart module (module/, -DQRXHOSTFS_MODULE) registers DosType
 * QHF\1 in FileSystem.resource, and qrxdisk.device adds the boot node HFS0: for it when the host shares
 * a directory (the board's feature bit). The volume's name is the DOSDriver's Startup string, else
 * "Host"; the boot volume is QRXSYS.
 *
 * A lock (struct HLock) and an open file (struct HFile) each hold their object's path relative to the
 * share ("" is the share itself, components separated by '/'). Names in packets are resolved against
 * a lock's path the AmigaDOS way: a "DEV:" part is dropped (dos.library passes the matching lock),
 * each leading '/' goes up one directory, "a//b" is a/../b. ExNext's position is the entry's index in fib_DiskKey.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <aros/asmcall.h>
#include <string.h>

#define PATHMAX 1024
#define HFS_SERVICE 12
#define HFS_RECORD 64
#define HOST_CALL ((volatile UQUAD *)0xFFFFFFFF00000400ULL)
enum { HFS_STAT = 1, HFS_READDIR, HFS_READ, HFS_WRITE, HFS_OPEN, HFS_DELETE, HFS_MKDIR, HFS_RENAME,
       HFS_SETSIZE, HFS_PROTECT, HFS_SETDATE, HFS_INFO, HFS_SYNC };
#define UNIX_TO_AMIGA 252460800   /* seconds from 1970-01-01 to 1978-01-01 */

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

struct HLock { struct FileLock fl; struct HLock *next; char path[PATHMAX]; };
struct HFile { struct HFile *next; char path[PATHMAX]; UQUAD pos; LONG mode; };
static struct HLock *locks;
static struct HFile *files;
static struct MsgPort *port;
static struct DosList *volume;
static char volname[108] = "Host";

/* ---- the host call: one request at a time (this handler is one task) */
static ULONG req[20] __attribute__((aligned(8)));
static ULONG r_err, r_moved;
static UQUAD r_size;
static LONG host(ULONG op, const char *path, void *buf, ULONG len, UQUAD off, ULONG arg)
{
    memset(req, 0, sizeof req);
    req[0] = HFS_SERVICE;
    req[3] = op;
    req[4] = (ULONG)((UQUAD)(IPTR)path >> 32); req[5] = (ULONG)(IPTR)path;
    req[6] = (ULONG)((UQUAD)(IPTR)buf >> 32);  req[7] = (ULONG)(IPTR)buf;
    req[8] = len;
    req[9] = (ULONG)(off >> 32); req[10] = (ULONG)off;
    req[11] = arg;
    /* the host reads the request and writes its results and buffers during this store: the compiler
     * must neither keep the request in registers across it nor assume req still holds what it wrote */
    __asm__ __volatile__("" ::: "memory");
    *HOST_CALL = (UQUAD)(IPTR)req;
    __asm__ __volatile__("" ::: "memory");
    r_err = req[12];
    r_moved = req[13];
    r_size = (UQUAD)req[14] << 32 | req[15];
    return (LONG)r_err;
}
static UBYTE rec[HFS_RECORD + 256] __attribute__((aligned(8)));
static ULONG rd32(const UBYTE *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }
static UQUAD rd64(const UBYTE *p) { return (UQUAD)rd32(p) << 32 | rd32(p + 4); }

/* ---- paths */
static const char *leaf_of(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
static void up(char *path) { char *s = strrchr(path, '/'); if (s) *s = 0; else path[0] = 0; }
/* base (a path) and an AmigaDOS name into out; FALSE: above the root, or too long */
static BOOL resolve(const char *base, const char *name, char *out)
{
    /* "DEV:" is dropped; dos.library has already made the lock the one the name is relative to
     * (0 for a volume or device name, the assign's directory for an assign), as afs does */
    const char *colon = strchr(name, ':');
    if (colon) name = colon + 1;
    strcpy(out, base);
    while (*name) {
        if (*name == '/') {                       /* a leading or doubled '/': the parent */
            if (!out[0]) return FALSE;
            up(out);
            name++;
            continue;
        }
        const char *end = strchr(name, '/');
        size_t n = end ? (size_t)(end - name) : strlen(name);
        size_t l = strlen(out);
        if (l + (l ? 1 : 0) + n >= PATHMAX || n > 255) return FALSE;
        if (l) out[l++] = '/';
        memcpy(out + l, name, n);
        out[l + n] = 0;
        name += n;
        if (*name == '/') name++;                 /* the separator after a component */
    }
    return TRUE;
}
static struct HLock *lock_of(SIPTR b) { return b ? (struct HLock *)BADDR(b) : NULL; }
static const char *path_of(SIPTR b) { struct HLock *l = lock_of(b); return l ? l->path : ""; }
static int same_name(const char *a, const char *b)     /* equal without regard to (ASCII) case */
{
    for (;; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static BOOL valid_file(SIPTR a) { for (struct HFile *f = files; f; f = f->next) if ((SIPTR)f == a) return TRUE; return FALSE; }

static struct HLock *new_lock(const char *path, LONG access)
{
    struct HLock *l = AllocVec(sizeof *l, MEMF_ANY | MEMF_CLEAR);
    if (!l) return NULL;
    strcpy(l->path, path);
    l->fl.fl_Key = (IPTR)l;
    l->fl.fl_Access = access;
    l->fl.fl_Task = port;
    l->fl.fl_Volume = MKBADDR(volume);
    l->next = locks;
    locks = l;
    return l;
}
static void free_lock(struct HLock *l)
{
    for (struct HLock **p = &locks; *p; p = &(*p)->next) if (*p == l) { *p = l->next; break; }
    FreeVec(l);
}

/* the record in rec into a FileInfoBlock; name: the entry's name */
static void fill_fib(struct FileInfoBlock *fib, const char *name, BOOL root)
{
    LONG type = (LONG)rd32(rec);
    if (root) type = ST_ROOT;
    UQUAD size = rd64(rec + 8);
    QUAD t = (QUAD)rd64(rec + 16) - UNIX_TO_AMIGA;
    if (t < 0) t = 0;
    size_t n = strlen(name);
    if (n > sizeof fib->fib_FileName - 2) n = sizeof fib->fib_FileName - 2;
    fib->fib_DirEntryType = type;
    fib->fib_EntryType = type;
    memcpy(&fib->fib_FileName[1], name, n);
    fib->fib_FileName[1 + n] = 0;
    fib->fib_FileName[0] = (char)n;
    fib->fib_Protection = rd32(rec + 4);
    fib->fib_Size = size > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)size;
    fib->fib_NumBlocks = (LONG)((size + 511) / 512);
    fib->fib_Date.ds_Days = (LONG)(t / 86400);
    fib->fib_Date.ds_Minute = (LONG)(t % 86400 / 60);
    fib->fib_Date.ds_Tick = (LONG)(t % 60 * TICKS_PER_SECOND);
    fib->fib_Comment[0] = 0;
    fib->fib_Comment[1] = 0;
    fib->fib_OwnerUID = 0;
    fib->fib_OwnerGID = 0;
}

static char pbuf[PATHMAX], pbuf2[PATHMAX];
/* opens name (relative to lock) as file handle fh for packet type */
static LONG do_open(struct FileHandle *fh, SIPTR lock, const char *name, LONG type, SIPTR *res2)
{
    if (!resolve(path_of(lock), name, pbuf)) { *res2 = ERROR_OBJECT_NOT_FOUND; return DOSFALSE; }
    ULONG how = type == ACTION_FINDOUTPUT ? 2 : type == ACTION_FINDUPDATE ? 1 : 0;
    if (host(HFS_OPEN, pbuf, NULL, 0, 0, how)) { *res2 = r_err; return DOSFALSE; }
    struct HFile *f = AllocVec(sizeof *f, MEMF_ANY | MEMF_CLEAR);
    if (!f) { *res2 = ERROR_NO_FREE_STORE; return DOSFALSE; }
    strcpy(f->path, pbuf);
    f->mode = type;
    f->next = files;
    files = f;
    fh->fh_Arg1 = (SIPTR)f;
    fh->fh_Port = DOSFALSE;                       /* not interactive */
    return DOSTRUE;
}
static void free_file(struct HFile *f)
{
    for (struct HFile **p = &files; *p; p = &(*p)->next) if (*p == f) { *p = f->next; break; }
    FreeVec(f);
}

static LONG hostfs_run(struct ExecBase *sysBase)
{
    SysBase = sysBase;
    struct Process *me = (struct Process *)FindTask(NULL);
    struct DosPacket *dp;
    port = &me->pr_MsgPort;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    WaitPort(port);
    dp = (struct DosPacket *)GetMsg(port)->mn_Node.ln_Name;
    if (!DOSBase) { dp->dp_Res1 = DOSFALSE; dp->dp_Res2 = ERROR_NO_FREE_STORE; ReplyMsg(dp->dp_Link); return RETURN_FAIL; }
    /* the startup packet: dp_Arg1 the Startup string, dp_Arg3 the DeviceNode */
    {
        struct DeviceNode *dn0 = BADDR(dp->dp_Arg3);
        BOOL boot = dn0 && dn0->dn_Name && same_name(AROS_BSTR_ADDR(dn0->dn_Name), "HFS0");
        const char *s = !boot && dp->dp_Arg1 ? AROS_BSTR_ADDR(dp->dp_Arg1) : NULL;
        if (s && *s == '"') s++;              /* Startup = "Name" */
        if (s && *s) {
            size_t n = 0;
            while (s[n] && s[n] != ':' && s[n] != '"' && n < sizeof volname - 1) { volname[n] = s[n]; n++; }
            if (n) volname[n] = 0;
        }
        struct DeviceNode *dn = BADDR(dp->dp_Arg3);
        if (dn) dn->dn_Task = port;
        /* the boot volume (qrxdisk.device's boot node HFS0:, no Startup string but a
         * FileSysStartupMsg in dp_Arg1): the system's volume name */
        if (dn && dn->dn_Name && same_name(AROS_BSTR_ADDR(dn->dn_Name), "HFS0")) strcpy(volname, "QRXSYS");
    }
    if (host(HFS_STAT, "", rec, sizeof rec, 0, 0)) { ReplyPkt(dp, DOSFALSE, ERROR_NO_DISK); return RETURN_FAIL; }
    volume = MakeDosEntry((STRPTR)volname, DLT_VOLUME);
    if (!volume) { ReplyPkt(dp, DOSFALSE, ERROR_NO_FREE_STORE); return RETURN_FAIL; }
    volume->dol_Task = port;
    volume->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;
    DateStamp(&volume->dol_misc.dol_volume.dol_VolumeDate);
    AddDosEntry(volume);
    ReplyPkt(dp, DOSTRUE, 0);

    for (;;) {
        struct Message *m;
        WaitPort(port);
        while ((m = GetMsg(port)) != NULL) {
            dp = (struct DosPacket *)m->mn_Node.ln_Name;
            SIPTR r1 = DOSTRUE, r2 = 0;
            switch (dp->dp_Type) {
            case ACTION_READ: case ACTION_WRITE: case ACTION_SEEK: case ACTION_END: case ACTION_SET_FILE_SIZE:
            case ACTION_EXAMINE_FH: case ACTION_COPY_DIR_FH: case ACTION_PARENT_FH:
                if (!valid_file(dp->dp_Arg1)) {
                    ReplyPkt(dp, dp->dp_Type == ACTION_READ || dp->dp_Type == ACTION_WRITE || dp->dp_Type == ACTION_SEEK ? -1 : DOSFALSE, ERROR_INVALID_LOCK);
                    continue;
                }
            }
            switch (dp->dp_Type) {
            case ACTION_LOCATE_OBJECT: {
                if (!resolve(path_of(dp->dp_Arg1), AROS_BSTR_ADDR(dp->dp_Arg2), pbuf)) { r1 = DOSFALSE; r2 = ERROR_OBJECT_NOT_FOUND; break; }
                if (host(HFS_STAT, pbuf, rec, sizeof rec, 0, 0)) { r1 = DOSFALSE; r2 = r_err; break; }
                struct HLock *l = new_lock(pbuf, dp->dp_Arg3 == EXCLUSIVE_LOCK ? EXCLUSIVE_LOCK : SHARED_LOCK);
                if (!l) { r1 = DOSFALSE; r2 = ERROR_NO_FREE_STORE; break; }
                r1 = (SIPTR)MKBADDR(l);
                break;
            }
            case ACTION_FREE_LOCK:
                if (lock_of(dp->dp_Arg1)) free_lock(lock_of(dp->dp_Arg1));
                break;
            case ACTION_COPY_DIR: case ACTION_PARENT: case ACTION_COPY_DIR_FH: case ACTION_PARENT_FH: {
                const char *p = dp->dp_Type == ACTION_COPY_DIR_FH || dp->dp_Type == ACTION_PARENT_FH
                                ? ((struct HFile *)dp->dp_Arg1)->path : path_of(dp->dp_Arg1);
                strcpy(pbuf, p);
                if (dp->dp_Type == ACTION_PARENT || dp->dp_Type == ACTION_PARENT_FH) {
                    if (!pbuf[0]) { r1 = 0; r2 = 0; break; }     /* the root has no parent */
                    up(pbuf);
                }
                struct HLock *l = new_lock(pbuf, SHARED_LOCK);
                if (!l) { r1 = DOSFALSE; r2 = ERROR_NO_FREE_STORE; break; }
                r1 = (SIPTR)MKBADDR(l);
                break;
            }
            case ACTION_SAME_LOCK:
                r1 = same_name(path_of(dp->dp_Arg1), path_of(dp->dp_Arg2)) ? DOSTRUE : DOSFALSE;
                break;
            case ACTION_EXAMINE_OBJECT: case ACTION_EXAMINE_FH: {
                const char *p = dp->dp_Type == ACTION_EXAMINE_FH ? ((struct HFile *)dp->dp_Arg1)->path : path_of(dp->dp_Arg1);
                struct FileInfoBlock *fib = BADDR(dp->dp_Arg2);
                if (host(HFS_STAT, p, rec, sizeof rec, 0, 0)) { r1 = DOSFALSE; r2 = r_err; break; }
                fill_fib(fib, p[0] ? leaf_of(p) : volname, !p[0]);
                fib->fib_DiskKey = 0;              /* ExNext: the index of the next entry */
                break;
            }
            case ACTION_EXAMINE_NEXT: {
                struct FileInfoBlock *fib = BADDR(dp->dp_Arg2);
                ULONG i = (ULONG)fib->fib_DiskKey;
                if (host(HFS_READDIR, path_of(dp->dp_Arg1), rec, sizeof rec, 0, i)) { r1 = DOSFALSE; r2 = r_err; break; }
                fill_fib(fib, (const char *)rec + HFS_RECORD, FALSE);
                fib->fib_DiskKey = i + 1;
                break;
            }
            case ACTION_FINDINPUT: case ACTION_FINDOUTPUT: case ACTION_FINDUPDATE:
                r1 = do_open(BADDR(dp->dp_Arg1), dp->dp_Arg2, AROS_BSTR_ADDR(dp->dp_Arg3), dp->dp_Type, &r2);
                break;
            case ACTION_FH_FROM_LOCK: {
                struct HLock *l = lock_of(dp->dp_Arg2);
                r1 = do_open(BADDR(dp->dp_Arg1), 0, l ? l->path : "", ACTION_FINDINPUT, &r2);
                if (r1 && l) {
                    /* the lock is the handle's now: a path is enough */
                    struct HFile *f = (struct HFile *)((struct FileHandle *)BADDR(dp->dp_Arg1))->fh_Arg1;
                    strcpy(f->path, l->path);
                    free_lock(l);
                }
                break;
            }
            case ACTION_END:
                free_file((struct HFile *)dp->dp_Arg1);
                break;
            case ACTION_READ: case ACTION_WRITE: {
                struct HFile *f = (struct HFile *)dp->dp_Arg1;
                ULONG len = (ULONG)dp->dp_Arg3, done = 0;
                UBYTE *b = (UBYTE *)dp->dp_Arg2;
                if (dp->dp_Type == ACTION_WRITE && f->mode == ACTION_FINDINPUT) {
                    /* MODE_OLDFILE may write too, as on AmigaDOS's own file systems */
                }
                while (done < len) {
                    ULONG n = len - done > 0x100000 ? 0x100000 : len - done;
                    if (host(dp->dp_Type == ACTION_READ ? HFS_READ : HFS_WRITE, f->path, b + done, n, f->pos, 0)) break;
                    f->pos += r_moved;
                    done += r_moved;
                    if (r_moved < n) break;
                }
                if (r_err && !done) { r1 = -1; r2 = r_err; break; }
                r1 = (SIPTR)done;
                break;
            }
            case ACTION_SEEK: {
                struct HFile *f = (struct HFile *)dp->dp_Arg1;
                QUAD old = (QUAD)f->pos, to = (QUAD)dp->dp_Arg2;
                if (host(HFS_STAT, f->path, rec, sizeof rec, 0, 0)) { r1 = -1; r2 = r_err; break; }
                QUAD size = (QUAD)rd64(rec + 8);
                if (dp->dp_Arg3 == OFFSET_CURRENT) to += old;
                else if (dp->dp_Arg3 == OFFSET_END) to += size;
                /* past the end is allowed, as on Unix: C programs (SQLite) seek there and write, and
                 * the host fills the gap with zeros */
                (void)size;
                if (to < 0) { r1 = -1; r2 = ERROR_SEEK_ERROR; break; }
                f->pos = (UQUAD)to;
                r1 = (SIPTR)old;
                break;
            }
            case ACTION_SET_FILE_SIZE: {
                struct HFile *f = (struct HFile *)dp->dp_Arg1;
                QUAD to = (QUAD)dp->dp_Arg2;
                if (dp->dp_Arg3 != OFFSET_BEGINNING) {
                    if (host(HFS_STAT, f->path, rec, sizeof rec, 0, 0)) { r1 = -1; r2 = r_err; break; }
                    to += dp->dp_Arg3 == OFFSET_CURRENT ? (QUAD)f->pos : (QUAD)rd64(rec + 8);
                }
                if (to < 0) { r1 = -1; r2 = ERROR_SEEK_ERROR; break; }
                if (host(HFS_SETSIZE, f->path, NULL, 0, (UQUAD)to, 0)) { r1 = -1; r2 = r_err; break; }
                if (f->pos > (UQUAD)to) f->pos = (UQUAD)to;
                r1 = (SIPTR)to;
                break;
            }
            case ACTION_DELETE_OBJECT:
                if (!resolve(path_of(dp->dp_Arg1), AROS_BSTR_ADDR(dp->dp_Arg2), pbuf) || !pbuf[0]) { r1 = DOSFALSE; r2 = ERROR_OBJECT_WRONG_TYPE; break; }
                if (host(HFS_DELETE, pbuf, NULL, 0, 0, 0)) { r1 = DOSFALSE; r2 = r_err; }
                break;
            case ACTION_RENAME_OBJECT:
                if (!resolve(path_of(dp->dp_Arg1), AROS_BSTR_ADDR(dp->dp_Arg2), pbuf) ||
                    !resolve(path_of(dp->dp_Arg3), AROS_BSTR_ADDR(dp->dp_Arg4), pbuf2)) { r1 = DOSFALSE; r2 = ERROR_OBJECT_NOT_FOUND; break; }
                if (host(HFS_RENAME, pbuf, pbuf2, PATHMAX, 0, 0)) { r1 = DOSFALSE; r2 = r_err; }
                break;
            case ACTION_CREATE_DIR: {
                if (!resolve(path_of(dp->dp_Arg1), AROS_BSTR_ADDR(dp->dp_Arg2), pbuf) || !pbuf[0]) { r1 = DOSFALSE; r2 = ERROR_OBJECT_EXISTS; break; }
                if (host(HFS_MKDIR, pbuf, NULL, 0, 0, 0)) { r1 = DOSFALSE; r2 = r_err; break; }
                struct HLock *l = new_lock(pbuf, SHARED_LOCK);
                if (!l) { r1 = DOSFALSE; r2 = ERROR_NO_FREE_STORE; break; }
                r1 = (SIPTR)MKBADDR(l);
                break;
            }
            case ACTION_SET_PROTECT:
                if (!resolve(path_of(dp->dp_Arg2), AROS_BSTR_ADDR(dp->dp_Arg3), pbuf)) { r1 = DOSFALSE; r2 = ERROR_OBJECT_NOT_FOUND; break; }
                if (host(HFS_PROTECT, pbuf, NULL, 0, 0, (ULONG)dp->dp_Arg4)) { r1 = DOSFALSE; r2 = r_err; }
                break;
            case ACTION_SET_DATE: {
                struct DateStamp *ds = (struct DateStamp *)dp->dp_Arg4;
                if (!resolve(path_of(dp->dp_Arg2), AROS_BSTR_ADDR(dp->dp_Arg3), pbuf)) { r1 = DOSFALSE; r2 = ERROR_OBJECT_NOT_FOUND; break; }
                UQUAD t = (UQUAD)ds->ds_Days * 86400 + (UQUAD)ds->ds_Minute * 60 + (UQUAD)ds->ds_Tick / TICKS_PER_SECOND + UNIX_TO_AMIGA;
                if (host(HFS_SETDATE, pbuf, NULL, 0, t, 0)) { r1 = DOSFALSE; r2 = r_err; }
                break;
            }
            case ACTION_SET_COMMENT:            /* comments are not kept on the host */
                break;
            case ACTION_INFO: case ACTION_DISK_INFO: {
                struct InfoData *id = BADDR((dp->dp_Type == ACTION_INFO ? dp->dp_Arg2 : dp->dp_Arg1));
                host(HFS_INFO, NULL, NULL, 0, 0, 0);
                memset(id, 0, sizeof *id);
                id->id_NumBlocks = (IPTR)r_size;
                id->id_NumBlocksUsed = (IPTR)r_moved;
                id->id_BytesPerBlock = 4096;
                id->id_DiskType = ID_DOS_DISK;
                id->id_DiskState = req[16] ? ID_WRITE_PROTECTED : ID_VALIDATED;
                id->id_VolumeNode = MKBADDR(volume);
                id->id_InUse = locks || files ? DOSTRUE : DOSFALSE;
                break;
            }
            case ACTION_FLUSH:
                host(HFS_SYNC, NULL, NULL, 0, 0, 0);
                break;
            case ACTION_IS_FILESYSTEM:
                break;
            case ACTION_CURRENT_VOLUME:
                r1 = (SIPTR)MKBADDR(volume);
                break;
            default:
                r1 = DOSFALSE;
                r2 = ERROR_ACTION_NOT_KNOWN;
                break;
            }
            ReplyPkt(dp, r1, r2);
        }
    }
    return RETURN_OK;
}

#ifdef QRXHOSTFS_MODULE
/* the kickstart module's handler (qrxhostfs.conf: handler_func, DosType QHF\1 in FileSystem.resource) */
LONG QRXHostFS_work(struct ExecBase *sysBase)
{
    return hostfs_run(sysBase);
}
#else
/* L:qrxhostfs-handler, started by a DOSDriver */
__startup static AROS_PROCH(qrxhostfs_main, argstr, argsize, sysBase)
{
    AROS_PROCFUNC_INIT
    return hostfs_run(sysBase);
    AROS_PROCFUNC_EXIT
}
#endif
