/* QRX OS One 22.5.12 - qrx-win32: PE image loader and resource lookup. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "qrxwin.h"

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

#include <ctype.h>
#include <dirent.h>
#include <unistd.h>

static qmod *pe_map(qwin *w, const uint8_t *b, size_t n, const char *name, int is_dll);

static void lower_base(char *d, const char *s, size_t max)
{
    const char *b = strrchr(s, '/'), *b2 = strrchr(s, '\\'), *c2 = strrchr(s, ':');
    size_t k;
    if (b2 && (!b || b2 > b)) b = b2;
    if (c2 && (!b || c2 > b)) b = c2;
    b = b ? b + 1 : s;
    for (k = 0; b[k] && k + 1 < max; k++) d[k] = (char)tolower((unsigned char)b[k]);
    d[k] = 0;
    if (!strchr(d, '.') && k + 4 < max) strcat(d, ".dll");
}

qmod *pe_find_module(qwin *w, uint32_t hmod)
{
    int i;
    for (i = 0; i < w->nmod; i++) if (w->mod[i].base == hmod) return &w->mod[i];
    return NULL;
}

qmod *pe_module_by_name(qwin *w, const char *name)
{
    char n[64];
    int i;
    lower_base(n, name, sizeof(n));
    for (i = 0; i < w->nmod; i++) if (!strcmp(w->mod[i].name, n)) return &w->mod[i];
    return NULL;
}

/* a free, page-aligned range for a DLL that cannot use its preferred base */
static int range_free(qwin *w, uint32_t a, uint32_t size)
{
    uint32_t p;
    if (a < 0x00400000u || (uint64_t)a + size > QW_DLL_LIMIT) return 0;
    if (a < QW_HEAP_LIMIT && (uint64_t)a + size > QW_HEAP_BASE) return 0;
    for (p = 0; p < size; p += X86_PAGE_SIZE) if (x86_is_mapped(&w->cpu, a + p)) return 0;
    return 1;
}

static void apply_relocs(qwin *w, uint32_t base, uint32_t rva, uint32_t size, uint32_t delta)
{
    x86cpu *c = &w->cpu;
    uint32_t off = 0;
    while (off + 8 <= size) {
        uint32_t page = x86_rd32(c, base + rva + off), bs = x86_rd32(c, base + rva + off + 4), i;
        if (bs < 8) break;
        for (i = 8; i + 2 <= bs; i += 2) {
            uint16_t e = x86_rd16(c, base + rva + off + i);
            uint32_t at = base + page + (e & 0xFFFu);
            switch (e >> 12) {
            case 0: break;                                           /* padding */
            case 3: x86_wr32(c, at, x86_rd32(c, at) + delta); break; /* HIGHLOW */
            case 2: x86_wr16(c, at, (uint16_t)(x86_rd16(c, at) + delta)); break;
            case 1: x86_wr16(c, at, (uint16_t)(x86_rd16(c, at) + (delta >> 16))); break;
            default: qw_log(w, "unsupported relocation type %u", e >> 12); break;
            }
        }
        off += bs;
    }
}

int qw_is_hle_dll(const char *dll)
{
    static const char *hle[] = { "lz32", "kernel32", "user32", "gdi32", "advapi32", "msvcrt", "comctl32", "shell32", "winmm", "ntdll",
                                 "ddraw", "dsound", "dinput", "ole32", "oleaut32", "version", "imm32", "comdlg32", "msvcrt40", "crtdll", "dsetup", NULL };
    char n[64];
    size_t k;
    int i;
    for (k = 0; dll[k] && dll[k] != '.' && k < 63; k++) n[k] = (char)tolower((unsigned char)dll[k]);
    n[k] = 0;
    for (i = 0; hle[i]; i++) if (!strcmp(n, hle[i])) return 1;
    return 0;
}

uint32_t pe_export(qwin *w, qmod *m, const char *name, uint32_t ordinal)
{
    x86cpu *c = &w->cpu;
    uint32_t d, nfn, nnames, fns, names, ords, i, rva = 0;
    if (!m || !m->exp_rva) return 0;
    d = m->base + m->exp_rva;
    nfn = x86_rd32(c, d + 20); nnames = x86_rd32(c, d + 24);
    fns = m->base + x86_rd32(c, d + 28); names = m->base + x86_rd32(c, d + 32); ords = m->base + x86_rd32(c, d + 36);
    if (name) {
        for (i = 0; i < nnames; i++) {
            char s[96];
            qw_rd_str(w, m->base + x86_rd32(c, names + i * 4), s, sizeof(s));
            if (!strcmp(s, name)) { uint16_t o = x86_rd16(c, ords + i * 2); if (o < nfn) rva = x86_rd32(c, fns + o * 4u); break; }
        }
    } else {
        uint32_t o = ordinal - x86_rd32(c, d + 16);           /* minus ordinal base */
        if (o < nfn) rva = x86_rd32(c, fns + o * 4);
    }
    if (!rva) return 0;
    if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {  /* forwarder "DLL.Function" */
        char fw[128], *dot;
        qw_rd_str(w, m->base + rva, fw, sizeof(fw));
        dot = strchr(fw, '.');
        if (!dot) return 0;
        *dot = 0;
        if (qw_is_hle_dll(fw)) return qw_thunk(w, fw, dot + 1);
        return pe_export(w, pe_load_dll(w, fw), dot + 1, 0);
    }
    return m->base + rva;
}

/* locate NAME next to the program (case-insensitive, AmigaDOS-style file systems keep case) */
static int find_dll_file(qwin *w, const char *name, char *out, size_t max)
{
    char dir[1024], want[64];
    const char *sl = strrchr(w->exe_path, '/');
    DIR *d;
    struct dirent *e;
    lower_base(want, name, sizeof(want));
    if (sl) { size_t L = (size_t)(sl - w->exe_path); if (L >= sizeof(dir)) return 0; memcpy(dir, w->exe_path, L); dir[L] = 0; }
    else {
        const char *co = strrchr(w->exe_path, ':');
        if (co) { size_t L = (size_t)(co - w->exe_path) + 1; memcpy(dir, w->exe_path, L); dir[L] = 0; }
        else if (!getcwd(dir, sizeof(dir))) strcpy(dir, ".");   /* AROS posixc: "." is not a directory name */
    }
    /* A path in the name, as in LoadLibrary("TITLE\\OPENING\\OPENING.DLL"): below the program. */
    {
        const char *p = name, *end = NULL, *q;
        size_t L = strlen(dir);
        if (p[0] && p[1] == ':') p += 2;
        while (*p == '\\' || *p == '/' || (p[0] == '.' && (p[1] == '\\' || p[1] == '/'))) p += p[0] == '.' ? 2 : 1;
        for (q = p; *q; q++) if (*q == '\\' || *q == '/') end = q;
        if (end && L + 1 + (size_t)(end - p) + 1 < sizeof(dir)) {
            if (L && dir[L - 1] != ':' && dir[L - 1] != '/') dir[L++] = '/';
            for (q = p; q < end; q++) dir[L++] = *q == '\\' ? '/' : *q;
            dir[L] = 0;
        }
    }
    for (;;) {
    d = opendir(dir);
    if (!d && w->verbose) qw_log(w, "cannot list %s for %s", dir, want);
    if (d) {
    while ((e = readdir(d))) {
        char low[256];
        size_t k;
        for (k = 0; e->d_name[k] && k < 255; k++) low[k] = (char)tolower((unsigned char)e->d_name[k]);
        low[k] = 0;
        if (!strcmp(low, want)) {
            size_t L = strlen(dir);
            snprintf(out, max, "%s%s%s", dir, (L && dir[L - 1] != '/' && dir[L - 1] != ':') ? "/" : "", e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    }
    /* Then where tools/armithlon/install-directx.sh puts Microsoft's helper DLLs. */
    if (!strcmp(dir, "APPS:windows_system")) return 0;
    if (strcmp(dir, "SYS:WINDOWS/SYSTEM")) strcpy(dir, "SYS:WINDOWS/SYSTEM");      /* what installers put there */
    else strcpy(dir, "APPS:windows_system");
    }
}

qmod *pe_load_dll(qwin *w, const char *name)
{
    char path[1400], low[64];
    FILE *f;
    long n;
    uint8_t *b;
    qmod *m = pe_module_by_name(w, name);
    if (m) return m;
    lower_base(low, name, sizeof(low));
    if (!find_dll_file(w, name, path, sizeof(path))) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    m = pe_map(w, b, (size_t)n, low, 1);
    free(b);
    if (m && w->verbose) qw_log(w, "loaded %s at %08x (%u bytes)", m->name, m->base, m->size);
    if (m && w->started) pe_init_dlls(w);
    return m;
}

void pe_init_dlls(qwin *w)
{
    int i;
    for (i = 0; i < w->ninit; i++) {
        qmod *m = &w->mod[w->init_order[i]];
        if (m->inited) continue;
        m->inited = 1;
        if (m->entry) {
            uint32_t r = qw_call(w, m->base + m->entry, 3, m->base, 1 /* DLL_PROCESS_ATTACH */, 0, 0);
            if (w->verbose) qw_log(w, "DllMain(%s, PROCESS_ATTACH) = %u", m->name, r);
        }
    }
}

static qmod *pe_map(qwin *w, const uint8_t *b, size_t n, const char *name, int is_dll)
{
    x86cpu *c = &w->cpu;
    uint32_t pe, nsec, optsz, opt, base, pref, size, hdrsz, i, imp_rva, rel_rva, rel_size;
    qmod *m;
    int idx;
    if (n < 0x40 || b[0] != 'M' || b[1] != 'Z') { qw_log(w, "%s: not an MZ executable", name); return NULL; }
    pe = le32(b + 0x3C);
    if ((uint64_t)pe + 0xF8 > n || memcmp(b + pe, "PE\0\0", 4)) { qw_log(w, "%s: no PE header", name); return NULL; }
    if (le16(b + pe + 4) != 0x14C) { qw_log(w, "%s: not an i386 PE image (machine %04x)", name, le16(b + pe + 4)); return NULL; }
    if (w->nmod >= QW_MAXMOD) { qw_log(w, "%s: too many modules", name); return NULL; }
    nsec = le16(b + pe + 6);
    optsz = le16(b + pe + 20);
    opt = pe + 24;
    if (le16(b + opt) != 0x10B) { qw_log(w, "%s: not a PE32 image", name); return NULL; }
    pref = le32(b + opt + 28);
    size = le32(b + opt + 56);
    hdrsz = le32(b + opt + 60);
    imp_rva = le32(b + opt + 104);
    rel_rva = le32(b + opt + 136);
    rel_size = le32(b + opt + 140);
    if (!is_dll && le16(b + opt + 68) != 2 && le16(b + opt + 68) != 3) qw_log(w, "note: subsystem %u", le16(b + opt + 68));
    base = pref;
    if (!is_dll) {
        /* program: preferred base; relocate only if it collides with our fixed areas */
        if (base < 0x00400000u || (uint64_t)base + size > QW_HEAP_BASE) {
            if (!rel_size) { qw_log(w, "image base %08x unusable and no relocations", base); return NULL; }
            base = QW_DLL_BASE;
            while (!range_free(w, base, size) && base < QW_DLL_LIMIT) base += 0x00100000u;
        }
    } else if (!range_free(w, base, size)) {
        if (!rel_size) { qw_log(w, "%s: preferred base %08x in use and no relocations", name, pref); return NULL; }
        if (!w->dll_next) w->dll_next = QW_DLL_BASE;
        base = w->dll_next;
        while (!range_free(w, base, size)) { base += 0x00100000u; if (base >= QW_DLL_LIMIT) { qw_log(w, "%s: no address space", name); return NULL; } }
    }
    if (!x86_map(c, base, size, 1)) return NULL;
    if (is_dll && base + size > w->dll_next && base >= QW_DLL_BASE) w->dll_next = (base + size + 0xFFFFFu) & ~0xFFFFFu;
    x86_write_mem(c, base, b, hdrsz < n ? hdrsz : (uint32_t)n);
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = b + opt + optsz + i * 40;
        uint32_t vsz = le32(s + 8), va = le32(s + 12), rsz = le32(s + 16), rp = le32(s + 20);
        if (rsz > vsz && vsz) rsz = vsz;
        if ((uint64_t)rp + rsz > n) rsz = rp < n ? (uint32_t)(n - rp) : 0;
        /* Raw pointer 0 means uninitialised data, whatever the size field says
         * (Watcom-linked programs such as Doom95 fill it in for .bss). */
        if (rsz && rp) x86_write_mem(c, base + va, b + rp, rsz);
    }
    if (base != pref) {
        apply_relocs(w, base, rel_rva, rel_size, base - pref);
        if (w->verbose) qw_log(w, "%s relocated %08x -> %08x", name, pref, base);
    }
    idx = w->nmod++;
    m = &w->mod[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->base = base; m->size = size; m->is_dll = is_dll;
    m->entry = le32(b + opt + 16);
    m->res_rva = le32(b + opt + 112);
    m->exp_rva = le32(b + opt + 96);
    m->exp_size = le32(b + opt + 100);
    /* imports: HLE DLLs bind to host thunks, other DLLs are loaded from the program directory */
    if (imp_rva) {
        uint32_t d;
        for (d = base + imp_rva; ; d += 20) {
            uint32_t ilt = x86_rd32(c, d), nm = x86_rd32(c, d + 12), iat = x86_rd32(c, d + 16), k;
            char dll[64];
            qmod *dm = NULL;
            if (!nm && !iat) break;
            qw_rd_str(w, base + nm, dll, sizeof(dll));
            if (!qw_is_hle_dll(dll)) {
                dm = pe_load_dll(w, dll);
                m = &w->mod[idx];                       /* (array is static; keep the pointer obvious) */
                if (!dm) {
                    qw_log(w, "%s imports %s: not found next to the program", name, dll);
                    if (!strncasecmp(dll, "d3dx", 4) || !strncasecmp(dll, "xinput", 6) || !strncasecmp(dll, "d3dcompiler", 11) || !strncasecmp(dll, "x3daudio", 8) || !strncasecmp(dll, "xapofx", 6))
                        qw_log(w, "%s belongs to Microsoft's DirectX runtime: run tools/armithlon/install-directx.sh on the Mac", dll);
                }
            }
            if (!ilt) ilt = iat;
            for (k = 0; ; k++) {
                uint32_t e = x86_rd32(c, base + ilt + k * 4), addr = 0;
                char fn[96];
                if (!e) break;
                if (e & 0x80000000u) snprintf(fn, sizeof(fn), "#%u", e & 0xFFFF);
                else qw_rd_str(w, base + e + 2, fn, sizeof(fn));
                if (dm) addr = pe_export(w, dm, (e & 0x80000000u) ? NULL : fn, e & 0xFFFF);
                if (!addr) addr = qw_thunk(w, dll, fn);
                x86_wr32(c, base + iat + k * 4, addr);
            }
        }
    }
    if (is_dll && w->ninit < QW_MAXMOD) w->init_order[w->ninit++] = idx;   /* after its own imports */
    /* read-only protection is not applied: many old programs write to .rdata-adjacent data */
    return &w->mod[idx];
}

int pe_load(qwin *w, const uint8_t *b, size_t n)
{
    char name[64];
    qmod *m;
    lower_base(name, w->exe_path, sizeof(name));
    m = pe_map(w, b, n, name, 0);
    if (!m) return 0;
    w->image_base = m->base;
    w->image_size = m->size;
    w->entry = m->entry;
    w->res_rva = m->res_rva;
    return 1;
}

/* ---------------------------------------------------------------- resources */
static int name_match(qwin *w, uint32_t dir_base, uint32_t ent, uint32_t name)
{
    x86cpu *c = &w->cpu;
    uint32_t id = x86_rd32(c, ent);
    if (name < 0x10000u) return !(id & 0x80000000u) && (id & 0xFFFF) == name;
    if (!(id & 0x80000000u)) return 0;
    {
        uint32_t s = dir_base + (id & 0x7FFFFFFFu);
        uint16_t len = x86_rd16(c, s), i;
        wchar16 want[64];
        int wl = qw_rd_wstr(w, name, want, 64);
        if (wl != len) return 0;
        for (i = 0; i < len; i++) {
            wchar16 a = x86_rd16(c, s + 2 + i * 2), b = want[i];
            if (a >= 'a' && a <= 'z') a = (wchar16)(a - 32);
            if (b >= 'a' && b <= 'z') b = (wchar16)(b - 32);
            if (a != b) return 0;
        }
        return 1;
    }
}

static uint32_t find_entry(qwin *w, uint32_t root, uint32_t dir, uint32_t name)
{
    x86cpu *c = &w->cpu;
    uint32_t nn = x86_rd16(c, dir + 12), ni = x86_rd16(c, dir + 14), i;
    for (i = 0; i < nn + ni; i++) {
        uint32_t e = dir + 16 + i * 8;
        if (name_match(w, root, e, name)) return x86_rd32(c, e + 4);
    }
    return 0xFFFFFFFFu;
}

int res_find(qwin *w, uint32_t type, uint32_t name, uint32_t *data, uint32_t *size)
{
    return res_find_in(w, w->image_base, type, name, data, size);
}

/* hmod: module handle (base); 0 or unknown -> the program itself */
int res_find_in(qwin *w, uint32_t hmod, uint32_t type, uint32_t name, uint32_t *data, uint32_t *size)
{
    x86cpu *c = &w->cpu;
    uint32_t root, t, nmd, ld, nn, i, best = 0xFFFFFFFFu, pick, mbase = w->image_base, mres = w->res_rva;
    int bestscore = -1;
    qmod *m = hmod ? pe_find_module(w, hmod) : NULL;
    if (m) { mbase = m->base; mres = m->res_rva; }
    if (!mres) return 0;
    root = mbase + mres;
    t = find_entry(w, root, root, type);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    nmd = find_entry(w, root, root + (t & 0x7FFFFFFFu), name);
    if (nmd == 0xFFFFFFFFu || !(nmd & 0x80000000u)) return 0;
    ld = root + (nmd & 0x7FFFFFFFu);
    nn = (uint32_t)x86_rd16(c, ld + 12) + x86_rd16(c, ld + 14);
    for (i = 0; i < nn; i++) {     /* language: exact > same primary > English > neutral > first */
        uint32_t e = ld + 16 + i * 8, lang = x86_rd32(c, e) & 0xFFFF;
        int sc = lang == w->lang ? 5 : (lang & 0x3FF) == (w->lang & 0x3FFu) ? 4 : lang == 0x409 ? 3 : (lang & 0x3FF) == 9 ? 2 : lang == 0 ? 1 : 0;
        if (sc > bestscore) { bestscore = sc; best = x86_rd32(c, e + 4); }
    }
    if (best == 0xFFFFFFFFu || (best & 0x80000000u)) return 0;
    pick = root + best;
    *data = mbase + x86_rd32(c, pick);
    *size = x86_rd32(c, pick + 4);
    return 1;
}

int res_find_id(qwin *w, int type, int id, uint32_t *data, uint32_t *size)
{
    return res_find(w, (uint32_t)type, (uint32_t)id, data, size);
}
