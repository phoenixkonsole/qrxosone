/* QRX OS One 22.5.11 - qrx-jit-selftest (opt-in, standalone)
 *
 * Validates, step by step, the m68k mechanics a JIT depends on, WITHOUT touching
 * QRX86GRTExec (whose native JIT stays quarantined) and without being called by
 * the boot sequence, QRXDesktop autostart or any ReactOS script.
 *
 *   qrx-jit-selftest            static stages only: byte emission, buffer
 *                               alignment, copy + CacheClearE/CacheClearU,
 *                               read-back.  NO generated code is executed.
 *   qrx-jit-selftest [LOGDIR=<dir>] [EXECUTE]
 *   qrx-jit-selftest EXECUTE    additionally executes four tiny generated
 *                               sequences (MOVEQ/RTS, MOVE.L/RTS, JSR helper,
 *                               self-modified MOVEQ after cache flush).
 *
 * An illegal instruction or address error in generated code is a CPU exception;
 * it cannot be caught by C code here and normally ends in an AROS Guru/crash
 * requester.  Therefore each native stage first writes T:QRX-JIT-SELFTEST.stage
 * ("<stage> PENDING"); after a crash that file names the stage that killed the
 * machine.  Run EXECUTE only in an emulator snapshot or on a machine you can
 * reset (see JIT-SELFTEST-22.5.11.txt).
 *
 * Exit: 0 every requested stage passed, 10 a stage failed, 20 setup error.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "qrx_jit_emit.h"

#define STAGE_NAME "QRX-JIT-SELFTEST.stage"
#define LOG_NAME   "QRX-JIT-SELFTEST.log"

static char g_stage_file[200] = "T:" STAGE_NAME;
static char g_log_file[200] = "T:" LOG_NAME;

static const char ver[] __attribute__((used)) = "$VER: qrx-jit-selftest 22.5.11 (28.10.2026) QRX OS One";

typedef LONG (*qrx_native_fn)(void);

static BPTR g_log;
static int g_fail;
static volatile LONG g_helper_calls;

static void both(const char *s)
{
    PutStr((CONST_STRPTR)s);
    if (g_log) FPuts(g_log, (CONST_STRPTR)s);
}
static void hex32(char *t, ULONG v)
{
    int i;
    for (i = 0; i < 8; i++) t[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    t[8] = 0;
}
static void both_hex(ULONG v) { char t[9]; hex32(t, v); both("0x"); both(t); }
static void both_dec(long v)
{
    char t[16]; int i = 15, neg = v < 0; unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
    t[i] = 0;
    do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (neg) t[--i] = '-';
    both(t + i);
}
static void result(const char *stage, int ok, const char *detail)
{
    both(ok ? "PASS " : "FAIL ");
    both(stage);
    if (detail && *detail) { both("  "); both(detail); }
    both("\n");
    if (!ok) g_fail = 1;
}
static void mark(const char *stage, const char *state)
{
    BPTR fh = Open((CONST_STRPTR)g_stage_file, MODE_NEWFILE);
    if (!fh) return;
    FPuts(fh, (CONST_STRPTR)stage);
    FPuts(fh, (CONST_STRPTR)" ");
    FPuts(fh, (CONST_STRPTR)state);
    FPuts(fh, (CONST_STRPTR)"\n");
    Close(fh);                       /* flushed to disk/RAM before the risky call */
}

/* C helper called from generated code through JSR abs.l. */
static LONG __attribute__((noinline, used)) qrx_jit_helper(void)
{
    g_helper_calls++;
    return 41;
}

static void flush(APTR p, ULONG n)
{
    CacheClearE(p, n, CACRF_ClearI | CACRF_ClearD);
}

static const unsigned char expect_static[] = {
    0x70, 0x2a, 0x4e, 0x75,                                /* moveq #42,d0 ; rts           */
    0x20, 0x3c, 0x51, 0x52, 0x53, 0x54, 0x4e, 0x75,        /* move.l #$51525354,d0 ; rts   */
    0x52, 0x80, 0x4e, 0x71                                 /* addq.l #1,d0 ; nop           */
};

int main(int argc, char **argv)
{
    int execute = 0, i;
    UWORD attn = SysBase->AttnFlags;
    ULONG cacr = CacheControl(0, 0);
    unsigned char tmp[32];
    struct qrx_emit e;
    UBYTE *code = NULL;
    const ULONG codesz = 64;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "EXECUTE") || !strcmp(argv[i], "execute") || !strcmp(argv[i], "--execute")) execute = 1;
        else if (!strncmp(argv[i], "LOGDIR=", 7) && strlen(argv[i] + 7) < 160) {
            /* put log + crash marker on a volume that survives the crash (host dir in FS-UAE) */
            const char *d = argv[i] + 7;
            size_t l = strlen(d);
            int sep = l && d[l - 1] != ':' && d[l - 1] != '/';
            strcpy(g_stage_file, d); if (sep) strcat(g_stage_file, "/"); strcat(g_stage_file, STAGE_NAME);
            strcpy(g_log_file, d); if (sep) strcat(g_log_file, "/"); strcat(g_log_file, LOG_NAME);
        } else {
            PutStr((CONST_STRPTR)"Usage: qrx-jit-selftest [LOGDIR=<dir>] [EXECUTE]\n");
            return RETURN_FAIL;
        }
    }
    g_log = Open((CONST_STRPTR)g_log_file, MODE_NEWFILE);

    both("QRX-JIT-SELFTEST 22.5.11 (opt-in; does NOT enable the QRX86 JIT)\n");
    both("CPU: ");
    both((attn & AFF_68060) ? "68060" : (attn & AFF_68040) ? "68040" : (attn & AFF_68030) ? "68030" :
         (attn & AFF_68020) ? "68020" : (attn & AFF_68010) ? "68010" : "68000");
    both((attn & (AFF_68881 | AFF_68882 | AFF_FPU40)) ? " +FPU" : "");
    both("  AttnFlags="); both_hex(attn); both("  CACR(CacheControl)="); both_hex(cacr);
    both((cacr & CACRF_CopyBack) ? " copyback" : ""); both("\n");

    /* S1: encoding into a plain buffer, compared with GNU-as-verified bytes. */
    qe_init(&e, tmp, sizeof(tmp));
    qe_moveq(&e, 42, 0); qe_rts(&e);
    qe_move_l_imm(&e, 0x51525354UL, 0); qe_rts(&e);
    qe_addq_l(&e, 1, 0); qe_nop(&e);
    result("S1 byte-emission", !e.overflow && e.len == sizeof(expect_static) && !memcmp(tmp, expect_static, e.len), "");

    /* S2: executable buffer allocation and alignment (68000/010 raise address errors on odd PCs). */
    code = (UBYTE *)AllocVec(codesz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!code) { result("S2 alloc", 0, "AllocVec failed"); goto out; }
    {
        char d[64];
        strcpy(d, "addr=0x"); hex32(d + 7, (ULONG)(IPTR)code);
        strcat(d, ((IPTR)code & 3) ? " (word aligned)" : " (longword aligned)");
        result("S2 alloc+alignment", ((IPTR)code & 1) == 0, d);
    }

    /* S3: write code, flush caches, read it back. */
    qe_init(&e, code, codesz);
    qe_moveq(&e, 42, 0); qe_rts(&e);                         /* +0  */
    qe_move_l_imm(&e, 0x51525354UL, 0); qe_rts(&e);          /* +4  */
    qe_jsr_abs(&e, (unsigned long)(IPTR)&qrx_jit_helper);    /* +12 */
    qe_addq_l(&e, 1, 0); qe_rts(&e);
    flush(code, (ULONG)e.len);
    CacheClearU();
    result("S3 copy+CacheClearE/U+readback", !e.overflow && !memcmp(code, expect_static, 12) &&
           code[12] == 0x4e && code[13] == 0xb9, "");

    if (!execute) {
        both("NOTE native execution NOT performed (static stages only). Re-run with EXECUTE inside\n"
             "     an emulator snapshot to execute generated code.\n");
        mark("static", "DONE");
        goto out;
    }
    if (g_fail) { both("ABORT static stages failed; generated code will not be executed\n"); goto out; }

    /* S4..S7 execute generated code. Each marks the stage first: a crash cannot be caught. */
    mark("S4 MOVEQ/RTS", "PENDING");
    {
        LONG r = ((qrx_native_fn)(void *)code)();
        result("S4 native MOVEQ #42,D0 ; RTS", r == 42, "");
        if (r != 42) { both("     got "); both_dec(r); both("\n"); }
    }
    mark("S5 MOVE.L/RTS", "PENDING");
    {
        LONG r = ((qrx_native_fn)(void *)(code + 4))();
        result("S5 native MOVE.L #$51525354,D0 ; RTS", (ULONG)r == 0x51525354UL, "");
    }
    mark("S6 JSR helper", "PENDING");
    {
        LONG r;
        g_helper_calls = 0;
        r = ((qrx_native_fn)(void *)(code + 12))();
        result("S6 native JSR abs.l C-helper ; ADDQ.L #1,D0 ; RTS", r == 42 && g_helper_calls == 1, "");
    }
    mark("S7 self-modify+flush", "PENDING");
    {
        LONG r;
        code[1] = 7;                     /* MOVEQ #42 -> MOVEQ #7 in place */
        flush(code, 4);
        r = ((qrx_native_fn)(void *)code)();
        result("S7 modified code visible after CacheClearE (I-cache coherence)", r == 7, "");
    }
    mark("S8 repeat x1000", "PENDING");
    {
        LONG sum = 0;
        for (i = 0; i < 1000; i++) sum += ((qrx_native_fn)(void *)(code + 12))();
        result("S8 1000 JSR-helper round trips", sum == 42000 && g_helper_calls == 1001, "");
    }
    mark("native", g_fail ? "FAILED" : "DONE");

out:
    if (code) FreeVec(code);
    both(g_fail ? "QRX-JIT-SELFTEST RESULT: FAIL\n" : (execute ? "QRX-JIT-SELFTEST RESULT: PASS (static + native)\n"
                                                              : "QRX-JIT-SELFTEST RESULT: PASS (static only)\n"));
    both("QRX86GRTExec native JIT remains quarantined regardless of this result.\n");
    if (g_log) Close(g_log);
    return g_fail ? RETURN_ERROR : RETURN_OK;
}
