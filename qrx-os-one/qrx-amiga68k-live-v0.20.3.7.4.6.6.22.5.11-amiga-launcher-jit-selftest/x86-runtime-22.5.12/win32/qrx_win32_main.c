/* QRX OS One 22.5.12 - qrx-win32: run 32-bit Windows GUI programs (PE/i386).
 *   qrx-win32 [-s SCRIPT] [-o OUTDIR] [-l LANGID] [-v] [-T] PROGRAM.EXE [ARGS...]
 * Without -s on AROS the program opens real windows (Intuition/RTG backend);
 * with -s (or on a host build) the headless backend runs the test script. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/qrx_bigstack.h"
#include "qrxwin.h"

static qwin g_w;

static void usage(void)
{
    fprintf(stderr, "usage: qrx-win32 [-s SCRIPT] [-o OUTDIR] [-l LANGID] [-L LOGFILE] [-v] [-T] PROGRAM.EXE [ARGS...]\n"
                    "  Runs a 32-bit Windows GUI program with the QRX Win32 HLE subsystem.\n"
                    "  No Windows/ReactOS DLLs are used; x86 code runs in the qrx86core interpreter.\n");
}

static int real_main(int argc, char **argv)
{
    qwin *w = &g_w;
    const char *script = NULL;
    int i = 1, rc;
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
    if (script && !headless_set_script(w, script)) { fprintf(stderr, "qrx-win32: cannot open script %s\n", script); return 2; }
    if (!w->be->init(w)) { fprintf(stderr, "qrx-win32: backend %s unavailable\n", w->be->name); return 20; }
    if (!qw_load(w, argv[i], argc - i, argv + i)) return 127;
    rc = qw_run(w);
    if (w->be->shutdown) w->be->shutdown(w);
    if (getenv("QRX_WIN_STATS")) fprintf(stderr, "qrx-win32: %llu guest instructions, %u HLE imports\n", (unsigned long long)w->cpu.icount, w->nthunk);
    if (w->expect_ok || w->expect_fail) printf("QRXWIN-RESULT expect_ok=%d expect_fail=%d exit=%d\n", w->expect_ok, w->expect_fail, rc);
    if (w->expect_fail) return 1;
    return rc;
}

QRX_BIGSTACK_MAIN(real_main)
