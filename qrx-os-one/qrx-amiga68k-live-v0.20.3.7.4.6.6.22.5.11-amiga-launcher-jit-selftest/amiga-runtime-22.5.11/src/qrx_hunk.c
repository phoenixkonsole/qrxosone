/* QRX OS One 22.5.11 - portable classic Amiga HUNK / executable-format analysis.
 * See qrx_hunk.h.  Only <string.h> is used so the same object builds with the
 * host compiler (tests) and the Deadwood m68k-aros-gcc (target) without the
 * Deadwood SDK stdio/snprintf declaration issues seen in 22.5.1-22.5.6.
 */
#include <string.h>
#include "qrx_hunk.h"

/* ---------- tiny bounded string helpers (no stdio dependency) ---------- */
static void qs_cpy(char *d, size_t n, const char *s)
{
    size_t i = 0;
    if (!n) return;
    while (s && s[i] && i + 1 < n) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void qs_cat(char *d, size_t n, const char *s)
{
    size_t l = strlen(d);
    if (l < n) qs_cpy(d + l, n - l, s);
}
static void qs_hex(char *d, size_t n, qrx_u32 v)
{
    char t[11]; int i;
    t[0] = '0'; t[1] = 'x';
    for (i = 0; i < 8; i++) t[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    t[10] = 0;
    qs_cat(d, n, t);
}
static void qs_dec(char *d, size_t n, qrx_u32 v)
{
    char t[12]; int i = 11;
    t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
    qs_cat(d, n, t + i);
}

static int fail(struct qrx_hunk_report *r, const char *msg, qrx_u32 val, int with_val)
{
    r->valid = 0;
    r->runnable = 0;
    qs_cpy(r->error, sizeof(r->error), msg);
    if (with_val) { qs_cat(r->error, sizeof(r->error), " "); qs_hex(r->error, sizeof(r->error), val); }
    return 0;
}

static void warn(struct qrx_hunk_report *r, const char *msg)
{
    if (r->warn_count < QRX_HUNK_MAX_WARN)
        qs_cpy(r->warn[r->warn_count++], QRX_HUNK_TEXT, msg);
}

/* ---------- bounds-checked big-endian reader ---------- */
struct rd { const unsigned char *b; qrx_u32 len, pos; };

static int rd32(struct rd *x, qrx_u32 *v)
{
    if (x->len - x->pos < 4 || x->pos > x->len) return 0;
    *v = ((qrx_u32)x->b[x->pos] << 24) | ((qrx_u32)x->b[x->pos + 1] << 16) |
         ((qrx_u32)x->b[x->pos + 2] << 8) | (qrx_u32)x->b[x->pos + 3];
    x->pos += 4;
    return 1;
}
static int rd16(struct rd *x, qrx_u32 *v)
{
    if (x->len - x->pos < 2 || x->pos > x->len) return 0;
    *v = ((qrx_u32)x->b[x->pos] << 8) | (qrx_u32)x->b[x->pos + 1];
    x->pos += 2;
    return 1;
}
static int skip(struct rd *x, qrx_u32 bytes)
{
    if (x->pos > x->len || x->len - x->pos < bytes) return 0;
    x->pos += bytes;
    return 1;
}
/* n longwords -> bytes, rejecting values that would overflow 32 bits. */
static int longs(qrx_u32 n, qrx_u32 *bytes)
{
    if (n > 0x3FFFFFFFUL) return 0;
    *bytes = n * 4UL;
    return 1;
}

const char *qrx_bin_kind_name(int kind)
{
    switch (kind) {
    case QRX_BIN_EMPTY:              return "empty file";
    case QRX_BIN_AMIGA_HUNK_EXE:     return "AmigaOS 1.x-3.x HUNK executable (m68k)";
    case QRX_BIN_AMIGA_HUNK_OBJECT:  return "Amiga HUNK object file (not linked)";
    case QRX_BIN_AMIGA_HUNK_LIBRARY: return "Amiga HUNK linker library (not a program)";
    case QRX_BIN_ELF_M68K_AROS:      return "AROS native ELF/m68k executable";
    case QRX_BIN_ELF_M68K_LINUX:     return "Linux/m68k ELF program";
    case QRX_BIN_ELF_PPC:            return "PowerPC ELF (AmigaOS 4 / MorphOS / Linux-PPC)";
    case QRX_BIN_ELF_OTHER:          return "ELF for a non-m68k CPU";
    case QRX_BIN_PE_MZ:              return "DOS/Windows MZ/PE image (x86)";
    case QRX_BIN_SCRIPT:             return "text / AmigaDOS or ARexx script";
    default:                         return "unknown format";
    }
}

const char *qrx_hunk_type_name(qrx_u32 t)
{
    switch (t) {
    case QRX_HUNK_CODE: return "CODE";
    case QRX_HUNK_DATA: return "DATA";
    case QRX_HUNK_BSS:  return "BSS";
    default:            return "?";
    }
}

/* ---------- library / device reference scan ---------- */
static int name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == '+';
}

static void add_ref(struct qrx_hunk_report *r, const unsigned char *s, qrx_u32 n, int dev)
{
    int i;
    char tmp[QRX_HUNK_NAME_LEN];
    if (n == 0 || n >= QRX_HUNK_NAME_LEN) return;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    for (i = 0; i < r->ref_count; i++)
        if (!strcmp(r->refs[i].name, tmp)) return;
    if (r->ref_count >= QRX_HUNK_MAX_REFS) return;
    qs_cpy(r->refs[r->ref_count].name, QRX_HUNK_NAME_LEN, tmp);
    r->refs[r->ref_count].is_device = dev;
    r->refs[r->ref_count].state = QRX_REF_UNCHECKED;
    r->refs[r->ref_count].where[0] = 0;
    r->ref_count++;
}

static void scan_refs(struct qrx_hunk_report *r, const unsigned char *p, qrx_u32 n)
{
    static const char *suf[2] = { ".library", ".device" };
    qrx_u32 i;
    int k;
    for (i = 0; i < n; i++) {
        for (k = 0; k < 2; k++) {
            qrx_u32 sl = (qrx_u32)strlen(suf[k]), start, end;
            if (p[i] != '.' || n - i < sl || memcmp(p + i, suf[k], sl)) continue;
            end = i + sl;
            if (end < n && name_char(p[end]) && p[end] != '.') continue;
            start = i;
            while (start > 0 && name_char(p[start - 1]) && i - start < QRX_HUNK_NAME_LEN) start--;
            /* must start with a letter/digit, not a dot: rejects "x..library" noise */
            while (start < i && !((p[start] >= 'a' && p[start] <= 'z') || (p[start] >= 'A' && p[start] <= 'Z') ||
                                  (p[start] >= '0' && p[start] <= '9')))
                start++;
            /* a string placed right after an RTS (4E 75 = "Nu") in a CODE hunk must
             * not absorb the opcode bytes: "\x4e\x75dos.library" is dos.library */
            while (i - start > 2 && p[start] == 0x4E && p[start + 1] == 0x75) start += 2;
            if (start < i) add_ref(r, p + start, end - start, k == 1);
        }
    }
}

/* ---------- format probes ---------- */
static int analyze_elf(const unsigned char *b, qrx_u32 len, struct qrx_hunk_report *r)
{
    int be;
    if (len < 52) { r->kind = QRX_BIN_ELF_OTHER; return fail(r, "truncated ELF header", len, 0); }
    if (b[4] != 1) { r->kind = QRX_BIN_ELF_OTHER; return fail(r, "64-bit ELF is not an Amiga program", 0, 0); }
    be = (b[5] == 2);
    r->elf_type = be ? ((unsigned)b[16] << 8 | b[17]) : ((unsigned)b[17] << 8 | b[16]);
    r->elf_machine = be ? ((unsigned)b[18] << 8 | b[19]) : ((unsigned)b[19] << 8 | b[18]);
    if (r->elf_machine == 4 && be) {
        if (r->elf_type == 1) {          /* ET_REL: how AROS stores native executables */
            r->kind = QRX_BIN_ELF_M68K_AROS;
            r->valid = r->runnable = 1;
            warn(r, "AROS-native ELF program, not a classic AmigaOS binary");
            return 1;
        }
        r->kind = QRX_BIN_ELF_M68K_LINUX;
        return fail(r, "Linux/m68k ELF: use 'linuxrun' (QRXLinuxRun), not the AmigaOS launcher", 0, 0);
    }
    if (r->elf_machine == 20 || r->elf_machine == 21) {
        r->kind = QRX_BIN_ELF_PPC;
        return fail(r, "PowerPC ELF (AmigaOS 4/MorphOS/Linux-PPC) is NOT supported; see PPC-ARCHITECTURE-PLAN-22.5.11.txt", 0, 0);
    }
    r->kind = QRX_BIN_ELF_OTHER;
    return fail(r, "ELF for foreign CPU, e_machine", r->elf_machine, 1);
}

static int looks_like_text(const unsigned char *b, qrx_u32 len)
{
    qrx_u32 i, n = len < 512 ? len : 512;
    for (i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0) return 0;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != 0x1b) return 0;
    }
    return n > 0;
}

/* ---------- HUNK load file ---------- */
static int analyze_hunk(const unsigned char *b, qrx_u32 len, struct qrx_hunk_report *r)
{
    struct rd x;
    qrx_u32 v, n, bytes, i, table, cur = 0, loaded = 0;
    int in_hunk = 0;

    x.b = b; x.len = len; x.pos = 4;
    r->kind = QRX_BIN_AMIGA_HUNK_EXE;

    for (;;) {                                  /* resident library names */
        if (!rd32(&x, &n)) return fail(r, "truncated HUNK_HEADER (resident name list)", 0, 0);
        if (!n) break;
        if (!longs(n, &bytes) || !skip(&x, bytes)) return fail(r, "corrupt resident name in HUNK_HEADER", n, 1);
        r->resident_names++;
    }
    if (r->resident_names)
        return fail(r, "HUNK_HEADER lists resident libraries; AmigaDOS LoadSeg rejects such files", 0, 0);

    if (!rd32(&x, &table) || !rd32(&x, &r->first_hunk) || !rd32(&x, &r->last_hunk))
        return fail(r, "truncated HUNK_HEADER (hunk table)", 0, 0);
    if (table == 0 || table > QRX_HUNK_MAX_HUNKS)
        return fail(r, "unsupported hunk table size", table, 1);
    if (r->first_hunk > r->last_hunk || r->last_hunk >= table)
        return fail(r, "inconsistent first/last hunk numbers", r->last_hunk, 1);
    if (r->first_hunk)
        warn(r, "first hunk is not 0 (overlay-style header)");
    r->hunk_count = r->last_hunk - r->first_hunk + 1;

    for (i = r->first_hunk; i <= r->last_hunk; i++) {
        qrx_u32 flags;
        if (!rd32(&x, &v)) return fail(r, "truncated hunk size table", i, 1);
        flags = v >> 30;
        if (!longs(v & 0x3FFFFFFFUL, &bytes)) return fail(r, "hunk size overflow", v, 1);
        r->hunks[i].alloc_bytes = bytes;
        r->hunks[i].memflags = flags;
        if (flags == 3 && !rd32(&x, &v)) return fail(r, "truncated extended memory attribute", i, 1);
        if (flags == 1) r->chip_bytes += bytes;
        else if (flags == 2) r->fast_bytes += bytes;
        else r->any_bytes += bytes;
    }

    cur = r->first_hunk;
    while (x.pos < x.len) {
        qrx_u32 type;
        if (x.len - x.pos < 4) {
            warn(r, "trailing bytes after last hunk ignored");
            break;
        }
        rd32(&x, &type);
        type &= 0x1FFFFFFFUL;                   /* strip CHIP/FAST/ADVISORY bits */
        switch (type) {
        case QRX_HUNK_CODE:
        case QRX_HUNK_DATA:
        case QRX_HUNK_BSS:
            if (in_hunk) return fail(r, "new hunk without HUNK_END at offset", x.pos - 4, 1);
            if (cur > r->last_hunk) return fail(r, "more hunks in file than declared in header", cur, 1);
            if (!rd32(&x, &n) || !longs(n, &bytes)) return fail(r, "truncated hunk length", cur, 1);
            if (bytes > r->hunks[cur].alloc_bytes)
                return fail(r, "hunk payload larger than header allocation, hunk", cur, 1);
            r->hunks[cur].type = type;
            if (type != QRX_HUNK_BSS) {
                r->hunks[cur].file_offset = x.pos;
                r->hunks[cur].file_bytes = bytes;
                if (!skip(&x, bytes)) return fail(r, "truncated hunk payload, hunk", cur, 1);
                scan_refs(r, b + r->hunks[cur].file_offset, bytes);
            }
            if (cur == r->first_hunk) r->first_is_code = (type == QRX_HUNK_CODE);
            in_hunk = 1;
            break;
        case QRX_HUNK_RELOC32:
        case QRX_HUNK_RELOC16:
        case QRX_HUNK_RELOC8:
        case QRX_HUNK_ABSRELOC16: {
            qrx_u32 width = type == QRX_HUNK_RELOC32 ? 4 : (type == QRX_HUNK_RELOC8 ? 1 : 2);
            if (!in_hunk) return fail(r, "relocation block outside a hunk at offset", x.pos - 4, 1);
            for (;;) {
                qrx_u32 cnt, target, k, off;
                if (!rd32(&x, &cnt)) return fail(r, "truncated relocation block", cur, 1);
                if (!cnt) break;
                if (!rd32(&x, &target)) return fail(r, "truncated relocation target", cur, 1);
                if (target < r->first_hunk || target > r->last_hunk)
                    return fail(r, "relocation references undeclared hunk", target, 1);
                for (k = 0; k < cnt; k++) {
                    if (!rd32(&x, &off)) return fail(r, "truncated relocation offsets", cur, 1);
                    if (off > r->hunks[cur].alloc_bytes || r->hunks[cur].alloc_bytes - off < width)
                        return fail(r, "relocation offset outside its hunk", off, 1);
                }
                r->hunks[cur].relocs += cnt;
                r->reloc_total += cnt;
            }
            break;
        }
        case QRX_HUNK_RELOC32SHORT:
        case QRX_HUNK_DREL32:                   /* V37+ executables use it as RELOC32SHORT */
        case QRX_HUNK_RELRELOC32: {
            qrx_u32 start = x.pos;
            if (!in_hunk) return fail(r, "short relocation block outside a hunk", x.pos - 4, 1);
            for (;;) {
                qrx_u32 cnt, target, k, off;
                if (!rd16(&x, &cnt)) return fail(r, "truncated short relocation block", cur, 1);
                if (!cnt) break;
                if (!rd16(&x, &target)) return fail(r, "truncated short relocation target", cur, 1);
                if (target < r->first_hunk || target > r->last_hunk)
                    return fail(r, "short relocation references undeclared hunk", target, 1);
                for (k = 0; k < cnt; k++) {
                    if (!rd16(&x, &off)) return fail(r, "truncated short relocation offsets", cur, 1);
                    if (off > r->hunks[cur].alloc_bytes || r->hunks[cur].alloc_bytes - off < 4)
                        return fail(r, "short relocation offset outside its hunk", off, 1);
                }
                r->hunks[cur].relocs += cnt;
                r->reloc_total += cnt;
            }
            if ((x.pos - start) & 2) {          /* blocks are padded to a longword */
                if (!skip(&x, 2)) return fail(r, "missing short relocation padding", cur, 1);
            }
            break;
        }
        case QRX_HUNK_SYMBOL:
            for (;;) {
                if (!rd32(&x, &n)) return fail(r, "truncated symbol table", cur, 1);
                n &= 0x00FFFFFFUL;
                if (!n) break;
                if (!longs(n, &bytes) || !skip(&x, bytes) || !skip(&x, 4))
                    return fail(r, "truncated symbol entry", cur, 1);
            }
            r->has_symbols = 1;
            break;
        case QRX_HUNK_DEBUG:
        case QRX_HUNK_NAME:
            if (!rd32(&x, &n) || !longs(n, &bytes) || !skip(&x, bytes))
                return fail(r, "truncated DEBUG/NAME block", cur, 1);
            if (type == QRX_HUNK_DEBUG) r->has_debug = 1;
            break;
        case QRX_HUNK_END:
            if (!in_hunk) return fail(r, "HUNK_END without hunk at offset", x.pos - 4, 1);
            in_hunk = 0;
            cur++;
            loaded++;
            break;
        case QRX_HUNK_OVERLAY:
            r->has_overlay = 1;
            warn(r, "overlay program: only the root node was validated; needs its overlay manager");
            goto done;
        case QRX_HUNK_BREAK:
            goto done;
        case QRX_HUNK_EXT:
            return fail(r, "HUNK_EXT in load file: unresolved externals (object file?)", 0, 0);
        default:
            return fail(r, "unknown hunk type", type, 1);
        }
    }
done:
    if (in_hunk && !r->has_overlay) { loaded++; warn(r, "last hunk has no HUNK_END"); }
    if (!r->has_overlay && loaded != r->hunk_count) {
        char m[QRX_HUNK_TEXT];
        m[0] = 0;
        qs_cat(m, sizeof(m), "truncated: only ");
        qs_dec(m, sizeof(m), loaded);
        qs_cat(m, sizeof(m), " of ");
        qs_dec(m, sizeof(m), r->hunk_count);
        qs_cat(m, sizeof(m), " declared hunks present");
        return fail(r, m, 0, 0);
    }
    if (!r->first_is_code) warn(r, "first hunk is not CODE (entry point is the start of hunk 0)");
    if (r->chip_bytes) warn(r, "requests Chip RAM (custom-chip DMA data: OCS/ECS/AGA hardware access likely)");
    r->valid = 1;
    r->runnable = 1;
    return 1;
}

int qrx_hunk_analyze(const unsigned char *buf, qrx_u32 len, struct qrx_hunk_report *r)
{
    memset(r, 0, sizeof(*r));
    r->file_size = len;
    if (!buf || len == 0) { r->kind = QRX_BIN_EMPTY; return fail(r, "file is empty", 0, 0); }
    if (len >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 0x03) {
        if (buf[3] == 0xF3) return analyze_hunk(buf, len, r);
        if (buf[3] == 0xE7) { r->kind = QRX_BIN_AMIGA_HUNK_OBJECT; return fail(r, "HUNK_UNIT object file: link it before running", 0, 0); }
        if (buf[3] == 0xFA) { r->kind = QRX_BIN_AMIGA_HUNK_LIBRARY; return fail(r, "HUNK_LIB linker library: not a program", 0, 0); }
    }
    if (len >= 4 && buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
        return analyze_elf(buf, len, r);
    if (len >= 2 && buf[0] == 'M' && buf[1] == 'Z') {
        r->kind = QRX_BIN_PE_MZ;
        return fail(r, "x86 DOS/Windows image: use 'winrun'/'wintrace' (QRX86), not the AmigaOS launcher", 0, 0);
    }
    if (looks_like_text(buf, len)) {
        r->kind = QRX_BIN_SCRIPT;
        return fail(r, "text file: run AmigaDOS scripts with 'Execute', ARexx with 'rx'", 0, 0);
    }
    r->kind = QRX_BIN_UNKNOWN;
    return fail(r, "not an Amiga load file; magic", len >= 4 ? ((qrx_u32)buf[0] << 24 | (qrx_u32)buf[1] << 16 | (qrx_u32)buf[2] << 8 | buf[3]) : 0, 1);
}

/* ---------- AmigaDOS quoting / command line ---------- */
int qrx_dos_quote(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    if (!in || !out || outsz < 3) return 0;
    out[o++] = '"';
    for (; *p; p++) {
        const char *e = NULL;
        char one[2];
        if (*p == '*') e = "**";
        else if (*p == '"') e = "*\"";
        else if (*p == '\n') e = "*N";
        else if (*p < 0x20 && *p != '\t') return 0;     /* CR, ESC, ... are refused */
        else { one[0] = (char)*p; one[1] = 0; e = one; }
        while (*e) {
            if (o + 2 >= outsz) return 0;
            out[o++] = *e++;
        }
    }
    out[o++] = '"';
    out[o] = 0;
    return 1;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int parse_ulong(const char *s, unsigned long *v)
{
    unsigned long x = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        if (x > 400000000UL) return 0;
        x = x * 10UL + (unsigned long)(*s - '0');
    }
    *v = x;
    return 1;
}

/* Read one AmigaDOS token at *pp into tok.  Returns 1 ok, 0 none, -1 error. */
static int next_token(const char **pp, char *tok, size_t n, struct qrx_launch_req *q)
{
    const char *p = *pp;
    size_t o = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '\r') { *pp = p; return 0; }
    if (*p == '"') {
        p++;
        for (;;) {
            char c = *p;
            if (!c || c == '\n') { qs_cpy(q->error, sizeof(q->error), "unterminated quoted argument"); return -1; }
            if (c == '"') { p++; break; }
            if (c == '*') {
                char e = p[1];
                if (e == '*' || e == '"') c = e;
                else if (e == 'N' || e == 'n') c = '\n';
                else if (e == 'E' || e == 'e') c = 0x1b;
                else { qs_cpy(q->error, sizeof(q->error), "invalid * escape in quoted argument"); return -1; }
                p++;
            }
            if (o + 1 >= n) { qs_cpy(q->error, sizeof(q->error), "argument too long"); return -1; }
            tok[o++] = c;
            p++;
        }
        if (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            qs_cpy(q->error, sizeof(q->error), "garbage directly after closing quote");
            return -1;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            if (o + 1 >= n) { qs_cpy(q->error, sizeof(q->error), "argument too long"); return -1; }
            tok[o++] = *p++;
        }
    }
    tok[o] = 0;
    *pp = p;
    return 1;
}

int qrx_parse_launch_line(const char *line, struct qrx_launch_req *q)
{
    const char *p = line ? line : "";
    char tok[512];
    int rc, opts = 1;
    memset(q, 0, sizeof(*q));
    for (;;) {
        const char *save = p;
        rc = next_token(&p, tok, sizeof(tok), q);
        if (rc < 0) return 0;
        if (rc == 0) { qs_cpy(q->error, sizeof(q->error), "no program given"); return 0; }
        if (opts && *save != '"' && *(save + strspn(save, " \t")) != '"') {
            if (!strcmp(tok, "--")) { opts = 0; continue; }
            if (ieq(tok, "CHECK") || ieq(tok, "-c") || ieq(tok, "--check")) { q->check_only = 1; continue; }
            if (ieq(tok, "ASYNC") || ieq(tok, "--async")) { q->async = 1; continue; }
            if (ieq(tok, "QUIET") || ieq(tok, "-q") || ieq(tok, "--quiet")) { q->quiet = 1; continue; }
            if (ieq(tok, "FORCE") || ieq(tok, "--force")) { q->force = 1; continue; }
            if (ieq(tok, "STRICT") || ieq(tok, "--strict")) { q->strict = 1; continue; }
            if (ieq(tok, "NOLOAD") || ieq(tok, "--noload")) { q->noload = 1; continue; }
            if ((tok[0] == 'S' || tok[0] == 's') && strlen(tok) > 6 && tok[5] == '=') {
                char kw[6];
                memcpy(kw, tok, 5); kw[5] = 0;
                if (ieq(kw, "STACK")) {
                    if (!parse_ulong(tok + 6, &q->stack)) { qs_cpy(q->error, sizeof(q->error), "STACK= needs a decimal byte count"); return 0; }
                    goto stack_check;
                }
            }
            if (ieq(tok, "--stack")) {
                if (next_token(&p, tok, sizeof(tok), q) != 1 || !parse_ulong(tok, &q->stack)) {
                    qs_cpy(q->error, sizeof(q->error), "--stack needs a decimal byte count");
                    return 0;
                }
            stack_check:
                if (q->stack < 4096UL || q->stack > 64UL * 1024UL * 1024UL) {
                    qs_cpy(q->error, sizeof(q->error), "stack must be 4096..67108864 bytes");
                    return 0;
                }
                continue;
            }
            if (tok[0] == '-' && tok[1] == '-') { qs_cpy(q->error, sizeof(q->error), "unknown option (use -- before a program named like an option)"); return 0; }
        }
        break;
    }
    if (!tok[0]) { qs_cpy(q->error, sizeof(q->error), "empty program name"); return 0; }
    qs_cpy(q->program, sizeof(q->program), tok);
    if (strlen(tok) >= sizeof(q->program)) { qs_cpy(q->error, sizeof(q->error), "program path too long"); return 0; }
    while (*p == ' ' || *p == '\t') p++;
    {
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) l--;
        if (l >= sizeof(q->args)) { qs_cpy(q->error, sizeof(q->error), "program arguments too long"); return 0; }
        memcpy(q->args, p, l);
        q->args[l] = 0;
        if (memchr(q->args, '\n', l) || memchr(q->args, '\r', l)) {
            qs_cpy(q->error, sizeof(q->error), "line breaks inside program arguments are refused");
            return 0;
        }
    }
    return 1;
}

int qrx_build_command(const struct qrx_launch_req *q, char *out, size_t outsz)
{
    size_t l;
    if (!qrx_dos_quote(q->program, out, outsz)) return 0;
    if (q->args[0]) {
        l = strlen(out);
        if (l + 1 + strlen(q->args) + 1 > outsz) return 0;
        out[l] = ' ';
        qs_cpy(out + l + 1, outsz - l - 1, q->args);
    }
    return 1;
}

const char *qrx_rc_class(long rc)
{
    if (rc < 0) return "NOT STARTED";
    if (rc == 0) return "OK";
    if (rc < 10) return "WARN";
    if (rc < 20) return "ERROR";
    return "FAIL";
}
