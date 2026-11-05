/* QRX OS One 22.5.12 - qrx-win32: HLE infrastructure (thunks, guest heap,
 * strings, process start). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "qrxwin.h"

/* ---------------------------------------------------------------- logging */
void qw_log(qwin *w, const char *fmt, ...)
{
    va_list ap;
    FILE *f = w && w->log ? w->log : stderr;
    va_start(ap, fmt);
    fputs("qrx-win32: ", f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    va_end(ap);
}

/* ---------------------------------------------------------------- strings */
int wlen16(const wchar16 *s) { int n = 0; while (s[n]) n++; return n; }
void wcpy16(wchar16 *d, const wchar16 *s, int max) { int i = 0; if (max <= 0) return; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
void a2w(wchar16 *d, const char *s, int max) { int i = 0; if (max <= 0) return; while (s[i] && i < max - 1) { d[i] = (uint8_t)s[i]; i++; } d[i] = 0; }
void w2a(char *d, const wchar16 *s, int max) { int i = 0; if (max <= 0) return; while (s[i] && i < max - 1) { d[i] = s[i] < 256 ? (char)s[i] : '?'; i++; } d[i] = 0; }
int wicmp16(const wchar16 *a, const wchar16 *b)
{
    for (;; a++, b++) {
        wchar16 x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (wchar16)(x - 32);
        if (y >= 'a' && y <= 'z') y = (wchar16)(y - 32);
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
}
int qw_rd_wstr(qwin *w, uint32_t a, wchar16 *out, int max)
{
    int i;
    if (!a) { if (max) out[0] = 0; return 0; }
    for (i = 0; i < max - 1; i++) { out[i] = x86_rd16(&w->cpu, a + (uint32_t)i * 2); if (!out[i]) return i; }
    out[i] = 0;
    return i;
}
int qw_rd_str(qwin *w, uint32_t a, char *out, int max)
{
    int i;
    if (!a) { if (max) out[0] = 0; return 0; }
    for (i = 0; i < max - 1; i++) { out[i] = (char)x86_rd8(&w->cpu, a + (uint32_t)i); if (!out[i]) return i; }
    out[i] = 0;
    return i;
}
int qw_wr_wstr(qwin *w, uint32_t a, const wchar16 *s, int max)
{
    int i;
    if (!a || max <= 0) return 0;
    for (i = 0; s[i] && i < max - 1; i++) x86_wr16(&w->cpu, a + (uint32_t)i * 2, s[i]);
    x86_wr16(&w->cpu, a + (uint32_t)i * 2, 0);
    return i;
}

/* ---------------------------------------------------------------- guest heap */
typedef struct qblk { uint32_t a, n; struct qblk *next; } qblk;
#define QW_HBUCKETS 4096
static qblk *g_hash[QW_HBUCKETS];

static unsigned hb(uint32_t a) { return (a >> 4) & (QW_HBUCKETS - 1); }

#ifdef __AROS__
#include <proto/exec.h>
#include <exec/memory.h>
#define QW_HOST_RESERVE (3u * 1024u * 1024u)
#endif
uint32_t qw_alloc(qwin *w, uint32_t n)
{
    qblk **pp, *b;
    uint32_t a;
    if (!n) n = 1;
    n = (n + 15u) & ~15u;
    for (pp = &w->freel; *pp; pp = &(*pp)->next) {       /* first fit */
        b = *pp;
        if (b->n >= n) {
            a = b->a;
            if (b->n - n >= 32) { b->a += n; b->n -= n; }
            else { n = b->n; *pp = b->next; free(b); }
            goto got;
        }
    }
    if ((uint64_t)w->heap_top + n > QW_HEAP_LIMIT) return 0;
    a = w->heap_top;
    if (a + n > w->heap_mapped) {
        uint32_t need = ((a + n - w->heap_mapped) + 0xFFFFu) & ~0xFFFFu;
#ifdef __AROS__
        /* Programs probe with ever smaller requests until one succeeds: refuse
         * early, so a too large probe neither pages the machine dry nor leaves
         * the operating system without memory. */
        if ((uint64_t)AvailMem(MEMF_ANY) < (uint64_t)need + QW_HOST_RESERVE) return 0;
#endif
        if (!x86_map(&w->cpu, w->heap_mapped, need, 1)) {
            x86_unmap(&w->cpu, w->heap_mapped, need);
            return 0;
        }
        w->heap_mapped += need;
    }
    w->heap_top = a + n;
got:
    b = (qblk *)malloc(sizeof(*b));
    if (!b) return 0;
    b->a = a; b->n = n; b->next = g_hash[hb(a)]; g_hash[hb(a)] = b;
    {   /* fresh memory is zeroed (several programs rely on calloc-like behaviour) */
        static const uint8_t zero[4096];
        uint32_t i;
        for (i = 0; i < n; i += sizeof(zero)) x86_write_mem(&w->cpu, a + i, zero, n - i < sizeof(zero) ? n - i : (uint32_t)sizeof(zero));
    }
    return a;
}

uint32_t qw_size(qwin *w, uint32_t a)
{
    qblk *b;
    (void)w;
    for (b = g_hash[hb(a)]; b; b = b->next) if (b->a == a) return b->n;
    return 0;
}

void qw_free(qwin *w, uint32_t a)
{
    qblk **pp, *b;
    if (!a) return;
    for (pp = &g_hash[hb(a)]; *pp; pp = &(*pp)->next) {
        b = *pp;
        if (b->a == a) {
            *pp = b->next;
            b->next = w->freel;
            w->freel = b;
            return;
        }
    }
}

uint32_t qw_data(qwin *w, uint32_t n)
{
    uint32_t a = w->data_next;
    n = (n + 15u) & ~15u;
    if (a + n > QW_DATA_BASE + QW_DATA_SIZE) return 0;
    w->data_next += n;
    return a;
}

/* ---------------------------------------------------------------- thunks */
static const qhle *tables[] = { qw_compat, qw_net, qw_dplay, qw_files, qw_extra, qw_kernel32, qw_user32, qw_gdi32, qw_misc, qw_ansi, qw_dx, NULL };

static int dllcmp(const char *a, const char *b)
{
    while (*a && *b && *b != '.') { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 1; a++; b++; }
    if (*a == '.' && (!*b || *b == '.')) return 0;
    return !(*a == 0 && (*b == 0 || *b == '.'));
}

const qhle *qw_find_hle(const char *dll, const char *name)
{
    int t;
    const qhle *e;
    for (t = 0; tables[t]; t++)
        for (e = tables[t]; e->name; e++)
            if (!strcmp(e->name, name) && (!dll || !dllcmp(e->dll, dll))) return e;
    return strstr(name, "::") ? qw_find_dx(name) : NULL;      /* COM methods (dx.c) */
}

uint32_t qw_data_import(qwin *w, const char *dll, const char *name);

uint32_t qw_thunk(qwin *w, const char *dll, const char *name)
{
    const qhle *e = qw_find_hle(dll, name);
    uint32_t i, d;
    if (!e && (d = qw_data_import(w, dll, name)) != 0) return d;
    for (i = 0; i < w->nthunk; i++)
        if (w->thunk[i] == e && !strcmp(w->thunk_name[i], name) && (e || 1)) return QW_THUNK_BASE + i;
    if (w->nthunk >= QW_THUNK_MAX) return 0;
    i = w->nthunk++;
    w->thunk[i] = e;
    snprintf(w->thunk_name[i], sizeof(w->thunk_name[i]), "%s", name);
    if (!e && w->verbose) qw_log(w, "import %s!%s is not implemented (fails only if called)", dll ? dll : "?", name);
    return QW_THUNK_BASE + i;
}

static uint32_t door_alloc_cb(x86cpu *c, uint32_t size) { return qw_alloc((qwin *)c->user, size); }
#define DOOR_SVC_API 2u
/* block: service, serial, dir, op(0 query, 1 bind), thunk index, scratch, result, name[64] */
static int host_api_call(qwin *w, uint32_t op, uint32_t idx, const char *name)
{
    static uint32_t block[7 + 16];
    static uint32_t scratch;
    if (!w->cpu.door) return 0;
    if (!scratch) scratch = qw_alloc(w, 4096);
    block[0] = DOOR_SVC_API; block[3] = op; block[4] = idx; block[5] = scratch; block[6] = 0;
    memset(block + 7, 0, 64);
    strncpy((char *)(block + 7), name, 63);
    if (!x86_door_call(&w->cpu, block)) return 0;
    return (int)block[6];
}
int qw_host_bind(qwin *w, uint32_t idx) { return host_api_call(w, 1, idx, w->thunk_name[idx]); }
int qw_host_has(qwin *w, const char *name) { return host_api_call(w, 0, 0, name); }
/* -v: which API functions take the time, printed with the frame rate. */
static uint32_t g_api_calls[QW_THUNK_MAX], g_api_ms[QW_THUNK_MAX];
void qw_api_report(qwin *w)
{
    int n;
    for (n = 0; n < 6; n++) {
        uint32_t best = 0;
        int i, b = -1;
        for (i = 0; i < w->nthunk; i++) if (g_api_ms[i] * 1000u + g_api_calls[i] > best) { best = g_api_ms[i] * 1000u + g_api_calls[i]; b = i; }
        if (b < 0) break;
        qw_log(w, "  API %s: %u calls, %u ms", w->thunk_name[b], g_api_calls[b], g_api_ms[b]);
        g_api_calls[b] = g_api_ms[b] = 0;
    }
    memset(g_api_calls, 0, sizeof(g_api_calls));
    memset(g_api_ms, 0, sizeof(g_api_ms));
}

static int hle_hook(x86cpu *c, uint32_t idx)
{
    qwin *w = (qwin *)c->user;
    const qhle *e = idx < w->nthunk ? w->thunk[idx] : NULL;
    uint32_t a[16], ret, r, sp = c->r[X86_ESP];
    int i;
#ifdef QRX_EXEC_PROFILE
    uint64_t profile_start = w->be->now(w);
    if (idx < QW_THUNK_MAX) w->prof_api_hits[idx]++;
#endif
    w->api_calls++;
    /* The door stops here for a thunk it does not know yet. If the host has the
     * function it takes this and all later calls without coming back. */
    if (!e && idx < w->nthunk && qw_host_bind(w, idx)) return 1;
    if (!e) {
        qw_log(w, "unimplemented API called: %s", idx < w->nthunk ? w->thunk_name[idx] : "?");
        c->fault = X86_FAULT_HOST;
        snprintf(c->fault_msg, sizeof(c->fault_msg), "unimplemented API %s", idx < w->nthunk ? w->thunk_name[idx] : "?");
        c->fault_eip = c->cur_eip;
        return 0;
    }
    ret = x86_rd32(c, sp);
    for (i = 0; i < 16; i++) a[i] = x86_rd32(c, sp + 4 + (uint32_t)i * 4);
    if (w->trace) qw_log(w, "-> %s(%08x, %08x, %08x, %08x, %08x, %08x, %08x) ret=%08x", e->name, a[0], a[1], a[2], a[3], a[4], a[5], a[6], ret);
    if (w->verbose) {
        uint64_t t0 = w->be->now(w);
        r = e->fn(w, a);
        g_api_calls[idx]++;
        g_api_ms[idx] += (uint32_t)(w->be->now(w) - t0);
        /* Also while no frames come: a stalled program shows where it spends its time. */
        {
            static uint64_t last;
            uint64_t now = w->be->now(w);
            if (!last) last = now;
            if (now - last >= 10000u) { last = now; qw_log(w, "API time over the last 10 s, %d frames so far:", w->frames); qw_api_report(w); }
        }
    } else
    r = e->fn(w, a);
#ifdef QRX_EXEC_PROFILE
    {
        uint64_t end = w->be->now(w);
        if (idx < QW_THUNK_MAX && end >= profile_start)
            w->prof_api_ms[idx] += end - profile_start;
    }
#endif
    if (w->trace) qw_log(w, "<- %s = %08x", e->name, r);
    if (c->fault) return 1;
    if (w->exiting) { c->stop = 1; return 1; }
    c->r[X86_ESP] = sp + 4 + (e->nargs > 0 ? 4u * (uint32_t)e->nargs : 0u);
    c->r[X86_EAX] = r;
    c->eip = ret;
    if (!(w->api_calls & 31u)) qw_thread_tick(w);
    return 1;
}

/* call a guest (or host thunk) function with up to 4 stdcall/cdecl arguments */
uint32_t qw_call(qwin *w, uint32_t fn, int n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t args[4];
    int ok;
    args[0] = a0; args[1] = a1; args[2] = a2; args[3] = a3;
    if (!fn) return 0;
    if (fn - QW_THUNK_BASE < w->nthunk) {
        const qhle *e = w->thunk[fn - QW_THUNK_BASE];
        uint32_t a[16];
        int i;
        if (!e) { qw_log(w, "call to unimplemented %s", w->thunk_name[fn - QW_THUNK_BASE]); return 0; }
        for (i = 0; i < 16; i++) a[i] = i < n ? args[i] : 0;
        return e->fn(w, a);
    }
    if (w->exiting || w->cpu.fault) return 0;
    return x86_call(&w->cpu, fn, n, args, &ok);
}

/* ---------------------------------------------------------------- process start */
int qw_load(qwin *w, const char *path, int argc, char **argv)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    x86cpu *c = &w->cpu;
    int i, k;
    if (!f) { qw_log(w, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    snprintf(w->exe_path, sizeof(w->exe_path), "%s", path);
    x86_init(c);
    c->user = w;
    c->hle_base = QW_THUNK_BASE;
    c->hle_count = QW_THUNK_MAX;
    /* A program can import a variable where we have a function: it then reads the thunk's
     * address as data. Zeroes there let it go on; execution never reaches this memory. */
    x86_map(c, QW_THUNK_BASE, QW_THUNK_MAX, 0);
    c->hle_hook = hle_hook;
    w->data_next = QW_DATA_BASE;
    x86_map(c, QW_DATA_BASE, QW_DATA_SIZE, 1);
    w->heap_top = w->heap_mapped = QW_HEAP_BASE;
    w->rand_seed = 1;
    /* command line: "prog" args */
    {
        char cl[1024];
        const char *base = strrchr(path, '/');
        int L;
        base = base ? base + 1 : path;
        L = snprintf(cl, sizeof(cl), "\"%s\"", base);
        for (i = 1; i < argc && L < (int)sizeof(cl) - 2; i++) L += snprintf(cl + L, sizeof(cl) - (size_t)L, " %s", argv[i]);
        a2w(w->cmdline, cl, 512);
    }
    user_init(w);
    gdi_init(w);
    if (!pe_load(w, b, (size_t)n)) { free(b); return 0; }
    free(b);
    /* stack, TEB, PEB */
    x86_map(c, QW_STACK_TOP - QW_STACK_SIZE, QW_STACK_SIZE, 1);
    x86_map(c, QW_TEB, 0x2000, 1);
    x86_map(c, QW_PEB, 0x1000, 1);
    x86_wr32(c, QW_TEB + 0x00, 0xFFFFFFFFu);
    x86_wr32(c, QW_TEB + 0x04, QW_STACK_TOP);
    x86_wr32(c, QW_TEB + 0x08, QW_STACK_TOP - QW_STACK_SIZE);
    x86_wr32(c, QW_TEB + 0x18, QW_TEB);
    x86_wr32(c, QW_TEB + 0x20, 0x100);
    x86_wr32(c, QW_TEB + 0x24, 0x104);
    x86_wr32(c, QW_TEB + 0x2C, QW_TEB + 0x1000);
    x86_wr32(c, QW_TEB + 0x30, QW_PEB);
    x86_wr32(c, QW_PEB + 0x08, w->image_base);
    x86_wr32(c, QW_PEB + 0x18, 0x00150000u);
    c->sreg[X86_FS] = 0x3B;
    c->sbase[X86_FS] = QW_TEB;
    c->r[X86_ESP] = QW_STACK_TOP - 64;
    k = 0;
    (void)k;
    return 1;
}

int qw_run(qwin *w)
{
    x86cpu *c = &w->cpu;
    int f;
    uint32_t exit_thunk = qw_thunk(w, "kernel32.dll", "ExitProcess");
    /* the entry point returns into ExitProcess(eax) */
    c->r[X86_ESP] -= 4;
    x86_wr32(c, c->r[X86_ESP], 0);              /* argument slot for ExitProcess */
    c->r[X86_ESP] -= 4;
    x86_wr32(c, c->r[X86_ESP], exit_thunk);
    w->started = 1;
    pe_init_dlls(w);                              /* DllMain of DLLs loaded with the program */
    c->eip = w->image_base + w->entry;
    /* SetEnv QRX86_HOST 1 in the board's startup: QRX86 runs on the host CPU. */
    if (getenv("QRX86_HOST") && x86_door_enable(c)) { c->door_alloc = door_alloc_cb; qw_log(w, "x86 runs on the host door"); }
    f = w->cpu.fault ? w->cpu.fault : x86_run(c, 0xFFFFFFFFu);
    if (f && !w->exiting) {
        char d[400];
        x86_describe_fault(c, d, sizeof(d));
        qw_log(w, "program stopped: %s", d);
        {
            uint32_t sp = w->cpu.r[X86_ESP];
            qw_log(w, "stack: %08x %08x %08x %08x %08x %08x %08x %08x", x86_rd32(&w->cpu, sp), x86_rd32(&w->cpu, sp + 4), x86_rd32(&w->cpu, sp + 8), x86_rd32(&w->cpu, sp + 12),
                   x86_rd32(&w->cpu, sp + 16), x86_rd32(&w->cpu, sp + 20), x86_rd32(&w->cpu, sp + 24), x86_rd32(&w->cpu, sp + 28));
        }
        return 139;
    }
    return (int)w->exit_code;
}
