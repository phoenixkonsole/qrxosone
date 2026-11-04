/* QRX86 host JIT: x86 -> AArch64 blocks for the door. Included by qrx86_door.c after
 * qrx86core.c, so it reaches the interpreter's internals.
 *
 * The interpreter stays the reference. Anything without a native translation, and
 * any memory access that misses the fast tables, runs there one instruction at a
 * time. Native code never changes guest state before its last memory access, so an
 * interpreter re-run of the instruction, and any fault it raises, sees the state at
 * the start of the instruction.
 *
 * Host registers:
 *   w19-w26  EAX ECX EDX EBX ESP EBP ESI EDI
 *   x27      x86cpu
 *   x28      read table:  1M entries, host base of a page minus its x86 address, 0 = slow
 *            8 MiB above it the write table: the same, also 0 for read-only pages and
 *            translated code
 *   x29      a real frame pointer, so that profilers and debuggers can walk the stack
 *   w0-w5, w9-w12, w16, w17  temporaries
 *
 * Flags: a native flag setter leaves the matching NZCV behind. A later Jcc, SETcc or
 * CMOV in the block uses it directly. The interpreter's lazy state (lf_*) is written
 * only when a later reader may need it, found by a liveness pass over the block and
 * a short look into its successors. */
#if defined(__aarch64__) || defined(__arm64__)
#define QRX86_HOST_JIT_ACTIVE 1
#include <sys/mman.h>
#include <pthread.h>
#include <stddef.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#define XJ_ARENA (128u * 1024u * 1024u)
#define XJ_HASH_BITS 16u
#define XJ_HASH (1u << XJ_HASH_BITS)
#define XJ_IBT_BITS 16u
#define XJ_MAX_INS 64
#define XJ_PAGES (1u << 20)
#define XJ_KILLS_VOLATILE 16u

#define XR(i) (19 + (i))
#define XCPU 27
#define XRT 28
#define WZR 31

typedef struct XBlock {
    uint32_t eip, n;
    uint32_t *code;
    struct XBlock *hash_next, *page_next;
} XBlock;
typedef struct { uint64_t eip; void *code; } XIbt;
typedef struct { uint32_t *site, *stub; } XLink;

static struct {
    bool on, failed;
    uint32_t *arena, *next, *end;
    uint32_t *enter_code, *epi_link, *epi_plain, *interp_thunk, *cond_thunk, *ibt_stub, *arena_blocks;
    XBlock *hash[XJ_HASH];
    XBlock **page_blocks;            /* per x86 page */
    uint8_t *page_kills;             /* per x86 page */
    uint32_t *ram_virt;              /* per RAM page: x86 page + 1 holding code */
    uintptr_t *rt, *wt;
    XIbt *ibt;
    XLink *links;
    size_t nlinks, caplinks;
    QRXFullCPU *cpu;
    uint64_t blocks, flushes, kills, interp_ins, volatile_pages, enters;
} xj;

typedef void (*xj_enter_fn)(x86cpu *c, void *code, uintptr_t *rt);

static inline void xj_w_enable(bool on) {
#if defined(__APPLE__)
    pthread_jit_write_protect_np(!on);
#else
    (void)on;
#endif
}
static inline void xj_sync(void *p, size_t n) {
#if defined(__APPLE__)
    sys_icache_invalidate(p, n);
#else
    __builtin___clear_cache((char *)p, (char *)p + n);
#endif
}

/* ---- instruction forms ---- */
enum {
    K_FALL, K_FALL_END, K_ALU, K_MOV, K_MOVX, K_LEA, K_PUSH, K_POP, K_INCDEC, K_NOT, K_NEG, K_SHIFT, K_IMUL,
    K_JCC, K_JMP, K_CALL, K_RET, K_JMPI, K_CALLI, K_SETCC, K_CMOV, K_CDQ, K_CWDE, K_XCHG, K_LEAVE, K_NOP,
    K_FLD_M, K_FSTP_M, K_FARITH_M, K_FARITH_ST
};
typedef struct {
    int base, index;
    unsigned scale;
    uint32_t disp;
    int seg;                         /* 0 flat, otherwise X86_FS or X86_GS */
} XMem;
typedef struct {
    uint32_t eip, next;
    uint8_t kind, sz, op, cc;
    uint8_t reg;                     /* register operand */
    uint8_t rm;                      /* register r/m when !has_mem */
    bool has_mem, has_imm, rm_dst;   /* rm_dst: the r/m operand is written */
    bool writes;                     /* ALU: result is stored (not CMP or TEST) */
    bool imm_cl;                     /* shift count in CL */
    bool need_lf;
    bool native_cc;
    uint8_t src_sz;
    bool src_signed;
    uint32_t imm;
    uint32_t target;
    XMem m;
} XIns;

static bool xj_fetch_ok(uint32_t eip, const uint8_t **out, uint32_t *room) {
    x86page *t = host.dir[eip >> 22];
    if (!t) return false;
    t += (eip >> 12) & 1023u;
    if (!t->mem) return false;
    *out = t->mem + (eip & X86_PAGE_MASK);
    *room = X86_PAGE_SIZE - (eip & X86_PAGE_MASK);
    return true;
}

/* Reads the r/m operand. Returns false when it cannot be decoded natively. */
static bool xj_modrm(const uint8_t *q, uint32_t room, uint32_t *pos, XIns *x, int seg) {
    uint8_t b;
    int mod, rm;
    if (*pos >= room) return false;
    b = q[(*pos)++];
    mod = b >> 6; rm = b & 7;
    x->reg = (b >> 3) & 7;
    x->rm = (uint8_t)rm;
    x->has_mem = mod != 3;
    x->m.base = -1; x->m.index = -1; x->m.scale = 0; x->m.disp = 0; x->m.seg = seg;
    if (mod == 3) return true;
    if (rm == 4) {
        uint8_t sib;
        int bs, ix;
        if (*pos >= room) return false;
        sib = q[(*pos)++];
        x->m.scale = sib >> 6;
        ix = (sib >> 3) & 7; bs = sib & 7;
        if (ix != 4) x->m.index = ix;
        if (bs == 5 && mod == 0) {
            if (*pos + 4 > room) return false;
            x->m.disp = ld32(q + *pos); *pos += 4;
        } else x->m.base = bs;
    } else if (rm == 5 && mod == 0) {
        if (*pos + 4 > room) return false;
        x->m.disp = ld32(q + *pos); *pos += 4;
    } else x->m.base = rm;
    if (mod == 1) {
        if (*pos + 1 > room) return false;
        x->m.disp += (uint32_t)(int32_t)(int8_t)q[(*pos)++];
    } else if (mod == 2) {
        if (*pos + 4 > room) return false;
        x->m.disp += ld32(q + *pos); *pos += 4;
    }
    return true;
}
static bool xj_imm(const uint8_t *q, uint32_t room, uint32_t *pos, int bytes, bool sign, uint32_t *out) {
    if (*pos + (uint32_t)bytes > room) return false;
    if (bytes == 1) *out = sign ? (uint32_t)(int32_t)(int8_t)q[*pos] : q[*pos];
    else if (bytes == 2) *out = sign ? (uint32_t)(int32_t)(int16_t)ld16(q + *pos) : ld16(q + *pos);
    else *out = ld32(q + *pos);
    *pos += (uint32_t)bytes;
    return true;
}

/* Decodes one instruction. Returns false when it crosses the page or the page is
 * not mapped: the caller ends the block in front of it. */
static bool xj_decode(uint32_t eip, XIns *x) {
    const uint8_t *q;
    uint32_t room, pos = 0;
    int osz = 32, seg = 0, rep = 0;
    uint8_t b;
    memset(x, 0, sizeof *x);
    x->eip = eip;
    if (!xj_fetch_ok(eip, &q, &room)) return false;
    x->kind = K_FALL_END;
    for (;;) {
        if (pos >= room) return false;
        b = q[pos];
        if (b == 0x66) osz = 16;
        else if (b == 0xF0 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E) { /* flat segments */ }
        else if (b == 0x64) seg = X86_FS;
        else if (b == 0x65) seg = X86_GS;
        else if (b == 0xF2 || b == 0xF3) rep = b;
        else if (b == 0x67) return true;     /* 16-bit addressing: interpreter */
        else break;
        pos++;
    }
    pos++;
    x->sz = (uint8_t)osz;
    x->op = 0;

    if (b < 0x40 && (b & 7) < 6) {           /* ALU forms */
        int op = (b >> 3) & 7, form = b & 7;
        if (b == 0x0F) goto two_byte;
        if (rep) return true;
        x->kind = K_ALU;
        x->op = (uint8_t)op;
        x->writes = op != 7;
        x->sz = (form & 1) ? (uint8_t)osz : 8;
        if (form < 4) {
            if (!xj_modrm(q, room, &pos, x, seg)) return false;
            x->rm_dst = form < 2;
        } else {
            x->reg = 0; x->rm = 0; x->rm_dst = true;
            x->has_imm = true;
            if (!xj_imm(q, room, &pos, x->sz == 8 ? 1 : x->sz == 16 ? 2 : 4, false, &x->imm)) return false;
        }
        if (op == 2 || op == 3) x->kind = K_FALL;
        goto done;
    }
    if (b == 0x0F) goto two_byte;
    if (b >= 0x40 && b <= 0x4F) {
        x->kind = K_INCDEC; x->op = b >= 0x48;
        x->rm = b & 7; x->rm_dst = true;
        goto done;
    }
    if (b >= 0x50 && b <= 0x57) { x->kind = K_PUSH; x->rm = b & 7; if (osz != 32) x->kind = K_FALL; goto done; }
    if (b >= 0x58 && b <= 0x5F) { x->kind = K_POP; x->rm = b & 7; if (osz != 32 || (b & 7) == X86_ESP) x->kind = K_FALL; goto done; }
    if (b >= 0x70 && b <= 0x7F) {
        uint32_t d;
        if (!xj_imm(q, room, &pos, 1, true, &d)) return false;
        x->kind = K_JCC; x->cc = b & 15; x->target = eip + pos + d;
        goto done;
    }
    if (b >= 0xB0 && b <= 0xB7) {
        x->kind = K_MOV; x->sz = 8; x->has_imm = true; x->rm = b & 7; x->rm_dst = true;
        if (!xj_imm(q, room, &pos, 1, false, &x->imm)) return false;
        goto done;
    }
    if (b >= 0xB8 && b <= 0xBF) {
        x->kind = K_MOV; x->has_imm = true; x->rm = b & 7; x->rm_dst = true;
        if (!xj_imm(q, room, &pos, osz == 16 ? 2 : 4, false, &x->imm)) return false;
        goto done;
    }
    if (b >= 0x91 && b <= 0x97) { x->kind = K_XCHG; x->reg = 0; x->rm = b & 7; goto done; }
    if (b >= 0xD8 && b <= 0xDF) {
        XIns y = *x;
        if (!xj_modrm(q, room, &pos, &y, seg)) return false;
        y.kind = K_FALL;
        y.next = eip + pos;
        /* The memory forms that dominate rasterisers run natively. */
        if (y.has_mem && !seg) {
            if (b == 0xD9 && y.reg == 0) { y.kind = K_FLD_M; y.src_sz = 32; }
            else if (b == 0xDD && y.reg == 0) { y.kind = K_FLD_M; y.src_sz = 64; }
            else if (b == 0xD9 && (y.reg == 2 || y.reg == 3)) { y.kind = K_FSTP_M; y.src_sz = 32; y.op = y.reg == 3; }
            else if (b == 0xDD && (y.reg == 2 || y.reg == 3)) { y.kind = K_FSTP_M; y.src_sz = 64; y.op = y.reg == 3; }
            else if ((b == 0xD8 || b == 0xDC) && y.reg != 2 && y.reg != 3) {
                y.kind = K_FARITH_M; y.src_sz = b == 0xD8 ? 32 : 64; y.op = y.reg;
            }
        } else if (!y.has_mem) {
            /* D8 ST0,STi; DC STi,ST0; DE STi,ST0 and pop: add, mul, sub, subr, div, divr */
            if (b == 0xD8 && y.reg != 2 && y.reg != 3) { y.kind = K_FARITH_ST; y.op = y.reg; y.cc = 0; }
            else if ((b == 0xDC || b == 0xDE) && y.reg != 2 && y.reg != 3) {
                static const uint8_t map[8] = { 0, 1, 2, 3, 5, 4, 7, 6 };
                y.kind = K_FARITH_ST; y.op = map[y.reg]; y.cc = b == 0xDE ? 2 : 1;
            }
        }
        *x = y;
        return true;
    }

    switch (b) {
    case 0x68: x->kind = K_PUSH; x->has_imm = true; if (osz != 32) { x->kind = K_FALL_END; return true; } if (!xj_imm(q, room, &pos, 4, false, &x->imm)) return false; break;
    case 0x6A: x->kind = K_PUSH; x->has_imm = true; if (osz != 32) { x->kind = K_FALL_END; return true; } if (!xj_imm(q, room, &pos, 1, true, &x->imm)) return false; break;
    case 0x69: case 0x6B:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_IMUL; x->has_imm = true;
        if (!xj_imm(q, room, &pos, b == 0x6B ? 1 : osz == 16 ? 2 : 4, true, &x->imm)) return false;
        if (osz != 32) x->kind = K_FALL;
        break;
    case 0x80: case 0x81: case 0x83:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_ALU; x->op = x->reg; x->writes = x->reg != 7; x->rm_dst = true; x->has_imm = true;
        x->sz = b == 0x80 ? 8 : (uint8_t)osz;
        if (!xj_imm(q, room, &pos, b == 0x81 ? (osz == 16 ? 2 : 4) : 1, b == 0x83, &x->imm)) return false;
        if (x->op == 2 || x->op == 3) x->kind = K_FALL;
        break;
    case 0x84: case 0x85:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_ALU; x->op = 4; x->writes = false; x->rm_dst = true;
        x->sz = b == 0x84 ? 8 : (uint8_t)osz;
        break;
    case 0xA8: case 0xA9:
        x->kind = K_ALU; x->op = 4; x->writes = false; x->rm_dst = true; x->has_imm = true; x->rm = 0;
        x->sz = b == 0xA8 ? 8 : (uint8_t)osz;
        if (!xj_imm(q, room, &pos, x->sz == 8 ? 1 : x->sz == 16 ? 2 : 4, false, &x->imm)) return false;
        break;
    case 0x86: case 0x87:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_XCHG; x->sz = b == 0x86 ? 8 : (uint8_t)osz;
        break;
    case 0x88: case 0x89: case 0x8A: case 0x8B:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_MOV; x->rm_dst = b < 0x8A;
        x->sz = (b & 1) ? (uint8_t)osz : 8;
        break;
    case 0x8D:
        if (!xj_modrm(q, room, &pos, x, 0)) return false;
        if (!x->has_mem || osz != 32) { x->kind = K_FALL_END; return true; }
        x->kind = K_LEA;
        break;
    case 0x90: x->kind = rep ? K_FALL : K_NOP; break;
    case 0x98: x->kind = K_CWDE; break;
    case 0x99: x->kind = K_CDQ; break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        x->kind = K_MOV; x->has_mem = true; x->m.base = -1; x->m.index = -1; x->m.seg = seg;
        x->reg = 0; x->rm_dst = b >= 0xA2;
        x->sz = (b & 1) ? (uint8_t)osz : 8;
        if (!xj_imm(q, room, &pos, 4, false, &x->m.disp)) return false;
        break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        x->kind = K_FALL;
        break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_SHIFT; x->op = x->reg; x->rm_dst = true;
        x->sz = (b & 1) ? (uint8_t)osz : 8;
        if (b <= 0xC1) { if (!xj_imm(q, room, &pos, 1, false, &x->imm)) return false; x->imm &= 31; x->has_imm = true; }
        else if (b <= 0xD1) { x->imm = 1; x->has_imm = true; }
        else x->imm_cl = true;
        /* RCL, RCR and undefined /6, and rotates of small operands */
        if (x->op == 2 || x->op == 3 || x->op == 6 || (x->op < 2 && x->sz != 32)) x->kind = K_FALL;
        break;
    case 0xC2: x->kind = K_RET; x->has_imm = true; if (!xj_imm(q, room, &pos, 2, false, &x->imm)) return false; if (osz != 32) x->kind = K_FALL_END; break;
    case 0xC3: x->kind = K_RET; if (osz != 32) x->kind = K_FALL_END; break;
    case 0xC6: case 0xC7:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        if (x->reg != 0) { x->kind = K_FALL_END; return true; }
        x->kind = K_MOV; x->rm_dst = true; x->has_imm = true;
        x->sz = b == 0xC6 ? 8 : (uint8_t)osz;
        if (!xj_imm(q, room, &pos, x->sz == 8 ? 1 : x->sz == 16 ? 2 : 4, false, &x->imm)) return false;
        break;
    case 0xC9: x->kind = osz == 32 ? K_LEAVE : K_FALL; break;
    case 0xE8: { uint32_t d; if (osz != 32 || !xj_imm(q, room, &pos, 4, false, &d)) { x->kind = K_FALL_END; return osz != 32; } x->kind = K_CALL; x->target = eip + pos + d; break; }
    case 0xE9: { uint32_t d; if (osz != 32 || !xj_imm(q, room, &pos, 4, false, &d)) { x->kind = K_FALL_END; return osz != 32; } x->kind = K_JMP; x->target = eip + pos + d; break; }
    case 0xEB: { uint32_t d; if (!xj_imm(q, room, &pos, 1, true, &d)) return false; x->kind = K_JMP; x->target = eip + pos + d; if (osz != 32) x->kind = K_FALL_END; break; }
    case 0xF6: case 0xF7:
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->sz = b == 0xF6 ? 8 : (uint8_t)osz;
        switch (x->reg) {
        case 0: case 1:
            x->kind = K_ALU; x->op = 4; x->writes = false; x->rm_dst = true; x->has_imm = true;
            if (!xj_imm(q, room, &pos, x->sz == 8 ? 1 : x->sz == 16 ? 2 : 4, false, &x->imm)) return false;
            break;
        case 2: x->kind = K_NOT; x->rm_dst = true; break;
        case 3: x->kind = K_NEG; x->rm_dst = true; break;
        default: x->kind = K_FALL; break;     /* MUL, IMUL, DIV, IDIV */
        }
        break;
    case 0xFE: case 0xFF: {
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->sz = b == 0xFE ? 8 : (uint8_t)osz;
        switch (x->reg) {
        case 0: case 1: x->kind = K_INCDEC; x->op = x->reg; x->rm_dst = true; break;
        case 2: if (b == 0xFE || osz != 32) { x->kind = K_FALL_END; return true; } x->kind = K_CALLI; break;
        case 4: if (b == 0xFE || osz != 32) { x->kind = K_FALL_END; return true; } x->kind = K_JMPI; break;
        case 6: if (b == 0xFE || osz != 32) { x->kind = K_FALL_END; return true; } x->kind = K_PUSH; if (!x->has_mem) x->kind = K_FALL; break;
        default: x->kind = K_FALL_END; return true;
        }
        break;
    }
    default:
        x->kind = K_FALL_END;
        return true;
    }
done:
    x->next = eip + pos;
    if (rep && x->kind != K_FALL) { x->kind = K_FALL_END; }
    return true;

two_byte:
    if (pos >= room) return false;
    b = q[pos++];
    if (b >= 0x80 && b <= 0x8F) {
        uint32_t d;
        if (!xj_imm(q, room, &pos, 4, false, &d)) return false;
        if (osz != 32) { x->kind = K_FALL_END; return true; }
        x->kind = K_JCC; x->cc = b & 15; x->target = eip + pos + d;
        x->next = eip + pos;
        return true;
    }
    if (b >= 0x90 && b <= 0x9F) {
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_SETCC; x->cc = b & 15; x->sz = 8; x->rm_dst = true;
        x->next = eip + pos;
        return true;
    }
    if (b >= 0x40 && b <= 0x4F) {
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = osz == 32 ? K_CMOV : K_FALL; x->cc = b & 15;
        x->next = eip + pos;
        return true;
    }
    if (b == 0xB6 || b == 0xB7 || b == 0xBE || b == 0xBF) {
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = K_MOVX;
        x->src_sz = (b & 1) ? 16 : 8;
        x->src_signed = b >= 0xBE;
        x->next = eip + pos;
        return true;
    }
    if (b == 0xAF) {
        if (!xj_modrm(q, room, &pos, x, seg)) return false;
        x->kind = osz == 32 ? K_IMUL : K_FALL;
        x->next = eip + pos;
        return true;
    }
    x->kind = K_FALL_END;
    return true;
}

/* ---- flag use ---- */
#define L_CF 1u
#define L_OTHER 2u
#define L_ALL 3u
static inline unsigned xj_cc_reads(unsigned cc) {
    switch (cc >> 1) { case 1: return L_CF; case 3: return L_ALL; default: return L_OTHER; }
}
/* True for an instruction that sets every flag whatever its operands are. */
static inline bool xj_kills_all(const XIns *x) {
    return x->kind == K_ALU || (x->kind == K_SHIFT && x->op >= 4 && !x->imm_cl && (x->imm & 31));
}
/* Which flags may be read by the code at addr, within a short look ahead. */
static unsigned xj_peek_live(uint32_t addr) {
    unsigned mask = L_ALL;
    for (int i = 0; i < 12; i++) {
        XIns y;
        if (!xj_decode(addr, &y)) return mask;
        switch (y.kind) {
        case K_ALU: return 0;
        case K_SHIFT:
            if (xj_kills_all(&y)) return 0;
            return mask;
        case K_INCDEC: mask &= ~L_OTHER; addr = y.next; break;
        case K_MOV: case K_MOVX: case K_LEA: case K_PUSH: case K_POP: case K_NOT: case K_NOP: case K_CDQ: case K_CWDE:
        case K_XCHG: case K_LEAVE: case K_FLD_M: case K_FSTP_M: case K_FARITH_M: case K_FARITH_ST:
            addr = y.next;
            break;
        case K_JMP: addr = y.target; break;
        case K_CALL: addr = y.target; break;
        default: return mask;
        }
    }
    return mask;
}
enum { NZ_NONE, NZ_SUB, NZ_ADD, NZ_LOGIC, NZ_INCDEC };
enum { C_EQ, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };
static int xj_a64_cc(int nz, unsigned cc) {
    static const int8_t sub[16] = { C_VS, C_VC, C_LO, C_HS, C_EQ, C_NE, C_LS, C_HI, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t add[16] = { C_VS, C_VC, C_HS, C_LO, C_EQ, C_NE, -1, -1, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t logic[16] = { -1, -1, -1, -1, C_EQ, C_NE, C_EQ, C_NE, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t incdec[16] = { C_VS, C_VC, -1, -1, C_EQ, C_NE, -1, -1, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    switch (nz) {
    case NZ_SUB: return sub[cc & 15];
    case NZ_ADD: return add[cc & 15];
    case NZ_LOGIC: return logic[cc & 15];
    case NZ_INCDEC: return incdec[cc & 15];
    default: return -1;
    }
}

/* Decides for each instruction which flags machinery it uses. An INC or DEC whose CF
 * is read later, and rotates and IMUL with live flags, go to the interpreter, which
 * changes what is known around them: so this runs until nothing changes. */
static void xj_analyse(XIns *ins, int n, unsigned end_live) {
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        int nz = NZ_NONE;
        for (int i = 0; i < n; i++) {
            XIns *x = &ins[i];
            x->native_cc = false;
            switch (x->kind) {
            case K_ALU: nz = (x->op == 5 || x->op == 7) ? NZ_SUB : x->op == 0 ? NZ_ADD : NZ_LOGIC; break;
            case K_INCDEC: nz = NZ_INCDEC; break;
            case K_JCC: case K_SETCC: case K_CMOV:
                /* An interpreter re-run of a memory form reads the lazy flags. */
                x->native_cc = !x->has_mem && xj_a64_cc(nz, x->cc) >= 0;
                if (!x->native_cc) nz = NZ_NONE;
                break;
            case K_SHIFT: case K_NEG: case K_IMUL: case K_FALL: case K_FALL_END: nz = NZ_NONE; break;
            default: break;
            }
        }
        unsigned live = end_live;
        for (int i = n - 1; i >= 0; i--) {
            XIns *x = &ins[i];
            x->need_lf = false;
            switch (x->kind) {
            case K_ALU:
                x->need_lf = live != 0;
                live = 0;
                break;
            case K_INCDEC:
                if (live & L_CF) { x->kind = K_FALL; changed = true; live = L_ALL; break; }
                x->need_lf = live != 0;
                live = 0;
                break;
            case K_SHIFT:
                if (x->op < 4) {             /* rotate: CF and OF differ, the lazy state stays */
                    if (live) { x->kind = K_FALL; changed = true; live = L_ALL; }
                    break;
                }
                if (xj_kills_all(x)) { x->need_lf = live != 0; live = 0; }
                else if (x->has_imm) { /* count 0: no effect on flags */ }
                else x->need_lf = live != 0; /* live stays: count 0 keeps the old flags */
                break;
            case K_NEG: case K_IMUL:
                if (live) { x->kind = K_FALL; changed = true; live = L_ALL; }
                break;
            case K_JCC: case K_SETCC: case K_CMOV:
                if (!x->native_cc) live |= xj_cc_reads(x->cc);
                break;
            case K_FALL: case K_FALL_END:
                live = L_ALL;
                break;
            default:
                break;
            }
        }
        if (!changed) return;
    }
}

/* ---- AArch64 emitter ---- */
typedef struct { uint32_t *p; } XE;
static inline void E(XE *e, uint32_t v) { *e->p++ = v; }
static void mov_w(XE *e, int rd, uint32_t v) {
    if (!(v >> 16)) { E(e, 0x52800000u | (v << 5) | (uint32_t)rd); return; }
    if (!(v & 0xFFFFu)) { E(e, 0x52A00000u | ((v >> 16) << 5) | (uint32_t)rd); return; }
    if ((v >> 16) == 0xFFFFu) { E(e, 0x12800000u | ((~v & 0xFFFFu) << 5) | (uint32_t)rd); return; }
    E(e, 0x52800000u | ((v & 0xFFFFu) << 5) | (uint32_t)rd);
    E(e, 0x72A00000u | ((v >> 16) << 5) | (uint32_t)rd);
}
static void mov_x(XE *e, int rd, uint64_t v) {
    E(e, 0xD2800000u | (uint32_t)((v & 0xFFFFu) << 5) | (uint32_t)rd);
    for (unsigned s = 1; s < 4; s++) {
        uint32_t part = (uint32_t)(v >> (s * 16)) & 0xFFFFu;
        if (part) E(e, 0xF2800000u | (s << 21) | (part << 5) | (uint32_t)rd);
    }
}
#define R3(base, d, n, m) ((uint32_t)(base) | ((uint32_t)(m) << 16) | ((uint32_t)(n) << 5) | (uint32_t)(d))
static inline void movr(XE *e, int d, int m) { if (d != m) E(e, R3(0x2A0003E0u, d, 0, m)); }
static inline void ubfm(XE *e, int d, int n, unsigned immr, unsigned imms) { E(e, 0x53000000u | (immr << 16) | (imms << 10) | ((uint32_t)n << 5) | (uint32_t)d); }
static inline void sbfm(XE *e, int d, int n, unsigned immr, unsigned imms) { E(e, 0x13000000u | (immr << 16) | (imms << 10) | ((uint32_t)n << 5) | (uint32_t)d); }
static inline void lsl_i(XE *e, int d, int n, unsigned s) { ubfm(e, d, n, (32u - s) & 31u, 31u - s); }
static inline void lsr_i(XE *e, int d, int n, unsigned s) { ubfm(e, d, n, s, 31u); }
static inline void asr_i(XE *e, int d, int n, unsigned s) { sbfm(e, d, n, s, 31u); }
static inline void bfi(XE *e, int d, int n, unsigned lsb, unsigned w) { E(e, 0x33000000u | (((32u - lsb) & 31u) << 16) | ((w - 1u) << 10) | ((uint32_t)n << 5) | (uint32_t)d); }
static inline void add_i(XE *e, int d, int n, uint32_t i) { E(e, 0x11000000u | (i << 10) | ((uint32_t)n << 5) | (uint32_t)d); }
static inline void sub_i(XE *e, int d, int n, uint32_t i) { E(e, 0x51000000u | (i << 10) | ((uint32_t)n << 5) | (uint32_t)d); }
static inline void ldr_cpu(XE *e, int rt, size_t off) { E(e, 0xB9400000u | ((uint32_t)(off / 4) << 10) | (XCPU << 5) | (uint32_t)rt); }
static inline void str_cpu(XE *e, int rt, size_t off) { E(e, 0xB9000000u | ((uint32_t)(off / 4) << 10) | (XCPU << 5) | (uint32_t)rt); }
static inline void stp_cpu(XE *e, int a, int b, size_t off) { E(e, 0x29000000u | ((uint32_t)(off / 4) << 15) | ((uint32_t)b << 10) | (XCPU << 5) | (uint32_t)a); }
static inline void ldp_cpu(XE *e, int a, int b, size_t off) { E(e, 0x29400000u | ((uint32_t)(off / 4) << 15) | ((uint32_t)b << 10) | (XCPU << 5) | (uint32_t)a); }
static inline void b_to(XE *e, const uint32_t *t) { E(e, 0x14000000u | ((uint32_t)(t - e->p) & 0x03FFFFFFu)); }
static inline void bl_to(XE *e, const uint32_t *t) { E(e, 0x94000000u | ((uint32_t)(t - e->p) & 0x03FFFFFFu)); }
static inline void bcond_to(XE *e, int c, const uint32_t *t) { E(e, 0x54000000u | (((uint32_t)(t - e->p) & 0x7FFFFu) << 5) | (uint32_t)c); }

/* Forward branches inside a block */
typedef struct { uint32_t *site; int ins; int form; } XFix;   /* form: 0 cbz/cbnz/b.cond, 1 tbz/tbnz, 2 b */
typedef struct {
    XE e;
    XIns *ins;
    int n, cur;
    XFix fix[XJ_MAX_INS * 16];
    int nfix;
} XT;
static void fix_add(XT *t, uint32_t inst, int form) {
    t->fix[t->nfix].site = t->e.p;
    t->fix[t->nfix].ins = t->cur;
    t->fix[t->nfix].form = form;
    t->nfix++;
    E(&t->e, inst);
}
static inline void cbz_stub_x(XT *t, int r) { fix_add(t, 0xB4000000u | (uint32_t)r, 0); }
static inline void tbnz_stub(XT *t, int r, unsigned bit) { fix_add(t, 0x37000000u | (bit << 19) | (uint32_t)r, 1); }

/* Effective address. Returns the host register holding it. */
static int ea(XT *t, const XMem *m) {
    XE *e = &t->e;
    int r = -1;
    if (m->base >= 0 && m->index < 0 && !m->disp && !m->seg) return XR(m->base);
    if (m->base >= 0 && m->index >= 0) {
        E(e, R3(0x0B000000u, 0, XR(m->base), XR(m->index)) | (m->scale << 10));
        r = 0;
    } else if (m->index >= 0) {
        lsl_i(e, 0, XR(m->index), m->scale);
        r = 0;
    } else if (m->base >= 0) r = XR(m->base);
    if (m->disp) {
        uint32_t d = m->disp, neg = 0u - d;
        if (r < 0) { mov_w(e, 0, d); r = 0; }
        else if (d < 4096) { add_i(e, 0, r, d); r = 0; }
        else if (neg < 4096) { sub_i(e, 0, r, neg); r = 0; }
        else { mov_w(e, 16, d); E(e, R3(0x0B000000u, 0, r, 16)); r = 0; }
    } else if (r < 0) { mov_w(e, 0, 0); r = 0; }
    if (m->seg) {
        ldr_cpu(e, 16, offsetof(x86cpu, sbase) + (size_t)m->seg * 4);
        E(e, R3(0x0B000000u, 0, r, 16));
        r = 0;
    }
    return r;
}
/* Leaves the host page base in x16 and goes to the interpreter on any miss. */
static void mem_check(XT *t, int addr, bool write, unsigned bytes) {
    XE *e = &t->e;
    lsr_i(e, 16, addr, 12);
    if (write) {
        E(e, 0x8B000000u | (16u << 16) | (3u << 10) | (XRT << 5) | 16u);   /* add x16,x28,x16,lsl #3 */
        E(e, 0x91400000u | (0x800u << 10) | (16u << 5) | 16u);             /* add x16,x16,#8M */
        E(e, 0xF9400000u | (16u << 5) | 16u);                              /* ldr x16,[x16] */
    } else {
        E(e, R3(0xF8607800u, 16, XRT, 16));        /* ldr x16,[x28,x16,lsl #3] */
    }
    cbz_stub_x(t, 16);
    if (bytes > 1) {
        add_i(e, 17, addr, bytes - 1u);
        E(e, R3(0x4A000000u, 17, 17, addr));       /* eor w17,w17,addr */
        tbnz_stub(t, 17, 12);
    }
}
static void load(XT *t, int rd, int addr, unsigned sz, bool sign) {
    XE *e = &t->e;
    mem_check(t, addr, false, sz / 8u);
    uint32_t op = sz == 32 ? 0xB8604800u : sz == 16 ? (sign ? 0x78E04800u : 0x78604800u) : (sign ? 0x38E04800u : 0x38604800u);
    E(e, R3(op, rd, 16, addr));
}
static void store(XT *t, int rs, int addr, unsigned sz) {
    XE *e = &t->e;
    mem_check(t, addr, true, sz / 8u);
    uint32_t op = sz == 32 ? 0xB8204800u : sz == 16 ? 0x78204800u : 0x38204800u;
    E(e, R3(op, rs, 16, addr));
}
/* A register operand as a host register, small sizes zero extended into tmp. */
static int reg_read(XE *e, int r, unsigned sz, int tmp) {
    if (sz == 32) return XR(r);
    if (sz == 16) { ubfm(e, tmp, XR(r), 0, 15); return tmp; }
    if (r < 4) ubfm(e, tmp, XR(r), 0, 7);
    else ubfm(e, tmp, XR(r - 4), 8, 15);
    return tmp;
}
static void reg_write(XE *e, int r, unsigned sz, int v) {
    if (sz == 32) movr(e, XR(r), v);
    else if (sz == 16) bfi(e, XR(r), v, 0, 16);
    else if (r < 4) bfi(e, XR(r), v, 0, 8);
    else bfi(e, XR(r - 4), v, 8, 8);
}
static void lf_store(XE *e, int op, unsigned sz, int res, int a, int b) {
    stp_cpu(e, res, a, offsetof(x86cpu, lf_res));
    str_cpu(e, b, offsetof(x86cpu, lf_b));
    mov_w(e, 9, (uint32_t)op);
    mov_w(e, 10, sz);
    stp_cpu(e, 9, 10, offsetof(x86cpu, lf_op));
}

/* Evaluates x86 condition cc. Returns the AArch64 condition that means true. */
static int cond_eval(XT *t, const XIns *x, int *nz) {
    XE *e = &t->e;
    if (x->native_cc) return xj_a64_cc(*nz, x->cc);
    mov_w(e, 1, x->cc);
    bl_to(e, xj.cond_thunk);
    E(e, 0x7100001Fu);                             /* cmp w0,#0 */
    *nz = NZ_NONE;
    return C_NE;
}

/* Registers are x86 state between instructions, so a value in w1-w5 stays there
 * across mem_check, which only touches w16 and w17. */
static void gen_alu(XT *t, const XIns *x, int *nz) {
    XE *e = &t->e;
    unsigned sz = x->sz, sh = 32u - sz;
    int addr = -1, a, b;
    bool mem_a = x->has_mem && x->rm_dst, mem_b = x->has_mem && !x->rm_dst;
    int dreg = x->rm_dst ? x->rm : x->reg;

    if (x->has_mem) {
        addr = ea(t, &x->m);
        if (addr == 0) { movr(e, 5, 0); addr = 5; }
    }
    if (mem_a) { load(t, 1, addr, sz, false); a = 1; }
    else a = reg_read(e, dreg, sz, 1);
    if (x->has_imm) {
        uint32_t im = sz == 32 ? x->imm : x->imm & ((1u << sz) - 1u);
        mov_w(e, 2, im);
        b = 2;
    } else if (mem_b) { load(t, 2, addr, sz, false); b = 2; }
    else if (x->rm_dst) b = reg_read(e, x->reg, sz, 2);
    else if (x->has_mem) b = 2;
    else b = reg_read(e, x->rm, sz, 2);

    /* Small sizes compute at the top of the word, where NZCV come out right. */
    int oa = a, ob = b;
    if (sz != 32) {
        lsl_i(e, 3, a, sh); a = 3;
        lsl_i(e, 4, b, sh); b = 4;
    }
    int res = 0;
    switch (x->op) {
    case 0: E(e, R3(0x2B000000u, res, a, b)); *nz = NZ_ADD; break;
    case 5: case 7: E(e, R3(0x6B000000u, res, a, b)); *nz = NZ_SUB; break;
    case 4: E(e, R3(0x6A000000u, res, a, b)); *nz = NZ_LOGIC; break;
    case 1: E(e, R3(0x2A000000u, res, a, b)); E(e, R3(0x6A000000u, WZR, res, res)); *nz = NZ_LOGIC; break;
    default: E(e, R3(0x4A000000u, res, a, b)); E(e, R3(0x6A000000u, WZR, res, res)); *nz = NZ_LOGIC; break;
    }
    if (sz != 32) lsr_i(e, res, res, sh);
    if (x->writes) {
        if (mem_a) store(t, res, addr, sz);
        /* lf_a and lf_b below read oa and ob: copy them before a register changes. */
        else if (x->need_lf && (oa >= 19 || ob >= 19)) {
            if (oa >= 19) { movr(e, 1, oa); oa = 1; }
            if (ob >= 19) { movr(e, 2, ob); ob = 2; }
            reg_write(e, dreg, sz, res);
        } else reg_write(e, dreg, sz, res);
    }
    if (x->need_lf) {
        int lf = x->op == 0 ? LF_ADD : (x->op == 5 || x->op == 7) ? LF_SUB : LF_LOGIC;
        lf_store(e, lf, sz, res, oa, ob);
    }
}

static void gen_incdec(XT *t, const XIns *x) {
    XE *e = &t->e;
    unsigned sz = x->sz, sh = 32u - sz;
    int addr = -1, a;
    if (x->has_mem) {
        addr = ea(t, &x->m);
        if (addr == 0) { movr(e, 5, 0); addr = 5; }
        load(t, 1, addr, sz, false);
        a = 1;
    } else a = reg_read(e, x->rm, sz, 1);
    if (sz != 32) { lsl_i(e, 3, a, sh); mov_w(e, 4, 1u << sh); }
    else mov_w(e, 4, 1);
    /* ADDS/SUBS with register 1: an immediate form would do, this keeps one path. */
    E(e, R3(x->op ? 0x6B000000u : 0x2B000000u, 0, sz != 32 ? 3 : a, 4));
    if (sz != 32) lsr_i(e, 0, 0, sh);
    if (x->has_mem) store(t, 0, addr, sz);
    else reg_write(e, x->rm, sz, 0);
    if (x->need_lf) {
        /* CF is dead here by construction, so lf_cf need not be correct. */
        mov_w(e, 2, 1);
        lf_store(e, x->op ? LF_DEC : LF_INC, sz, 0, x->has_mem ? 1 : (sz == 32 ? 2 : 1), 2);
    }
}

static void gen_shift(XT *t, const XIns *x) {
    XE *e = &t->e;
    unsigned sz = x->sz;
    int addr = -1, a;
    uint32_t *skip = NULL;
    if (x->has_imm && !(x->imm & 31)) return;      /* count 0: nothing at all */
    if (x->has_mem) {
        addr = ea(t, &x->m);
        if (addr == 0) { movr(e, 5, 0); addr = 5; }
        load(t, 1, addr, sz, false);
        a = 1;
    } else {
        a = reg_read(e, x->rm, sz, 1);
        if (a != 1) { movr(e, 1, a); a = 1; }
    }
    if (x->imm_cl) {
        ubfm(e, 2, XR(X86_ECX), 0, 4);             /* count = CL & 31 */
        skip = e->p;
        E(e, 0x34000000u | 2u);                    /* cbz w2,end */
    } else mov_w(e, 2, x->imm & 31);
    /* a holds the zero-extended value; SAR works on the sign-extended one. */
    switch (x->op) {
    case 0: E(e, R3(0x4B000000u, 3, WZR, 2)); E(e, R3(0x1AC02C00u, 0, 1, 3)); break;    /* ROL = ROR by -n */
    case 1: E(e, R3(0x1AC02C00u, 0, 1, 2)); break;
    case 4: E(e, R3(0x1AC02000u, 0, 1, 2)); break;
    case 5: E(e, R3(0x1AC02400u, 0, 1, 2)); break;
    default:
        if (sz == 32) movr(e, 3, 1);
        else if (sz == 16) sbfm(e, 3, 1, 0, 15);
        else sbfm(e, 3, 1, 0, 7);
        E(e, R3(0x1AC02800u, 0, 3, 2));
        break;
    }
    if (sz == 16) ubfm(e, 0, 0, 0, 15);
    else if (sz == 8) ubfm(e, 0, 0, 0, 7);
    if (x->has_mem) store(t, 0, addr, sz);
    else reg_write(e, x->rm, sz, 0);
    if (x->need_lf && x->op >= 4) lf_store(e, x->op == 4 ? LF_SHL : x->op == 5 ? LF_SHR : LF_SAR, sz, 0, 1, 2);
    if (skip) *skip |= (uint32_t)(e->p - skip) << 5;
}

/* Budget and leaving the block. A direct exit branches to a stub that asks the
 * dispatcher to link it. */
static void budget(XT *t) {
    XE *e = &t->e;
    ldr_cpu(e, 9, offsetof(x86cpu, left));
    sub_i(e, 9, 9, (uint32_t)t->n);
    str_cpu(e, 9, offsetof(x86cpu, left));
}
typedef struct { uint32_t *site, *over; uint32_t target; } XOut;
static void exit_direct(XT *t, XOut *outs, int *nouts, uint32_t target) {
    XE *e = &t->e;
    budget(t);
    outs[*nouts].over = e->p;
    E(e, 0x37000000u | (31u << 19) | 9u);          /* tbnz w9,#31,stub */
    outs[*nouts].site = e->p;
    outs[*nouts].target = target;
    (*nouts)++;
    E(e, 0x14000000u);                             /* patched */
}
static void exit_indirect(XT *t) {
    XE *e = &t->e;
    budget(t);
    b_to(e, xj.ibt_stub);
}

/* x87: registers live in memory as in the interpreter. Translated forms keep its
 * double precision and tag handling, see fpush and fpop. A NaN result goes back to
 * the interpreter, whose clean-up gives it the x87 sign and payload. */
static void fp_top(XE *e, int rd) { ldr_cpu(e, rd, offsetof(x86cpu, top)); }
/* Tag word of register wtop: empty (fpop) or valid (fpush). Uses w3 and w4. */
static void fp_tag(XE *e, int wtop, bool empty) {
    mov_w(e, 3, 3);
    lsl_i(e, 4, wtop, 1);
    E(e, R3(0x1AC02000u, 3, 3, 4));                /* lsl w3,w3,w4 */
    E(e, 0x79400000u | ((uint32_t)(offsetof(x86cpu, ftw) / 2) << 10) | (XCPU << 5) | 4u);   /* ldrh w4,ftw */
    if (empty) E(e, R3(0x2A000000u, 4, 4, 3));     /* orr */
    else E(e, R3(0x0A200000u, 4, 4, 3));           /* bic */
    E(e, 0x79000000u | ((uint32_t)(offsetof(x86cpu, ftw) / 2) << 10) | (XCPU << 5) | 4u);   /* strh */
}
static void fp_st_addr(XE *e, int xd, int wtop, unsigned i) {
    /* xd = &c->st[(top + i) & 7] */
    add_i(e, 17, wtop, i);
    ubfm(e, 17, 17, 0, 2);
    E(e, 0x91000000u | ((uint32_t)offsetof(x86cpu, st) << 10) | (XCPU << 5) | (uint32_t)xd);
    E(e, 0x8B000000u | (17u << 16) | (3u << 10) | ((uint32_t)xd << 5) | (uint32_t)xd);
}
static void fp_load_mem(XT *t, int addr, unsigned bits) {
    /* d0 = value at addr */
    XE *e = &t->e;
    mem_check(t, addr, false, bits / 8u);
    if (bits == 32) {
        E(e, R3(0xBC604800u, 0, 16, addr));        /* ldr s0,[x16,addr,uxtw] */
        E(e, 0x1E22C000u);                         /* fcvt d0,s0 */
    } else {
        E(e, R3(0xFC604800u, 0, 16, addr));        /* ldr d0,[x16,addr,uxtw] */
    }
}
static const uint32_t fp_ops[4] = { 0x1E602800u, 0x1E600800u, 0x1E603800u, 0x1E601800u };   /* add mul sub div */
/* d0 = d1 op d2, with x86 op numbering 0 add, 1 mul, 4 sub, 5 subr, 6 div, 7 divr */
static void fp_arith(XE *e, unsigned op) {
    uint32_t base;
    int n = 1, m = 2;
    switch (op) {
    case 0: base = fp_ops[0]; break;
    case 1: base = fp_ops[1]; break;
    case 4: base = fp_ops[2]; break;
    case 5: base = fp_ops[2]; n = 2; m = 1; break;
    case 6: base = fp_ops[3]; break;
    default: base = fp_ops[3]; n = 2; m = 1; break;
    }
    E(e, base | ((uint32_t)m << 16) | ((uint32_t)n << 5));
}
/* Before any state changes: a NaN in d0 re-runs the instruction in the interpreter.
 * NZCV stays untouched. */
static void fp_nan_check(XT *t) {
    XE *e = &t->e;
    E(e, 0x5E60E400u | (4u << 0));                 /* fcmeq d4,d0,d0 */
    E(e, 0x9E660000u | (4u << 5) | 4u);            /* fmov x4,d4 */
    cbz_stub_x(t, 4);
}

static void gen_ins(XT *t, XIns *x, int *nz, XOut *outs, int *nouts) {
    XE *e = &t->e;
    int addr, v;
    switch (x->kind) {
    case K_ALU: gen_alu(t, x, nz); break;
    case K_INCDEC: gen_incdec(t, x); *nz = NZ_INCDEC; break;
    case K_NOP: break;
    case K_MOV:
        if (x->has_imm) {
            uint32_t im = x->sz == 32 ? x->imm : x->imm & ((1u << x->sz) - 1u);
            if (x->has_mem) {
                addr = ea(t, &x->m);
                if (addr == 0) { movr(e, 5, 0); addr = 5; }
                mov_w(e, 1, im);
                store(t, 1, addr, x->sz);
            } else if (x->sz == 32) mov_w(e, XR(x->rm), im);
            else { mov_w(e, 1, im); reg_write(e, x->rm, x->sz, 1); }
        } else if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            if (x->rm_dst) { v = reg_read(e, x->reg, x->sz, 1); store(t, v, addr, x->sz); }
            else if (x->sz == 32) { load(t, 1, addr, 32, false); movr(e, XR(x->reg), 1); }
            else { load(t, 1, addr, x->sz, false); reg_write(e, x->reg, x->sz, 1); }
        } else {
            int s = x->rm_dst ? x->reg : x->rm, d = x->rm_dst ? x->rm : x->reg;
            v = reg_read(e, s, x->sz, 1);
            reg_write(e, d, x->sz, v);
        }
        break;
    case K_MOVX:
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, x->src_sz, x->src_signed);
            v = 1;
        } else {
            v = reg_read(e, x->rm, x->src_sz, 1);
            if (x->src_signed) { sbfm(e, 1, v, 0, x->src_sz - 1u); v = 1; }
        }
        reg_write(e, x->reg, x->sz, v);
        break;
    case K_LEA: {
        XMem m = x->m;
        v = ea(t, &m);
        movr(e, XR(x->reg), v);
        break;
    }
    case K_PUSH:
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, 32, false);
            v = 1;
        } else if (x->has_imm) { mov_w(e, 1, x->imm); v = 1; }
        else v = XR(x->rm);
        sub_i(e, 2, XR(X86_ESP), 4);
        store(t, v, 2, 32);
        movr(e, XR(X86_ESP), 2);
        break;
    case K_POP:
        load(t, 1, XR(X86_ESP), 32, false);
        add_i(e, XR(X86_ESP), XR(X86_ESP), 4);
        movr(e, XR(x->rm), 1);
        break;
    case K_LEAVE:
        load(t, 1, XR(X86_EBP), 32, false);
        add_i(e, XR(X86_ESP), XR(X86_EBP), 4);
        movr(e, XR(X86_EBP), 1);
        break;
    case K_NOT:
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, x->sz, false);
            E(e, R3(0x2A2003E0u, 1, 0, 1));        /* mvn w1,w1 */
            store(t, 1, addr, x->sz);
        } else {
            v = reg_read(e, x->rm, x->sz, 1);
            E(e, R3(0x2A2003E0u, 1, 0, v));
            reg_write(e, x->rm, x->sz, 1);
        }
        break;
    case K_NEG:
        /* Only with dead flags, see xj_analyse */
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, x->sz, false);
            E(e, R3(0x4B000000u, 1, WZR, 1));
            store(t, 1, addr, x->sz);
        } else {
            v = reg_read(e, x->rm, x->sz, 1);
            E(e, R3(0x4B000000u, 1, WZR, v));
            reg_write(e, x->rm, x->sz, 1);
        }
        *nz = NZ_NONE;
        break;
    case K_SHIFT: gen_shift(t, x); *nz = NZ_NONE; break;
    case K_IMUL:
        /* Only with dead flags, see xj_analyse */
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, 32, false);
            v = 1;
        } else v = XR(x->rm);
        if (x->has_imm) { mov_w(e, 2, x->imm); E(e, 0x1B007C00u | (2u << 16) | ((uint32_t)v << 5) | (uint32_t)XR(x->reg)); }
        else E(e, 0x1B007C00u | ((uint32_t)XR(x->reg) << 16) | ((uint32_t)v << 5) | (uint32_t)XR(x->reg));
        *nz = NZ_NONE;
        break;
    case K_CDQ:
        if (x->sz == 32) asr_i(e, XR(X86_EDX), XR(X86_EAX), 31);
        else { sbfm(e, 1, XR(X86_EAX), 0, 15); asr_i(e, 1, 1, 16); bfi(e, XR(X86_EDX), 1, 0, 16); }
        break;
    case K_CWDE:
        if (x->sz == 32) sbfm(e, XR(X86_EAX), XR(X86_EAX), 0, 15);
        else { sbfm(e, 1, XR(X86_EAX), 0, 7); bfi(e, XR(X86_EAX), 1, 0, 16); }
        break;
    case K_XCHG:
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, x->sz, false);
            v = reg_read(e, x->reg, x->sz, 2);
            store(t, v, addr, x->sz);
            reg_write(e, x->reg, x->sz, 1);
        } else {
            int r1 = reg_read(e, x->reg, x->sz, 1), r2 = reg_read(e, x->rm, x->sz, 2);
            if (r1 != 1) { movr(e, 1, r1); r1 = 1; }
            reg_write(e, x->reg, x->sz, r2);
            reg_write(e, x->rm, x->sz, r1);
        }
        break;
    case K_SETCC: {
        /* The condition first: a helper call loses w0-w17. */
        int c = cond_eval(t, x, nz);
        E(e, 0x1A9F07E0u | (((uint32_t)c ^ 1u) << 12) | 1u);   /* cset w1,c */
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            store(t, 1, addr, 8);
        } else reg_write(e, x->rm, 8, 1);
        break;
    }
    case K_CMOV: {
        int c = cond_eval(t, x, nz);
        if (x->has_mem) {
            /* The load happens whatever the condition, as on real hardware. */
            E(e, 0x1A9F07E0u | (((uint32_t)c ^ 1u) << 12) | 3u);   /* cset w3,c */
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, 32, false);
            E(e, 0x7100001Fu | (3u << 5));         /* cmp w3,#0 */
            c = C_NE;
            v = 1;
        } else v = XR(x->rm);
        E(e, 0x1A800000u | ((uint32_t)XR(x->reg) << 16) | ((uint32_t)c << 12) | ((uint32_t)v << 5) | (uint32_t)XR(x->reg));
        break;
    }
    case K_FLD_M:
        addr = ea(t, &x->m);
        if (addr == 0) { movr(e, 5, 0); addr = 5; }
        fp_load_mem(t, addr, x->src_sz);
        fp_top(e, 1);
        sub_i(e, 1, 1, 1);
        ubfm(e, 1, 1, 0, 2);
        fp_st_addr(e, 2, 1, 0);
        E(e, 0xFD000000u | (2u << 5) | 0u);        /* str d0,[x2] */
        str_cpu(e, 1, offsetof(x86cpu, top));
        fp_tag(e, 1, false);
        break;
    case K_FSTP_M:
        addr = ea(t, &x->m);
        if (addr == 0) { movr(e, 5, 0); addr = 5; }
        fp_top(e, 1);
        fp_st_addr(e, 2, 1, 0);
        E(e, 0xFD400000u | (2u << 5) | 0u);        /* ldr d0,[x2] */
        mem_check(t, addr, true, x->src_sz / 8u);
        if (x->src_sz == 32) {
            E(e, 0x1E624000u);                     /* fcvt s0,d0 */
            E(e, R3(0xBC204800u, 0, 16, addr));
        } else E(e, R3(0xFC204800u, 0, 16, addr));
        if (x->op) {
            fp_tag(e, 1, true);
            add_i(e, 1, 1, 1);
            ubfm(e, 1, 1, 0, 2);
            str_cpu(e, 1, offsetof(x86cpu, top));
        }
        break;
    case K_FARITH_M:
        addr = ea(t, &x->m);
        if (addr == 0) { movr(e, 5, 0); addr = 5; }
        fp_load_mem(t, addr, x->src_sz);
        E(e, 0x1E604002u);                         /* fmov d2,d0 */
        fp_top(e, 1);
        fp_st_addr(e, 2, 1, 0);
        E(e, 0xFD400000u | (2u << 5) | 1u);        /* ldr d1,[x2] */
        fp_arith(e, x->op);
        fp_nan_check(t);
        E(e, 0xFD000000u | (2u << 5) | 0u);
        break;
    case K_FARITH_ST:
        /* rm is the ST index; cc: 0 ST0 = ST0 op STi, 1 STi = STi op ST0, 2 the same and pop */
        fp_top(e, 1);
        fp_st_addr(e, 2, 1, 0);
        fp_st_addr(e, 3, 1, x->rm);
        if (x->cc == 0) {
            E(e, 0xFD400000u | (2u << 5) | 1u);
            E(e, 0xFD400000u | (3u << 5) | 2u);
            fp_arith(e, x->op);
            fp_nan_check(t);
            E(e, 0xFD000000u | (2u << 5) | 0u);
        } else {
            E(e, 0xFD400000u | (3u << 5) | 1u);
            E(e, 0xFD400000u | (2u << 5) | 2u);
            fp_arith(e, x->op);
            fp_nan_check(t);
            E(e, 0xFD000000u | (3u << 5) | 0u);
            if (x->cc == 2) {
                fp_tag(e, 1, true);
                add_i(e, 1, 1, 1);
                ubfm(e, 1, 1, 0, 2);
                str_cpu(e, 1, offsetof(x86cpu, top));
            }
        }
        break;
    case K_JCC: {
        int c = cond_eval(t, x, nz);
        uint32_t *skip = e->p;
        E(e, 0x54000000u | (uint32_t)c);           /* b.c taken */
        exit_direct(t, outs, nouts, x->next);
        *skip |= (uint32_t)(e->p - skip) << 5;
        exit_direct(t, outs, nouts, x->target);
        break;
    }
    case K_JMP: exit_direct(t, outs, nouts, x->target); break;
    case K_CALL:
        mov_w(e, 1, x->next);
        sub_i(e, 2, XR(X86_ESP), 4);
        store(t, 1, 2, 32);
        movr(e, XR(X86_ESP), 2);
        exit_direct(t, outs, nouts, x->target);
        break;
    case K_RET:
        load(t, 1, XR(X86_ESP), 32, false);
        add_i(e, XR(X86_ESP), XR(X86_ESP), 4);
        if (x->has_imm && x->imm) {
            if (x->imm < 4096) add_i(e, XR(X86_ESP), XR(X86_ESP), x->imm);
            else { mov_w(e, 2, x->imm); E(e, R3(0x0B000000u, XR(X86_ESP), XR(X86_ESP), 2)); }
        }
        exit_indirect(t);
        break;
    case K_JMPI:
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, 32, false);
        } else movr(e, 1, XR(x->rm));
        exit_indirect(t);
        break;
    case K_CALLI:
        if (x->has_mem) {
            addr = ea(t, &x->m);
            if (addr == 0) { movr(e, 5, 0); addr = 5; }
            load(t, 1, addr, 32, false);
        } else movr(e, 1, XR(x->rm));
        mov_w(e, 3, x->next);
        sub_i(e, 2, XR(X86_ESP), 4);
        store(t, 3, 2, 32);
        movr(e, XR(X86_ESP), 2);
        exit_indirect(t);
        break;
    default:
        break;
    }
}

/* ---- runtime ---- */
/* next: where the block goes on. The interpreter can stay elsewhere: REP string
 * instructions run in portions and leave eip on themselves until the count is done. */
static uint64_t xj_interp_helper(x86cpu *c, uint32_t eip, uint32_t next) {
    c->eip = eip;
    c->left--;
    xj.interp_ins++;
    exec_one(c);
    return (uint64_t)(c->xj_abort != 0 || c->eip != next);
}
static uint64_t xj_cond_helper(x86cpu *c, uint32_t cc) { return (uint64_t)cond(c, (int)cc); }

static inline unsigned xj_hash(uint32_t eip) { return (eip ^ (eip >> 15)) & (XJ_HASH - 1u); }
static XBlock *xj_lookup(uint32_t eip) {
    for (XBlock *b = xj.hash[xj_hash(eip)]; b; b = b->hash_next) if (b->eip == eip) return b;
    return NULL;
}

/* A flat view of the shadow page table; see shadow_rebuild. */
static inline void xj_table_page(uint32_t vpage, x86page *p) {
    if (!xj.on) return;
    if (!p || !p->mem) { xj.rt[vpage] = 0; xj.wt[vpage] = 0; return; }
    uintptr_t base = (uintptr_t)p->mem - ((uintptr_t)vpage << 12);
    xj.rt[vpage] = base;
    xj.wt[vpage] = (p->flags & X86_PG_W) && !(p->flags & X86_PG_CODE) ? base : 0;
}
static void xj_unlink_all(void) {
    xj_w_enable(true);
    for (size_t i = 0; i < xj.nlinks; i++) {
        uint32_t *s = xj.links[i].site;
        *s = 0x14000000u | ((uint32_t)(xj.links[i].stub - s) & 0x03FFFFFFu);
        xj_sync(s, 4);
    }
    xj_w_enable(false);
    xj.nlinks = 0;
    memset(xj.ibt, 0, sizeof(XIbt) << XJ_IBT_BITS);
}
static void xj_gate_ram(uint32_t rampage, bool on);
/* Drops every translation of one x86 page. */
static void xj_kill_page(uint32_t vpage) {
    XBlock *b = xj.page_blocks[vpage];
    x86page *t = host.dir[vpage >> 10];
    if (!b) return;
    xj.page_blocks[vpage] = NULL;
    while (b) {
        XBlock *next = b->page_next;
        XBlock **h = &xj.hash[xj_hash(b->eip)];
        while (*h && *h != b) h = &(*h)->hash_next;
        if (*h) *h = b->hash_next;
        free(b);
        b = next;
    }
    xj_unlink_all();
    if (t) {
        x86page *p = &t[vpage & 1023u];
        p->flags &= ~X86_PG_CODE;
        xj_table_page(vpage, p);
        if (p->mem && xj.cpu) {
            uint32_t rp = (uint32_t)((uintptr_t)(p->mem - xj.cpu->ram_owned) >> 12);
            if ((uint64_t)rp << 12 < xj.cpu->vm.ram.size) xj.ram_virt[rp] = 0;
        }
    }
    tlb_flush(&host);
    if (xj.page_kills[vpage] < 255) xj.page_kills[vpage]++;
    if (xj.page_kills[vpage] == XJ_KILLS_VOLATILE) xj.volatile_pages++;
    xj.kills++;
    host.xj_abort = 1;
}
static uint64_t xp_notify_calls, xp_notify_bytes;
static uint32_t xp_notify_page[1024];
/* The interpreter's write path reached a page with translated code. */
static void x86_host_code_write(x86cpu *c, uint32_t a) { (void)c; xj_kill_page(a >> 12); }
/* A 68k write or DMA reached RAM holding translated x86 code. */
static void xj_ram_written(QRXFullCPU *c, uint32_t addr, uint32_t len) {
    (void)c;
    if (!xj.on || !len) return;
    xp_notify_calls++; xp_notify_bytes += len;
    xp_notify_page[(addr >> 12) & 1023u]++;
    uint64_t end = (uint64_t)addr + len;
    for (uint64_t a = addr; a < end; a = (a | 0xFFFu) + 1u) {
        uint32_t rp = (uint32_t)(a >> 12);
        if ((uint64_t)rp << 12 >= xj.cpu->vm.ram.size) break;
        if (xj.ram_virt[rp]) xj_kill_page(xj.ram_virt[rp] - 1u);
    }
}
static void xj_gate_ram(uint32_t rampage, bool on) {
    QRXFullCPU *c = xj.cpu;
    if (!on || !c->wpage) return;
    uint32_t first = rampage << (12u - QRX_WPAGE_SHIFT);
    for (uint32_t i = 0; i < (1u << (12u - QRX_WPAGE_SHIFT)); i++) c->wpage[first + i] = 0;
    /* The previous granule: its last bytes can spill over the boundary. */
    if (first) c->wpage[first - 1u] = 0;
}
/* The 68k JIT rebuilt its write map: gate x86 code pages again. */
static void xj_regate(QRXFullCPU *c) {
    (void)c;
    if (!xj.on) return;
    for (uint64_t rp = 0; (rp << 12) < xj.cpu->vm.ram.size; rp++)
        if (xj.ram_virt[rp]) xj_gate_ram((uint32_t)rp, true);
}

static bool xj_gen_runtime(void) {
    XE e = { xj.arena };
    /* void enter(x86cpu*, void *code, rt, wt) */
    xj.enter_code = e.p;
    E(&e, 0xA9BA7BFDu);                            /* stp x29,x30,[sp,#-96]! */
    E(&e, 0xA90153F3u);                            /* stp x19,x20,[sp,#16] */
    E(&e, 0xA9025BF5u);                            /* stp x21,x22,[sp,#32] */
    E(&e, 0xA90363F7u);                            /* stp x23,x24,[sp,#48] */
    E(&e, 0xA9046BF9u);                            /* stp x25,x26,[sp,#64] */
    E(&e, 0xA90573FBu);                            /* stp x27,x28,[sp,#80] */
    E(&e, 0x910003FDu);                            /* mov x29,sp */
    E(&e, R3(0xAA0003E0u, XCPU, 0, 0));            /* mov x27,x0 */
    E(&e, R3(0xAA0003E0u, XRT, 0, 2));             /* mov x28,x2 */
    ldp_cpu(&e, 19, 20, 0); ldp_cpu(&e, 21, 22, 8); ldp_cpu(&e, 23, 24, 16); ldp_cpu(&e, 25, 26, 24);
    E(&e, 0xD61F0000u | (1u << 5));                /* br x1 */

    /* Exit with x10 = branch site to link, or none */
    xj.epi_plain = e.p;
    E(&e, R3(0xAA0003E0u, 10, 0, WZR));            /* mov x10,xzr */
    xj.epi_link = e.p;
    E(&e, 0xF9000000u | ((uint32_t)(offsetof(x86cpu, xj_site) / 8) << 10) | (XCPU << 5) | 10u);
    stp_cpu(&e, 19, 20, 0); stp_cpu(&e, 21, 22, 8); stp_cpu(&e, 23, 24, 16); stp_cpu(&e, 25, 26, 24);
    E(&e, 0xA94573FBu);                            /* ldp x27,x28,[sp,#80] */
    E(&e, 0xA9446BF9u);
    E(&e, 0xA94363F7u);
    E(&e, 0xA9425BF5u);
    E(&e, 0xA94153F3u);
    E(&e, 0xA8C67BFDu);                            /* ldp x29,x30,[sp],#96 */
    E(&e, 0xD65F03C0u);

    /* w0 = eip of the instruction, w2 = the next one: runs it in the interpreter, keeps NZCV.
     * Returns with x0 != 0 when the block must end. */
    xj.interp_thunk = e.p;
    E(&e, 0xD53B4209u);                            /* mrs x9,nzcv */
    E(&e, 0xA9BE27FEu);                            /* stp x30,x9,[sp,#-32]! */
    stp_cpu(&e, 19, 20, 0); stp_cpu(&e, 21, 22, 8); stp_cpu(&e, 23, 24, 16); stp_cpu(&e, 25, 26, 24);
    E(&e, R3(0x2A0003E0u, 1, 0, 0));               /* mov w1,w0 */
    E(&e, R3(0xAA0003E0u, 0, 0, XCPU));            /* mov x0,x27 */
    mov_x(&e, 16, (uint64_t)(uintptr_t)xj_interp_helper);
    E(&e, 0xD63F0000u | (16u << 5));               /* blr x16 */
    ldp_cpu(&e, 19, 20, 0); ldp_cpu(&e, 21, 22, 8); ldp_cpu(&e, 23, 24, 16); ldp_cpu(&e, 25, 26, 24);
    E(&e, 0xA8C227FEu);                            /* ldp x30,x9,[sp],#32 */
    E(&e, 0xD51B4209u);                            /* msr nzcv,x9 */
    E(&e, 0xD65F03C0u);

    /* w1 = condition: w0 = its value in the lazy flags. */
    xj.cond_thunk = e.p;
    E(&e, 0xF81F0FFEu);                            /* str x30,[sp,#-16]! */
    E(&e, R3(0xAA0003E0u, 0, 0, XCPU));
    mov_x(&e, 16, (uint64_t)(uintptr_t)xj_cond_helper);
    E(&e, 0xD63F0000u | (16u << 5));
    E(&e, 0xF84107FEu);                            /* ldr x30,[sp],#16 */
    E(&e, 0xD65F03C0u);

    /* w1 = target eip of an indirect branch */
    xj.ibt_stub = e.p;
    str_cpu(&e, 1, offsetof(x86cpu, eip));
    ldr_cpu(&e, 9, offsetof(x86cpu, left));
    uint32_t *neg = e.p;
    E(&e, 0x37000000u | (31u << 19) | 9u);         /* tbnz w9,#31,out */
    mov_x(&e, 11, (uint64_t)(uintptr_t)xj.ibt);
    /* index = (eip ^ eip >> 12) & mask */
    lsr_i(&e, 12, 1, 12);
    E(&e, R3(0x4A000000u, 12, 12, 1));
    ubfm(&e, 12, 12, 0, XJ_IBT_BITS - 1u);
    E(&e, 0x8B000000u | (12u << 16) | (4u << 10) | (11u << 5) | 11u);    /* add x11,x11,x12,lsl #4 */
    E(&e, 0xA9400000u | (12u << 10) | (11u << 5) | 13u);                 /* ldp x13,x12,[x11] */
    E(&e, 0xEB01001Fu | (13u << 5));               /* cmp x13,x1 */
    uint32_t *miss = e.p;
    E(&e, 0x54000000u | C_NE);
    E(&e, 0xD61F0000u | (12u << 5));               /* br x12 */
    *neg |= (uint32_t)(e.p - neg) << 5;
    *miss |= (uint32_t)(e.p - miss) << 5;
    b_to(&e, xj.epi_plain);

    xj.arena_blocks = e.p;
    return true;
}
static bool xj_init(QRXFullCPU *c) {
    if (xj.on) return true;
    if (xj.failed || !c->wpage || !c->rpage) return false;
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
    flags |= MAP_JIT;
#endif
    void *code = mmap(NULL, XJ_ARENA, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    xj.rt = mmap(NULL, 2u * XJ_PAGES * sizeof(uintptr_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    xj.wt = xj.rt + XJ_PAGES;
    xj.page_blocks = mmap(NULL, XJ_PAGES * sizeof(XBlock *), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    xj.ibt = mmap(NULL, sizeof(XIbt) << XJ_IBT_BITS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    xj.page_kills = calloc(XJ_PAGES, 1);
    xj.ram_virt = calloc((size_t)(c->vm.ram.size >> 12) + 1u, sizeof(uint32_t));
    if (code == MAP_FAILED || xj.rt == MAP_FAILED || xj.page_blocks == MAP_FAILED ||
        xj.ibt == MAP_FAILED || !xj.page_kills || !xj.ram_virt) { xj.failed = true; return false; }
    xj.arena = code;
    xj.end = xj.arena + XJ_ARENA / 4u;
    xj.cpu = c;
    xj_w_enable(true);
    xj_gen_runtime();
    xj_w_enable(false);
    xj_sync(xj.arena, (size_t)((char *)xj.arena_blocks - (char *)xj.arena));
    xj.next = xj.arena_blocks;
    xj.on = true;
    /* Tables start out empty; fill them from what is mapped already. */
    for (unsigned i = 0; i < 1024; i++) {
        x86page *t = host.dir[i];
        if (!t) continue;
        for (unsigned j = 0; j < 1024; j++) xj_table_page((i << 10) | j, &t[j]);
    }
    qrx_x86code_notify = xj_ram_written;
    qrx_x86code_regate = xj_regate;
    return true;
}
static void xj_flush_all(void) {
    for (uint32_t i = 0; i < XJ_HASH; i++) {
        for (XBlock *b = xj.hash[i]; b;) { XBlock *n = b->hash_next; free(b); b = n; }
        xj.hash[i] = NULL;
    }
    for (unsigned i = 0; i < 1024; i++) {
        x86page *t = host.dir[i];
        if (!t) continue;
        for (unsigned j = 0; j < 1024; j++) {
            uint32_t vp = (i << 10) | j;
            if (t[j].flags & X86_PG_CODE) { t[j].flags &= ~X86_PG_CODE; xj_table_page(vp, &t[j]); }
            xj.page_blocks[vp] = NULL;
        }
    }
    memset(xj.ram_virt, 0, ((size_t)(xj.cpu->vm.ram.size >> 12) + 1u) * sizeof(uint32_t));
    memset(xj.ibt, 0, sizeof(XIbt) << XJ_IBT_BITS);
    xj.nlinks = 0;
    xj.next = xj.arena_blocks;
    tlb_flush(&host);
    xj.flushes++;
}

/* Marks the page as holding code: writes from here on come back to us. */
static void xj_protect_page(uint32_t vpage) {
    x86page *t = host.dir[vpage >> 10];
    if (!t) return;
    x86page *p = &t[vpage & 1023u];
    if (!p->mem || (p->flags & X86_PG_CODE)) return;
    p->flags |= X86_PG_CODE;
    xj.wt[vpage] = 0;
    tlb_flush(&host);
    uint64_t off = (uint64_t)(uintptr_t)p->mem - (uint64_t)(uintptr_t)xj.cpu->ram_owned;
    if (off < xj.cpu->vm.ram.size) {
        uint32_t rp = (uint32_t)(off >> 12);
        xj.ram_virt[rp] = vpage + 1u;
        xj_gate_ram(rp, true);
    }
}

/* QRX86_JITOFF=mask: instruction kinds (bit = K_ value) left to the interpreter, to find
 * by bisection which translation a misbehaving program depends on. */
static uint32_t xj_off_mask(void) {
    static int have;
    static uint32_t mask;
    if (!have) { const char *e = getenv("QRX86_JITOFF"); mask = e ? (uint32_t)strtoul(e, NULL, 0) : 0; have = 1; }
    return mask;
}
static XBlock *xj_translate(uint32_t eip) {
    static XIns ins[XJ_MAX_INS];
    static XT t;
    static XOut outs[XJ_MAX_INS * 2];
    uint32_t vpage = eip >> 12, a = eip;
    int n = 0, nouts = 0;

    if (xj.page_kills[vpage] >= XJ_KILLS_VOLATILE) return NULL;
    while (n < XJ_MAX_INS) {
        XIns *x = &ins[n];
        if (a >> 12 != vpage || !xj_decode(a, x)) break;
        if (x->kind != K_FALL && x->kind != K_FALL_END && ((xj_off_mask() >> x->kind) & 1u)) {
            bool control = x->kind == K_JCC || x->kind == K_JMP || x->kind == K_CALL || x->kind == K_RET || x->kind == K_JMPI || x->kind == K_CALLI;
            x->kind = control ? K_FALL_END : K_FALL;
        }
        n++;
        if (x->kind == K_FALL_END || x->kind == K_JCC || x->kind == K_JMP || x->kind == K_CALL ||
            x->kind == K_RET || x->kind == K_JMPI || x->kind == K_CALLI) break;
        a = x->next;
        /* A fall-through into the HLE window ends here; the dispatcher handles it. */
        if (a - host.hle_base < host.hle_count) break;
    }
    if (!n) return NULL;
    if ((size_t)(xj.end - xj.next) < XJ_MAX_INS * 64 + 256) {
        xj_flush_all();
        host.xj_abort = 1;
    }
    /* What may read the flags after the block? */
    XIns *last = &ins[n - 1];
    unsigned live = L_ALL;
    switch (last->kind) {
    case K_JCC: live = xj_peek_live(last->target) | xj_peek_live(last->next); break;
    case K_JMP: case K_CALL: live = xj_peek_live(last->target); break;
    case K_FALL_END: case K_RET: case K_JMPI: case K_CALLI: live = L_ALL; break;
    default: live = xj_peek_live(last->next); break;
    }
    xj_analyse(ins, n, live);

    xj_protect_page(vpage);
    xj_w_enable(true);
    uint32_t *start = xj.next;
    t.e.p = start; t.ins = ins; t.n = n; t.nfix = 0;
    XE *e = &t.e;
    static uint32_t *lab[XJ_MAX_INS + 1];
    int nz = NZ_NONE;
    for (int i = 0; i < n; i++) {
        XIns *x = &ins[i];
        lab[i] = e->p;
        t.cur = i;
        if (x->kind == K_FALL || x->kind == K_FALL_END) {
            mov_w(e, 0, x->eip);
            mov_w(e, 2, x->next);
            bl_to(e, xj.interp_thunk);
            if (x->kind == K_FALL_END) b_to(e, xj.epi_plain);
            else {
                /* The runtime can be further away than a CBNZ reaches. */
                E(e, 0xB4000040u);                 /* cbz x0,+2 */
                b_to(e, xj.epi_plain);
            }
            nz = NZ_NONE;
        } else {
            gen_ins(&t, x, &nz, outs, &nouts);
        }
    }
    lab[n] = e->p;
    /* A block cut by the instruction limit, page end or HLE window goes on directly. */
    t.cur = n - 1;
    if (last->kind != K_FALL_END && last->kind != K_JCC && last->kind != K_JMP && last->kind != K_CALL &&
        last->kind != K_RET && last->kind != K_JMPI && last->kind != K_CALLI) {
        exit_direct(&t, outs, &nouts, last->next);
    }

    /* Interpreter stubs for every memory miss of the block */
    for (int i = 0; i < n; i++) {
        bool used = false;
        for (int f = 0; f < t.nfix; f++) if (t.fix[f].ins == i) { used = true; break; }
        if (!used) continue;
        XIns *x = &ins[i];
        uint32_t *stub = e->p;
        mov_w(e, 0, x->eip);
        mov_w(e, 2, x->next);
        bl_to(e, xj.interp_thunk);
        bool control = x->kind == K_JMP || x->kind == K_CALL || x->kind == K_RET || x->kind == K_JMPI ||
                       x->kind == K_CALLI || x->kind == K_JCC;
        bool setter = x->kind == K_ALU || x->kind == K_INCDEC || x->kind == K_SHIFT || x->kind == K_NEG || x->kind == K_IMUL;
        if (control || setter || i == n - 1) {
            /* Control left, or NZCV no longer match the lazy flags the interpreter set. */
            b_to(e, xj.epi_plain);
        } else {
            E(e, 0xB4000040u);                     /* cbz x0,+2 */
            b_to(e, xj.epi_plain);
            b_to(e, lab[i + 1]);
        }
        for (int f = 0; f < t.nfix; f++) {
            if (t.fix[f].ins != i) continue;
            uint32_t *s = t.fix[f].site;
            uint32_t d = (uint32_t)(stub - s);
            if (t.fix[f].form == 0) *s |= (d & 0x7FFFFu) << 5;
            else *s |= (d & 0x3FFFu) << 5;
        }
    }
    /* Exit stubs for direct branches */
    for (int i = 0; i < nouts; i++) {
        uint32_t *stub = e->p, *site = outs[i].site;
        *outs[i].over |= ((uint32_t)(stub - outs[i].over) & 0x3FFFu) << 5;
        mov_w(e, 9, outs[i].target);
        str_cpu(e, 9, offsetof(x86cpu, eip));
        /* adr x10,site */
        int64_t rel = (char *)site - (char *)e->p;
        E(e, 0x10000000u | ((uint32_t)(rel & 3) << 29) | (((uint32_t)(rel >> 2) & 0x7FFFFu) << 5) | 10u);
        b_to(e, xj.epi_link);
        *site = 0x14000000u | ((uint32_t)(stub - site) & 0x03FFFFFFu);
    }
    xj.next = e->p;
    xj_w_enable(false);
    xj_sync(start, (size_t)((char *)e->p - (char *)start));

    XBlock *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->eip = eip; b->n = (uint32_t)n; b->code = start;
    unsigned h = xj_hash(eip);
    b->hash_next = xj.hash[h]; xj.hash[h] = b;
    b->page_next = xj.page_blocks[vpage]; xj.page_blocks[vpage] = b;
    xj.blocks++;
    return b;
}
/* A linked site's stub is found again by decoding the branch that is still in it. */
static void xj_link(uint32_t *site, XBlock *b) {
    if (xj.nlinks == xj.caplinks) {
        size_t n = xj.caplinks ? xj.caplinks * 2 : 4096;
        XLink *l = realloc(xj.links, n * sizeof *l);
        if (!l) return;
        xj.links = l; xj.caplinks = n;
    }
    int32_t off = (int32_t)(*site << 6) >> 6;
    xj.links[xj.nlinks].site = site;
    xj.links[xj.nlinks].stub = site + off;
    xj.nlinks++;
    xj_w_enable(true);
    *site = 0x14000000u | ((uint32_t)(b->code - site) & 0x03FFFFFFu);
    xj_w_enable(false);
    xj_sync(site, 4);
}

/* QRX86_PROF=1: a sampling profile of the CPU thread on stderr, every 8000 samples
 * (about 8 s of CPU). `sample` and debuggers work too, this one also names x86 code. */
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#define XP_MAX 8000
static struct { uint64_t pc[XP_MAX]; volatile unsigned n; mach_port_t thread; } xp;
/* Signals would only arrive at system calls and blame those. This stops the thread. */
static void *xp_sampler(void *arg) {
    (void)arg;
    for (;;) {
        arm_thread_state64_t st;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        usleep(1000);
        if (xp.n >= XP_MAX) continue;
        if (thread_suspend(xp.thread) != KERN_SUCCESS) continue;
        if (thread_get_state(xp.thread, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS)
            xp.pc[xp.n++] = arm_thread_state64_get_pc(st);
        thread_resume(xp.thread);
    }
    return NULL;
}
static void xp_start(void) {
    pthread_t t;
    xp.thread = mach_thread_self();
    pthread_create(&t, NULL, xp_sampler, NULL);
}
typedef struct { const char *name; uint32_t eip; unsigned n; } XpRow;
static int xp_cmp(const void *a, const void *b) { return (int)((const XpRow *)b)->n - (int)((const XpRow *)a)->n; }
static void xp_dump(void) {
    static XpRow row[512];
    unsigned nrow = 0, jit = 0, total = xp.n;
    for (unsigned i = 0; i < total; i++) {
        uint32_t *pc = (uint32_t *)xp.pc[i];
        const char *name = NULL;
        uint32_t eip = 0;
        if (pc >= xj.arena && pc < xj.arena_blocks) name = "jit-runtime";
        else if (pc >= xj.arena && pc < xj.end) {
            XBlock *best = NULL;
            jit++;
            for (uint32_t h = 0; h < XJ_HASH; h++)
                for (XBlock *b = xj.hash[h]; b; b = b->hash_next)
                    if (b->code <= pc && (!best || b->code > best->code)) best = b;
            name = "x86";
            eip = best ? best->eip : 0;
        } else {
            Dl_info d;
            name = dladdr(pc, &d) && d.dli_sname ? d.dli_sname : "other";
        }
        unsigned r;
        for (r = 0; r < nrow; r++) if (row[r].eip == eip && !strcmp(row[r].name, name)) { row[r].n++; break; }
        if (r == nrow && nrow < 512) { row[nrow].name = name; row[nrow].eip = eip; row[nrow].n = 1; nrow++; }
    }
    qsort(row, nrow, sizeof row[0], xp_cmp);
    fprintf(stderr, "[QRX86-PROF] %u samples, %u%% in x86 blocks, interp %llu blocks %llu kills %llu\n", total, total ? jit * 100 / total : 0,
            (unsigned long long)xj.interp_ins, (unsigned long long)xj.blocks, (unsigned long long)xj.kills);
    for (unsigned r = 0; r < nrow && r < 28; r++) fprintf(stderr, "  %5.1f%% %s %08x\n", row[r].n * 100.0 / total, row[r].name, row[r].eip);
    {
        unsigned best = 0;
        for (unsigned i = 0; i < 1024; i++) if (xp_notify_page[i] > xp_notify_page[best]) best = i;
        fprintf(stderr, "  notify calls=%llu bytes=%llu top page%%1024=%u (%u)\n", (unsigned long long)xp_notify_calls,
                (unsigned long long)xp_notify_bytes, best, xp_notify_page[best]);
        memset(xp_notify_page, 0, sizeof xp_notify_page);
        xp_notify_calls = xp_notify_bytes = 0;
    }
    xp.n = 0;
}

/* Replaces x86_run for one door quantum. */
static int xj_run(x86cpu *c, uint32_t stop_eip) {
    jmp_buf jb, *old = c->jb;
    xj_enter_fn enter = (xj_enter_fn)xj.enter_code;
    if (c->fault || c->stop) return c->fault;
    c->jb = &jb;
    if (setjmp(jb)) {
        if ((int32_t)c->left < 0) c->left = 0;
        c->jb = old;
        return c->fault;
    }
    c->xj_site = NULL;
    {
        static int prof = -1;
        if (prof < 0) { prof = getenv("QRX86_PROF") != NULL; if (prof) xp_start(); }
        if (prof && xp.n >= XP_MAX) xp_dump();
    }
    while (c->eip != stop_eip) {
        if ((int32_t)c->left <= 0) {
            c->left = 0;
            FAULT(c, X86_FAULT_BUDGET, c->eip, "instruction budget exhausted");
        }
        uint32_t eip = c->eip;
        uint32_t *site = c->xj_site;
        c->xj_site = NULL;
        c->xj_abort = 0;
        XBlock *b = NULL;
        if (eip - c->hle_base >= c->hle_count) {
            b = xj_lookup(eip);
            if (!b) b = xj_translate(eip);
            /* Translating can flush the cache: the site is gone with it. */
            if (c->xj_abort) { site = NULL; c->xj_abort = 0; }
        }
        if (!b) {
            c->left--;
            xj.interp_ins++;
            exec_one(c);
            continue;
        }
        if (site) xj_link(site, b);
        else {
            XIbt *i = &xj.ibt[(eip ^ (eip >> 12)) & ((1u << XJ_IBT_BITS) - 1u)];
            i->eip = eip;
            i->code = b->code;
        }
        xj.enters++;
        enter(c, b->code, xj.rt);
    }
    if ((int32_t)c->left < 0) c->left = 0;
    c->jb = old;
    return c->fault;
}
static void xj_report(void) {
    if (xj.blocks) fprintf(stderr, "[QRX86-JIT] blocks=%llu enters=%llu interp=%llu kills=%llu volatile=%llu flushes=%llu code=%zuKB\n",
        (unsigned long long)xj.blocks, (unsigned long long)xj.enters, (unsigned long long)xj.interp_ins, (unsigned long long)xj.kills,
        (unsigned long long)xj.volatile_pages, (unsigned long long)xj.flushes, (size_t)((char *)xj.next - (char *)xj.arena) / 1024u);
}
#endif
