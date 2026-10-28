/* QRX 22.5.11 target test program, built as a real m68k HUNK executable.
 *   QRXHunkTest [RC=n] [args...]  prints its arguments, returns n (default 0)
 * Built with -DQRX_NEEDLIB it also requires "qrxnonexistent.library" and
 * returns 20 when that library cannot be opened (missing-library test). */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

static void num(long v)
{
    char t[16]; int i = 15; unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    t[i] = 0;
    do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (v < 0) t[--i] = '-';
    PutStr((CONST_STRPTR)(t + i));
}

int main(int argc, char **argv)
{
    int i;
    long rc = 0;
    PutStr((CONST_STRPTR)"QRXHUNKTEST argc="); num(argc - 1); PutStr((CONST_STRPTR)"\n");
    for (i = 1; i < argc; i++) {
        PutStr((CONST_STRPTR)"QRXHUNKTEST arg["); num(i); PutStr((CONST_STRPTR)"]=<");
        PutStr((CONST_STRPTR)argv[i]); PutStr((CONST_STRPTR)">\n");
        if (!strncmp(argv[i], "RC=", 3)) {
            const char *p = argv[i] + 3;
            rc = 0;
            while (*p >= '0' && *p <= '9') rc = rc * 10 + (*p++ - '0');
        }
    }
#ifdef QRX_NEEDLIB
    {
        struct Library *b = OpenLibrary((CONST_STRPTR)"qrxnonexistent.library", 0);
        if (!b) { PutStr((CONST_STRPTR)"QRXHUNKTEST cannot open qrxnonexistent.library\n"); return 20; }
        CloseLibrary(b);
    }
#endif
    PutStr((CONST_STRPTR)"QRXHUNKTEST rc="); num(rc); PutStr((CONST_STRPTR)"\n");
    return (int)rc;
}
