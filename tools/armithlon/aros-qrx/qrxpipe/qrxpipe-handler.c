/*
 * qrxpipe-handler: PIPE: for QRX OS One 64 (AROS on the 68100EC64), 64-bit clean.
 *
 * AROS's C library makes pipe() from two opens of PIPE:<name> (MODE_NEWFILE for the writer, then
 * MODE_OLDFILE for the reader), std::process and the shell's redirections use it. The old
 * pipe-handler in workbench/fs/pipe (1990, K&R C) does not start on this 64-bit system, so this one
 * replaces it, mounted by DEVS:DOSDrivers/PIPE.
 *
 * A pipe is created by the first open of its name and lives until its last opener closes it.
 * Writes never block: the data goes into a list of chunks. A read with nothing there waits (its
 * packet is kept) until data arrives or the last writer has closed; then it returns 0 (end of
 * file). A reader that opens before any writer waits the same way.
 *
 * Private packets for libqrxaroscompat (dp_Arg1 = fh_Arg1), answered with ERROR_ACTION_NOT_KNOWN
 * by every other handler, so they also tell a pipe from anything else:
 *   ACTION_QRX_PIPE_AVAIL  bytes waiting, or -1 at end of file (poll(); AROS's ACTION_WAIT_CHAR
 *                          carries no file handle)
 *   ACTION_QRX_PIPE_NAME   copies the pipe's name to dp_Arg2 (dp_Arg3 bytes): a child process gets
 *                          its own handle by opening PIPE:<name> again
 * Every packet's fh_Arg1 is checked against the opens of this handler first.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <aros/asmcall.h>
#include <string.h>

#define ACTION_QRX_PIPE_AVAIL 0x51525001
#define ACTION_QRX_PIPE_NAME 0x51525002
#define CHUNK 4096
#define NAMELEN 108

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

struct Chunk { struct Chunk *next; ULONG start, end; UBYTE data[CHUNK]; };
struct Pipe {
    struct Pipe *next;
    char name[NAMELEN];
    ULONG readers, writers;
    BOOL had_writer;
    struct Chunk *head, *tail;
    ULONG bytes;
    struct DosPacket *waiting[16];   /* reads waiting for data, in arrival order */
    ULONG nwait;
};
struct Open { struct Open *next; struct Pipe *pipe; LONG mode; };
static struct Open *opens;          /* every open handle, to check the packets' fh_Arg1 */
static struct Open *valid(SIPTR a) { for (struct Open *o = opens; o; o = o->next) if ((SIPTR)o == a) return o; return NULL; }

static struct Pipe *pipes;

static struct Pipe *find_pipe(const char *name, BOOL create)
{
    struct Pipe *p;
    for (p = pipes; p; p = p->next)
        if (!strcmp(p->name, name)) return p;
    if (!create) return NULL;
    p = AllocVec(sizeof *p, MEMF_ANY | MEMF_CLEAR);
    if (!p) return NULL;
    strncpy(p->name, name, NAMELEN - 1);
    p->next = pipes;
    pipes = p;
    return p;
}
static void free_pipe(struct Pipe *p)
{
    struct Pipe **pp;
    struct Chunk *c, *n;
    for (pp = &pipes; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    for (c = p->head; c; c = n) { n = c->next; FreeVec(c); }
    FreeVec(p);
}
/* copies up to len bytes out of the pipe */
static ULONG take(struct Pipe *p, UBYTE *buf, ULONG len)
{
    ULONG got = 0;
    while (got < len && p->head) {
        struct Chunk *c = p->head;
        ULONG k = c->end - c->start;
        if (k > len - got) k = len - got;
        CopyMem(c->data + c->start, buf + got, k);
        c->start += k; got += k; p->bytes -= k;
        if (c->start == c->end) { p->head = c->next; if (!p->head) p->tail = NULL; FreeVec(c); }
    }
    return got;
}
static LONG put(struct Pipe *p, const UBYTE *buf, ULONG len)
{
    ULONG done = 0;
    while (done < len) {
        struct Chunk *c = p->tail;
        if (!c || c->end == CHUNK) {
            c = AllocVec(sizeof *c, MEMF_ANY);
            if (!c) return done ? (LONG)done : -1;
            c->next = NULL; c->start = c->end = 0;
            if (p->tail) p->tail->next = c; else p->head = c;
            p->tail = c;
        }
        ULONG k = CHUNK - c->end;
        if (k > len - done) k = len - done;
        CopyMem((APTR)(buf + done), c->data + c->end, k);
        c->end += k; done += k; p->bytes += k;
    }
    return (LONG)done;
}
static BOOL at_eof(struct Pipe *p) { return !p->bytes && !p->writers && p->had_writer; }
/* answers the waiting reads that can be answered now */
static void serve(struct Pipe *p)
{
    while (p->nwait && (p->bytes || at_eof(p))) {
        struct DosPacket *dp = p->waiting[0];
        ULONG n = take(p, (UBYTE *)dp->dp_Arg2, (ULONG)dp->dp_Arg3);
        memmove(&p->waiting[0], &p->waiting[1], (p->nwait - 1) * sizeof p->waiting[0]);
        p->nwait--;
        ReplyPkt(dp, (SIPTR)n, 0);
    }
}
/* the name after the device part, without a trailing /size the old handler accepted */
static void pipe_name(const char *full, char *out)
{
    const char *c = strchr(full, ':');
    c = c ? c + 1 : full;
    strncpy(out, c, NAMELEN - 1);
    out[NAMELEN - 1] = 0;
    char *slash = strchr(out, '/');
    if (slash) *slash = 0;
}

static BOOL do_open(struct DosPacket *dp, LONG mode)
{
    struct FileHandle *fh = BADDR(dp->dp_Arg1);
    char name[NAMELEN];
    pipe_name((const char *)AROS_BSTR_ADDR(dp->dp_Arg3), name);
    struct Pipe *p = find_pipe(name, TRUE);
    struct Open *o = p ? AllocVec(sizeof *o, MEMF_ANY | MEMF_CLEAR) : NULL;
    if (!o) { if (p && !p->readers && !p->writers) free_pipe(p); SetIoErr(ERROR_NO_FREE_STORE); return FALSE; }
    o->pipe = p; o->mode = mode;
    o->next = opens; opens = o;
    if (mode == ACTION_FINDINPUT) p->readers++;
    else { p->writers++; p->had_writer = TRUE; if (mode == ACTION_FINDUPDATE) p->readers++; }
    fh->fh_Arg1 = (SIPTR)o;
    fh->fh_Port = DOSFALSE;     /* fh_Port is the "interactive" flag here: not interactive */
    return TRUE;
}
static void do_end(struct Open *o)
{
    struct Pipe *p = o->pipe;
    for (struct Open **op = &opens; *op; op = &(*op)->next) if (*op == o) { *op = o->next; break; }
    if (o->mode == ACTION_FINDINPUT) p->readers--;
    else { p->writers--; if (o->mode == ACTION_FINDUPDATE) p->readers--; }
    FreeVec(o);
    serve(p);                       /* the last writer gone: waiting reads see end of file */
    if (!p->readers && !p->writers) {
        while (p->nwait) { ReplyPkt(p->waiting[--p->nwait], 0, 0); }
        free_pipe(p);
    }
}

__startup static AROS_PROCH(qrxpipe_main, argstr, argsize, sysBase)
{
    AROS_PROCFUNC_INIT
    SysBase = sysBase;
    struct Process *me = (struct Process *)FindTask(NULL);
    struct MsgPort *port = &me->pr_MsgPort;
    struct DosPacket *dp;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    WaitPort(port);
    dp = (struct DosPacket *)GetMsg(port)->mn_Node.ln_Name;
    if (!DOSBase) { dp->dp_Res1 = DOSFALSE; dp->dp_Res2 = ERROR_NO_FREE_STORE; ReplyMsg(dp->dp_Link); return RETURN_FAIL; }
    {
        struct DeviceNode *dn = BADDR(dp->dp_Arg3);
        if (dn) dn->dn_Task = port;
    }
    ReplyPkt(dp, DOSTRUE, 0);

    for (;;) {
        struct Message *m;
        WaitPort(port);
        while ((m = GetMsg(port)) != NULL) {
            dp = (struct DosPacket *)m->mn_Node.ln_Name;
            SIPTR r1 = DOSTRUE, r2 = 0;
            switch (dp->dp_Type) {
            case ACTION_END: case ACTION_READ: case ACTION_WRITE: case ACTION_QRX_PIPE_AVAIL:
            case ACTION_QRX_PIPE_NAME:
                if (!valid(dp->dp_Arg1)) { ReplyPkt(dp, dp->dp_Type == ACTION_END ? DOSFALSE : -1, ERROR_INVALID_LOCK); continue; }
                break;
            }
            switch (dp->dp_Type) {
            case ACTION_FINDINPUT: case ACTION_FINDOUTPUT: case ACTION_FINDUPDATE:
                if (!do_open(dp, dp->dp_Type)) { r1 = DOSFALSE; r2 = IoErr(); }
                break;
            case ACTION_END:
                do_end((struct Open *)dp->dp_Arg1);
                break;
            case ACTION_READ: {
                struct Open *o = (struct Open *)dp->dp_Arg1;
                struct Pipe *p = o->pipe;
                if (o->mode == ACTION_FINDOUTPUT) { r1 = -1; r2 = ERROR_READ_PROTECTED; break; }
                if (!dp->dp_Arg3 || p->bytes || at_eof(p)) {
                    r1 = (SIPTR)take(p, (UBYTE *)dp->dp_Arg2, (ULONG)dp->dp_Arg3);
                    break;
                }
                if (p->nwait == sizeof p->waiting / sizeof p->waiting[0]) { r1 = -1; r2 = ERROR_TASK_TABLE_FULL; break; }
                p->waiting[p->nwait++] = dp;     /* answered by serve() */
                continue;
            }
            case ACTION_WRITE: {
                struct Open *o = (struct Open *)dp->dp_Arg1;
                if (o->mode == ACTION_FINDINPUT) { r1 = -1; r2 = ERROR_WRITE_PROTECTED; break; }
                r1 = put(o->pipe, (const UBYTE *)dp->dp_Arg2, (ULONG)dp->dp_Arg3);
                if (r1 < 0) r2 = ERROR_NO_FREE_STORE;
                serve(o->pipe);
                break;
            }
            case ACTION_QRX_PIPE_AVAIL: {
                struct Pipe *p = ((struct Open *)dp->dp_Arg1)->pipe;
                r1 = at_eof(p) ? -1 : (SIPTR)p->bytes;
                break;
            }
            case ACTION_QRX_PIPE_NAME: {
                struct Pipe *p = ((struct Open *)dp->dp_Arg1)->pipe;
                if ((ULONG)dp->dp_Arg3 <= strlen(p->name)) { r1 = DOSFALSE; r2 = ERROR_LINE_TOO_LONG; break; }
                strcpy((char *)dp->dp_Arg2, p->name);
                break;
            }
            case ACTION_WAIT_CHAR:
                r1 = DOSFALSE;          /* no file handle in this packet; see ACTION_QRX_PIPE_AVAIL */
                break;
            case ACTION_IS_FILESYSTEM:
                r1 = DOSFALSE;
                break;
            case ACTION_DELETE_OBJECT:   /* pipe() deletes the name when its second open fails */
                break;
            case ACTION_SEEK:
                r1 = -1; r2 = ERROR_ACTION_NOT_KNOWN;
                break;
            default:
                r1 = DOSFALSE; r2 = ERROR_ACTION_NOT_KNOWN;
                break;
            }
            ReplyPkt(dp, r1, r2);
        }
    }
    return RETURN_OK;
    AROS_PROCFUNC_EXIT
}
