/* QRX OS One 22.5.12 - qrx-win32: run 32-bit Windows GUI programs (PE/i386).
 *   qrx-win32 [-s SCRIPT] [-o OUTDIR] [-l LANGID] [-v] [-T] PROGRAM.EXE [ARGS...]
 * Without -s on AROS the program opens real windows (Intuition/RTG backend);
 * with -s (or on a host build) the headless backend runs the test script. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/qrx_bigstack.h"
#include "qrxwin.h"

static qwin g_w;

static void usage(void)
{
    fprintf(stderr, "usage: qrx-win32 [-s SCRIPT] [-o OUTDIR] [-l LANGID] [-L LOGFILE] [-e VAR=VALUE] [-G FILE N] [-v] [-T] PROGRAM.EXE [ARGS...]\n"
                    "  Runs a 32-bit Windows GUI program with the QRX Win32 HLE subsystem.\n"
                    "  No Windows/ReactOS DLLs are used; x86 code runs in the qrx86core interpreter.\n");
}

static int real_main(int argc, char **argv)
{
    qwin *w = &g_w;
    const char *script = NULL;
    int i = 1, rc;
    uint64_t t0;
    memset(w, 0, sizeof(*w));
    w->lang = 0x0407;
    w->log = stderr;
    snprintf(w->outdir, sizeof(w->outdir), ".");
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) snprintf(w->outdir, sizeof(w->outdir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) w->lang = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-v")) w->verbose = 1;
        else if (!strcmp(argv[i], "-vv")) w->verbose = 2;
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) { FILE *lf = fopen(argv[++i], "w"); if (lf) w->log = lf; }
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) { char kv[300], *eq; snprintf(kv, sizeof(kv), "%s", argv[++i]); eq = strchr(kv, '='); if (eq) { *eq = 0; qw_env_set(kv, eq + 1); } }
        else if (!strcmp(argv[i], "-G") && i + 2 < argc) { snprintf(w->grab_path, sizeof(w->grab_path), "%s", argv[i + 1]); w->grab_at = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "-T")) w->trace = 1;
        else { usage(); return 2; }
        i++;
    }
    if (i >= argc) { usage(); return 2; }
#ifdef __AROS__
    w->be = script ? &qw_headless : &qw_aros;
#else
    w->be = &qw_headless;
#endif
    if (getenv("QRX86_JIT") && strcmp(getenv("QRX86_JIT"), "off") &&
        strcmp(getenv("QRX86_JIT"), "experimental")) {
        fprintf(stderr,"qrx-win32: QRX86_JIT must be off or experimental\n");return 2;
    }
    if (script && !headless_set_script(w, script)) { fprintf(stderr, "qrx-win32: cannot open script %s\n", script); return 2; }
    if (!w->be->init(w)) { fprintf(stderr, "qrx-win32: backend %s unavailable\n", w->be->name); return 20; }
    if (!qw_load(w, argv[i], argc - i, argv + i)) return 127;
    if (getenv("QRX_WIN_BUDGET")) w->cpu.budget = strtoull(getenv("QRX_WIN_BUDGET"), NULL, 0);   /* profiling: stop after N instructions */
    if (getenv("QRX86_JIT") && strcmp(getenv("QRX86_JIT"), "off")) {
        unsigned profile = 0;
        const char *p = getenv("QRX86_JIT_CPU");
        if (p && strcmp(p,"auto")) {
            char *end; unsigned long n = strtoul(p,&end,0);
            profile = (end==p || *end || (n!=0 && n!=20 && n!=30 && n!=40 && n!=60)) ? 99u : (unsigned)n;
        }
        if (strcmp(getenv("QRX86_JIT"), "experimental")) {
            fprintf(stderr, "qrx-win32: QRX86_JIT must be off or experimental\n"); return 2;
        }
        fprintf(stderr, "qrx-win32: experimental m68k JIT %s (default remains interpreter)\n",
                x86_jit_enable(&w->cpu, profile, getenv("QRX86_JIT_TIMING") != NULL) ? "enabled" : "unavailable; interpreter fallback");
    }
    t0 = w->be->now(w);
    rc = qw_run(w);
    if (getenv("QRX_WIN_STATS")) {      /* backend clock: real time on AROS, virtual time headless */
        uint64_t ms = w->be->now(w) - t0, n = x86_icount(&w->cpu);
        char m[200];
        snprintf(m, sizeof(m), "%llu guest instructions in %llu ms (%llu per second), %d DirectDraw frames, %u HLE imports, %llu API transitions",
                 (unsigned long long)n, (unsigned long long)ms, (unsigned long long)(ms ? n * 1000u / ms : 0), w->frames, w->nthunk, (unsigned long long)w->api_calls);
        fprintf(stderr, "qrx-win32: %s\n", m);
        qw_log(w, "%s", m);
    }
    if (w->cpu.jit) {
        x86jit_stats s; x86_jit_snapshot(&w->cpu, &s);
        char line[768];
        snprintf(line, sizeof(line), "QRX86-JIT profile=680%u native_blocks=%llu native_insns=%llu fallback=%llu compile=%llu hits=%llu misses=%llu invalidations=%llu evictions=%llu code_bytes=%llu live_code_bytes=%llu peak_code_bytes=%llu compile_ticks=%llu native_ticks=%llu interpreter_ticks=%llu ticks_per_second=%lu timing=%d cache_profile=%d clock_invalid=%llu\n",
            s.profile, (unsigned long long)s.native_blocks, (unsigned long long)s.native_instructions,
            (unsigned long long)s.fallback_instructions, (unsigned long long)s.compilations,
            (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.invalidations,
            (unsigned long long)s.evictions, (unsigned long long)s.code_bytes,
            (unsigned long long)s.live_code_bytes, (unsigned long long)s.peak_code_bytes,
            (unsigned long long)s.compile_ticks, (unsigned long long)s.native_ticks,
            (unsigned long long)s.interpreter_ticks, (unsigned long)CLOCKS_PER_SEC, s.timing,s.cache_profile,(unsigned long long)s.clock_invalid);
        fputs(line,stderr); qw_log(w,"%s",line);
        x86_jit_disable(&w->cpu);
    }
    if (w->be->shutdown) w->be->shutdown(w);
    if (w->expect_ok || w->expect_fail) printf("QRXWIN-RESULT expect_ok=%d expect_fail=%d exit=%d\n", w->expect_ok, w->expect_fail, rc);
    if (w->expect_fail) return 1;
    return rc;
}

QRX_BIGSTACK_MAIN(real_main)
