/* QRX OS One 22.5.11 - QRXAmigaRun / amigarun / amigacheck
 *
 * Native AROS m68k launcher for classic AmigaOS 1.x-3.x HUNK programs.
 *
 *   amigacheck [options] <program>          static + loader check, nothing is run
 *   amigarun   [options] <program> [args]   check, then start it as a DOS process
 *
 * The behaviour is selected by the program name (amigacheck / amigarun /
 * QRXAmigaRun) or by the CHECK option.  Only AROS' own exec/dos APIs are used:
 * Lock/Examine, Read, LoadSeg/UnLoadSeg, SystemTags, FindName/FindResident and
 * Fault.  Nothing here emulates AmigaOS 4, PowerPC or custom-chip hardware.
 *
 * Exit codes
 *   check mode : 0 runnable, no problem found
 *                5 runnable but warnings (missing library/device, overlay, Chip RAM, ...)
 *               10 not a runnable AmigaOS 1.x-3.x / AROS m68k program
 *               20 file missing/unreadable or invalid launcher arguments
 *   run mode   : the program's own return code; 20 if it could not be started
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "qrx_hunk.h"

#define QRX_AMIGARUN_VERSION "22.5.11"
#define QRX_ANALYZE_LIMIT (32UL * 1024UL * 1024UL)
#define QRX_LOG "T:QRX-AmigaRun.log"

static const char ver[] __attribute__((used)) = "$VER: QRXAmigaRun " QRX_AMIGARUN_VERSION " (28.10.2026) QRX OS One";

static int g_quiet;

/* ---------- output without stdio (Deadwood SDK stdio declarations are unreliable) ---------- */
static void out(const char *s) { PutStr((CONST_STRPTR)s); }
static void outn(long v)
{
    char t[16]; int i = 15, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    t[i] = 0;
    do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (neg) t[--i] = '-';
    out(t + i);
}
static void line2(const char *a, const char *b) { out(a); out(b); out("\n"); }

static void fault_text(LONG code, char *buf, LONG n)
{
    buf[0] = 0;
    if (code) Fault(code, NULL, (STRPTR)buf, n);
}

static void log_line(const char *mode, const char *prog, long rc, LONG res2)
{
    BPTR fh = Open((CONST_STRPTR)QRX_LOG, MODE_READWRITE);
    char n[16]; int i; unsigned long u;
    if (!fh) return;
    Seek(fh, 0, OFFSET_END);
    FPuts(fh, (CONST_STRPTR)"QRXAMIGARUN mode="); FPuts(fh, (CONST_STRPTR)mode);
    FPuts(fh, (CONST_STRPTR)" rc=");
    i = 15; n[i] = 0; u = rc < 0 ? (unsigned long)(-rc) : (unsigned long)rc;
    do { n[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (rc < 0) n[--i] = '-';
    FPuts(fh, (CONST_STRPTR)(n + i));
    FPuts(fh, (CONST_STRPTR)" result2=");
    i = 15; n[i] = 0; u = res2 < 0 ? 0 : (unsigned long)res2;
    do { n[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    FPuts(fh, (CONST_STRPTR)(n + i));
    FPuts(fh, (CONST_STRPTR)" program="); FPuts(fh, (CONST_STRPTR)prog);
    FPuts(fh, (CONST_STRPTR)"\n");
    Close(fh);
}

/* ---------- dependency probe: resident list, ROM resident modules, LIBS:/DEVS:, PROGDIR ---------- */
static int path_exists(const char *p)
{
    BPTR l;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;          /* never pop up "insert volume" requesters */
    l = Lock((CONST_STRPTR)p, SHARED_LOCK);
    me->pr_WindowPtr = old;
    if (!l) return 0;
    UnLock(l);
    return 1;
}

static void join(char *d, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 2 > n) { d[0] = 0; return; }
    memcpy(d, a, la);
    d[la] = 0;
    if (la && a[la - 1] != ':' && a[la - 1] != '/') { d[la++] = '/'; d[la] = 0; }
    memcpy(d + la, b, lb + 1);
}

static void probe_refs(struct qrx_hunk_report *r, const char *progdir)
{
    int i;
    char p[256];
    for (i = 0; i < r->ref_count; i++) {
        struct qrx_ref *x = &r->refs[i];
        struct Node *n;
        Forbid();
        n = FindName(x->is_device ? &SysBase->DeviceList : &SysBase->LibList, (CONST_STRPTR)x->name);
        Permit();
        if (n) { x->state = QRX_REF_RESIDENT; strcpy(x->where, "in memory"); continue; }
        if (FindResident((CONST_STRPTR)x->name)) { x->state = QRX_REF_RESIDENT; strcpy(x->where, "ROM resident module"); continue; }
        join(p, sizeof(p), x->is_device ? "DEVS:" : "LIBS:", x->name);
        if (p[0] && path_exists(p)) { x->state = QRX_REF_FILE; strncpy(x->where, p, sizeof(x->where) - 1); continue; }
        if (progdir[0]) {
            char sub[300];
            join(sub, sizeof(sub), progdir, x->is_device ? "devs" : "libs");
            join(p, sizeof(p), sub, x->name);
            if (p[0] && path_exists(p)) { x->state = QRX_REF_FILE; strncpy(x->where, p, sizeof(x->where) - 1); continue; }
            join(p, sizeof(p), progdir, x->name);
            if (p[0] && path_exists(p)) { x->state = QRX_REF_FILE; strncpy(x->where, p, sizeof(x->where) - 1); continue; }
        }
        x->state = QRX_REF_MISSING;
    }
}

/* ---------- check ---------- */
struct check_result {
    int rc;                  /* check-mode exit code */
    int missing;
    struct qrx_hunk_report rep;
};

static struct check_result g_chk;     /* ~20 KB: keep it off the process stack */

static int do_check(const struct qrx_launch_req *q, struct check_result *c)
{
    BPTR lock, fh;
    struct FileInfoBlock *fib;
    LONG size = 0, prot = 0, err;
    char ebuf[100], progdir[256];
    UBYTE *buf = NULL;
    int i, analysed = 0;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;

    memset(c, 0, sizeof(*c));
    me->pr_WindowPtr = (APTR)-1;
    lock = Lock((CONST_STRPTR)q->program, SHARED_LOCK);
    err = IoErr();
    me->pr_WindowPtr = oldwin;
    if (!lock) {
        fault_text(err, ebuf, sizeof(ebuf));
        out("QRXAMIGARUN ERROR: cannot access "); out(q->program); out(": "); out(ebuf[0] ? ebuf : "not found"); out("\n");
        c->rc = RETURN_FAIL;
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(lock, fib)) {
        if (fib) FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        out("QRXAMIGARUN ERROR: Examine failed\n");
        c->rc = RETURN_FAIL;
        return 0;
    }
    if (fib->fib_DirEntryType > 0) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        line2("QRXAMIGARUN ERROR: is a directory, not a program: ", q->program);
        c->rc = RETURN_FAIL;
        return 0;
    }
    size = fib->fib_Size;
    prot = fib->fib_Protection;
    FreeDosObject(DOS_FIB, fib);

    /* PROGDIR of the target, for program-local libs/ lookups */
    {
        BPTR parent = ParentDir(lock);
        progdir[0] = 0;
        if (parent) {
            if (!NameFromLock(parent, (STRPTR)progdir, sizeof(progdir))) progdir[0] = 0;
            UnLock(parent);
        }
    }
    UnLock(lock);

    if (size > 0 && (ULONG)size <= QRX_ANALYZE_LIMIT)
        buf = (UBYTE *)AllocVec((ULONG)size, MEMF_ANY);
    if (size == 0 || buf) {
        fh = Open((CONST_STRPTR)q->program, MODE_OLDFILE);
        if (fh) {
            LONG got = size ? Read(fh, buf, size) : 0;
            Close(fh);
            if (got == size) {
                qrx_hunk_analyze(buf, (qrx_u32)size, &c->rep);
                analysed = 1;
            }
        }
    }
    if (buf) FreeVec(buf);
    if (!analysed) {
        out("QRXAMIGARUN WARNING: file too large or not readable for structural analysis; loader test only\n");
        c->rep.runnable = 1;
        c->rep.kind = QRX_BIN_UNKNOWN;
    }
    if (c->rep.ref_count) probe_refs(&c->rep, progdir);

    /* Real proof that AROS can load it: the system loader itself. */
    if (c->rep.runnable && !q->noload) {
        BPTR seg = LoadSeg((CONST_STRPTR)q->program);
        if (!seg) {
            err = IoErr();
            fault_text(err, ebuf, sizeof(ebuf));
            c->rep.runnable = 0;
            strcpy(c->rep.error, "AROS LoadSeg() rejected the file: ");
            strncat(c->rep.error, ebuf[0] ? ebuf : "unknown error", sizeof(c->rep.error) - strlen(c->rep.error) - 1);
        } else {
            UnLoadSeg(seg);
        }
    }

    for (i = 0; i < c->rep.ref_count; i++)
        if (c->rep.refs[i].state == QRX_REF_MISSING) c->missing++;

    if (!c->rep.runnable) c->rc = RETURN_ERROR;
    else if (c->missing || c->rep.warn_count || (prot & FIBF_EXECUTE)) c->rc = RETURN_WARN;
    else c->rc = RETURN_OK;

    if (!q->quiet || c->rc) {
        out("QRXAMIGARUN CHECK "); out(q->program); out("\n");
        line2("  format   : ", analysed ? qrx_bin_kind_name(c->rep.kind) : "not analysed");
        out("  size     : "); outn(size); out(" bytes\n");
        if (c->rep.kind == QRX_BIN_AMIGA_HUNK_EXE) {
            out("  hunks    : "); outn((long)c->rep.hunk_count);
            out("  relocs: "); outn((long)c->rep.reloc_total);
            out("  chip: "); outn((long)c->rep.chip_bytes);
            out("  fast: "); outn((long)c->rep.fast_bytes);
            out("  any: "); outn((long)c->rep.any_bytes); out(" bytes\n");
            for (i = 0; i < (int)c->rep.hunk_count && i < 16; i++) {
                out("    hunk "); outn(i); out(" "); out(qrx_hunk_type_name(c->rep.hunks[i].type));
                out(" "); outn((long)c->rep.hunks[i].alloc_bytes); out(" bytes\n");
            }
        }
        if (prot & FIBF_EXECUTE) out("  WARNING  : protection bit 'e' is cleared; the shell refuses to run it\n");
        for (i = 0; i < c->rep.warn_count; i++) line2("  note     : ", c->rep.warn[i]);
        for (i = 0; i < c->rep.ref_count; i++) {
            const struct qrx_ref *x = &c->rep.refs[i];
            out(x->is_device ? "  device   : " : "  library  : "); out(x->name); out(" -> ");
            if (x->state == QRX_REF_MISSING) out("MISSING");
            else { out("ok ("); out(x->where); out(")"); }
            out("\n");
        }
        if (c->rep.ref_count)
            out("  (library/device names are found by scanning the program image; optional or\n"
                "   version-specific requirements cannot be decided statically)\n");
        if (!c->rep.runnable) line2("  RESULT   : NOT RUNNABLE - ", c->rep.error);
        else if (c->rc == RETURN_WARN) out("  RESULT   : RUNNABLE WITH WARNINGS\n");
        else out("  RESULT   : RUNNABLE (structure + AROS LoadSeg OK)\n");
    }
    return 1;
}

static const char *base_name(const char *s)
{
    const char *p = s, *b = s;
    for (; *p; p++) if (*p == '/' || *p == ':') b = p + 1;
    return b;
}

static void usage(void)
{
    out("QRXAmigaRun " QRX_AMIGARUN_VERSION " - classic AmigaOS 1.x-3.x HUNK launcher for AROS m68k\n"
        "  amigacheck [STRICT] [NOLOAD] <program>\n"
        "  amigarun   [ASYNC] [QUIET] [FORCE] [STRICT] [STACK=<bytes>] <program> [arguments]\n"
        "  QRXAmigaRun CHECK ... | QRXAmigaRun ...   (same, explicit)\n"
        "Quote paths with spaces: amigarun \"Work:My Game/Game\" -level 2\n"
        "Use -- before a program that is named like an option.\n");
}

int main(int argc, char **argv)
{
    static struct qrx_launch_req q;
    static char cmd[1700];
    char name[64], ebuf[100];
    const char *raw;
    long rc;
    LONG res2;
    struct CommandLineInterface *cli;
    LONG oldstack = 0;

    (void)argv;
    if (argc == 0) return RETURN_FAIL;      /* Workbench start is not supported */
    raw = (const char *)GetArgStr();
    if (!qrx_parse_launch_line(raw, &q)) {
        if (q.error[0] && strcmp(q.error, "no program given")) line2("QRXAMIGARUN ERROR: ", q.error);
        usage();
        return RETURN_FAIL;
    }
    g_quiet = q.quiet;
    name[0] = 0;
    GetProgramName((STRPTR)name, sizeof(name));
    if (!strcmp(base_name(name), "amigacheck") || !strcmp(base_name(name), "AmigaCheck")) q.check_only = 1;

    do_check(&q, &g_chk);
    if (q.check_only) {
        log_line("check", q.program, g_chk.rc, 0);
        return g_chk.rc;
    }
    if (g_chk.rc >= RETURN_FAIL) { log_line("run", q.program, -1, 0); return RETURN_FAIL; }
    if (g_chk.rc >= RETURN_ERROR && !q.force) {
        out("QRXAMIGARUN: not started (use FORCE to try anyway)\n");
        log_line("run", q.program, -1, 0);
        return RETURN_FAIL;
    }
    if (g_chk.missing && q.strict) {
        out("QRXAMIGARUN: not started, STRICT and a library/device is missing\n");
        log_line("run", q.program, -1, 0);
        return RETURN_FAIL;
    }
    if (!qrx_build_command(&q, cmd, sizeof(cmd))) {
        out("QRXAMIGARUN ERROR: command line too long or contains unsupported control characters\n");
        return RETURN_FAIL;
    }

    cli = Cli();
    if (q.stack && cli) {
        oldstack = cli->cli_DefaultStack;               /* in longwords */
        cli->cli_DefaultStack = (LONG)((q.stack + 3UL) / 4UL);
    }
    if (q.async) {
        BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
        BPTR o = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)in, SYS_Output, (IPTR)o,
                        SYS_Asynch, TRUE, SYS_UserShell, TRUE, TAG_DONE);
        res2 = IoErr();
        if (rc == -1) { if (in) Close(in); if (o) Close(o); }
    } else {
        rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)Input(), SYS_Output, (IPTR)Output(),
                        SYS_UserShell, TRUE, TAG_DONE);
        res2 = IoErr();
    }
    if (q.stack && cli) cli->cli_DefaultStack = oldstack;

    if (rc == -1) {
        fault_text(res2, ebuf, sizeof(ebuf));
        out("QRXAMIGARUN ERROR: could not start "); out(q.program); out(": "); out(ebuf[0] ? ebuf : "unknown"); out("\n");
        log_line(q.async ? "async" : "run", q.program, -1, res2);
        return RETURN_FAIL;
    }
    if (q.async) {
        if (!q.quiet) line2("QRXAMIGARUN: started asynchronously: ", q.program);
        log_line("async", q.program, 0, 0);
        return RETURN_OK;
    }
    if (!q.quiet || rc) {
        out("QRXAMIGARUN RESULT rc="); outn(rc); out(" ("); out(qrx_rc_class(rc)); out(")");
        if (rc && res2) {
            fault_text(res2, ebuf, sizeof(ebuf));
            out(" result2="); outn(res2);
            if (ebuf[0]) { out(" "); out(ebuf); }
        }
        out("\n");
    }
    log_line("run", q.program, rc, res2);
    return (int)rc;
}
